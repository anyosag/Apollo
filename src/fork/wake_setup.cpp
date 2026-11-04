/**
 * @file src/fork/wake_setup.cpp
 * @brief Turning this PC on from off with LuzLunar's Wake PC: the network adapter's driver
 * settings and Fast Startup, read from and written to the registry, the journal that undoes a fix,
 * and how the PC was last powered on (wake_setup_policy.h).
 *
 * The adapter is the one with the address the client reached, so its MAC is the one /serverinfo
 * gave LuzLunar. Its settings are the driver's Advanced tab: Ndi\params under its key in the
 * network class, with the values in the key itself. A fix restarts the adapter, as Device Manager
 * does after a change there (DIF_PROPERTYCHANGE with DICS_PROPCHANGE).
 */
// standard includes
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "fork_config.h"
#include "host_sleep_policy.h"
#include "src/crypto.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/rtsp.h"
#include "src/utility.h"
#include "updater.h"
#include "wake_setup.h"
#include "wake_setup_policy.h"

#ifdef _WIN32
  // platform includes
  // clang-format off
  #include <winsock2.h>
  #include <windows.h>
  #include <iphlpapi.h>
  #include <setupapi.h>
  #include <powrprof.h>
  // clang-format on

  #include "src/platform/windows/misc.h"
  #include "src/platform/windows/nvprefs/undo_file.h"
#endif

using namespace std::literals;

namespace fork_tweaks::wake_setup {
  namespace policy = wake_setup_policy;

  namespace {
    reply_t refusal(int status, std::string_view error) {
      return {status, host_sleep_policy::refusal_json(error)};
    }
  }  // namespace

#ifdef _WIN32
  namespace {
    /// The network adapters' setup class, GUID_DEVCLASS_NET
    constexpr GUID net_class_guid {0x4d36e972, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    constexpr std::wstring_view net_class = L"{4d36e972-e325-11ce-bfc1-08002be10318}";
    constexpr std::wstring_view class_key = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";
    constexpr const wchar_t *power_key = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power";
    constexpr const wchar_t *policy_key = L"SOFTWARE\\Policies\\Microsoft\\Windows\\System";
    constexpr const wchar_t *hiberboot_value = L"HiberbootEnabled";

    /// The raw SMBIOS provider's signature for GetSystemFirmwareTable(), 'RSMB'
    constexpr DWORD rsmb = 0x52534D42;

    std::mutex state_mutex;
    host_sleep_policy::tokens_t tokens;  ///< Separate from Sleep's, so the two never use each other's
    std::optional<policy::result_t> last_result;

    /// A fix or undo is being carried out
    std::atomic<bool> applying {false};

    /// How the PC was last powered on, read once at startup
    std::atomic<policy::power_on_e> power_on {policy::power_on_e::unknown};

    bool streaming() {
      return !rtsp_stream::get_all_session_uuids().empty();
    }

    std::string mac_text(const BYTE *address, ULONG length) {
      static constexpr char hex[] = "0123456789abcdef";
      std::string out;
      for (ULONG i = 0; i < length; i++) {
        if (i > 0) {
          out += ':';
        }
        out += hex[address[i] >> 4];
        out += hex[address[i] & 0xF];
      }
      return out;
    }

    /// A REG_SZ (or a REG_DWORD, as text); none when it isn't there
    std::optional<std::wstring> read_string(HKEY root, const std::wstring &key, const wchar_t *name) {
      DWORD type = 0;
      DWORD size = 0;
      if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_DWORD, &type, nullptr, &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      if (type == REG_DWORD) {
        DWORD value = 0;
        size = sizeof(value);
        if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
          return std::nullopt;
        }
        return std::to_wstring(value);
      }
      std::wstring text(size / sizeof(wchar_t) + 1, L'\0');
      size = (DWORD) (text.size() * sizeof(wchar_t));
      if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, text.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      text.resize(wcsnlen(text.c_str(), text.size()));
      return text;
    }

    std::optional<DWORD> read_dword(const wchar_t *key, const wchar_t *name) {
      DWORD value = 0;
      DWORD size = sizeof(value);
      if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      return value;
    }

    /// A registry key's subkeys
    std::vector<std::wstring> subkeys(const std::wstring &key) {
      std::vector<std::wstring> out;
      HKEY handle = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) {
        return out;
      }
      auto close = util::fail_guard([&]() {
        RegCloseKey(handle);
      });
      for (DWORD i = 0;; i++) {
        wchar_t name[256];
        DWORD length = _countof(name);
        auto status = RegEnumKeyExW(handle, i, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
          break;
        }
        if (status == ERROR_SUCCESS) {
          out.emplace_back(name, length);
        }
      }
      return out;
    }

    /// An enum's options: the values of its "enum" subkey, each named for its value
    std::vector<policy::option_t> options(const std::wstring &key) {
      std::vector<policy::option_t> out;
      HKEY handle = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (key + L"\\enum").c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) {
        return out;
      }
      auto close = util::fail_guard([&]() {
        RegCloseKey(handle);
      });
      for (DWORD i = 0;; i++) {
        wchar_t name[256];
        DWORD name_length = _countof(name);
        wchar_t data[512];
        DWORD data_size = sizeof(data) - sizeof(wchar_t);
        DWORD type = 0;
        auto status = RegEnumValueW(handle, i, name, &name_length, nullptr, &type, (LPBYTE) data, &data_size);
        if (status == ERROR_NO_MORE_ITEMS) {
          break;
        }
        if (status != ERROR_SUCCESS || type != REG_SZ) {
          continue;
        }
        data[data_size / sizeof(wchar_t)] = L'\0';
        out.push_back({platf::to_utf8(std::wstring_view(name, name_length)), platf::to_utf8(data)});
      }
      return out;
    }

    /// A setting's name; some drivers keep it as an indirect string (@file,-id)
    std::string param_desc(const std::wstring &key) {
      auto desc = read_string(HKEY_LOCAL_MACHINE, key, L"ParamDesc");
      if (!desc) {
        return {};
      }
      if (!desc->empty() && (*desc)[0] == L'@') {
        HKEY handle = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &handle) == ERROR_SUCCESS) {
          wchar_t text[512];
          if (RegLoadMUIStringW(handle, L"ParamDesc", text, sizeof(text), nullptr, 0, nullptr) == ERROR_SUCCESS) {
            desc = text;
          }
          RegCloseKey(handle);
        }
      }
      return platf::to_utf8(*desc);
    }

    std::wstring driver_path(const std::string &driver_key) {
      return std::wstring {class_key} + L"\\" + platf::from_utf8(driver_key);
    }

    /// The driver's key for an adapter: the subkey of the network class whose NetCfgInstanceId is its GUID
    std::optional<std::string> find_driver_key(const std::string &guid) {
      auto wanted = platf::from_utf8(guid);
      for (const auto &subkey : subkeys(std::wstring {class_key})) {
        auto id = read_string(HKEY_LOCAL_MACHINE, std::wstring {class_key} + L"\\" + subkey, L"NetCfgInstanceId");
        if (id && _wcsicmp(id->c_str(), wanted.c_str()) == 0) {
          return platf::to_utf8(subkey);
        }
      }
      return std::nullopt;
    }

    /// The driver's settings, as its Advanced tab lists them
    std::vector<policy::param_t> read_params(const std::string &driver_key) {
      std::vector<policy::param_t> out;
      auto driver = driver_path(driver_key);
      auto params = driver + L"\\Ndi\\params";
      for (const auto &keyword : subkeys(params)) {
        auto key = params + L"\\" + keyword;
        policy::param_t param;
        param.keyword = platf::to_utf8(keyword);
        param.desc = param_desc(key);
        param.options = options(key);
        auto value = read_string(HKEY_LOCAL_MACHINE, driver, keyword.c_str());
        param.set = value.has_value();
        if (!value) {
          value = read_string(HKEY_LOCAL_MACHINE, key, L"default");
        }
        param.value = value ? platf::to_utf8(*value) : std::string {};
        out.push_back(std::move(param));
      }
      return out;
    }

    policy::link_e link_of(const IP_ADAPTER_ADDRESSES &adapter, const std::string &description) {
      if (adapter.IfType == IF_TYPE_IEEE80211) {
        return policy::link_e::wifi;
      }
      if (adapter.IfType == IF_TYPE_PROP_VIRTUAL || adapter.IfType == IF_TYPE_TUNNEL || policy::is_virtual(description)) {
        return policy::link_e::virtual_adapter;
      }
      if (adapter.IfType == IF_TYPE_ETHERNET_CSMACD) {
        return policy::link_e::ethernet;
      }
      return policy::link_e::other;
    }

    /// Logs an adapter's settings the first time it's seen, so an unknown driver can be added later
    void log_params(const policy::adapter_t &adapter) {
      static std::mutex mutex;
      static std::set<std::string> logged;
      std::lock_guard lock {mutex};
      if (!logged.insert(adapter.guid).second) {
        return;
      }
      BOOST_LOG(info) << "fork: Wake: LuzLunar reaches this PC through ["sv << adapter.name << "] ("sv << adapter.description << ", "sv
                      << policy::key(adapter.link) << ", driver key "sv << adapter.driver_key << ')';
      for (const auto &param : adapter.params) {
        std::string options;
        for (const auto &option : param.options) {
          options += (options.empty() ? ""s : ", "s) + option.value + '=' + option.desc;
        }
        BOOST_LOG(debug) << "fork: Wake:   "sv << param.keyword << " ("sv << param.desc << ") = "sv << param.value << (param.set ? ""sv : " (default)"sv)
                         << (options.empty() ? ""s : " ["s + options + ']');
      }
    }

    /**
     * @brief The adapter with the address the client reached; when none has it (a VPN, or the
     * adapter restarting), the one with that GUID, and else the one with the default route.
     */
    policy::adapter_t find_adapter(const std::string &address, const std::string &guid_hint = {}) {
      policy::adapter_t found;
      ULONG size = 16 * 1024;
      std::vector<uint8_t> buffer;
      ULONG status = ERROR_BUFFER_OVERFLOW;
      for (int tries = 0; tries < 4 && status == ERROR_BUFFER_OVERFLOW; tries++) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, (PIP_ADAPTER_ADDRESSES) buffer.data(), &size);
      }
      if (status != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "fork: Wake: couldn't list the network adapters: "sv << status;
        return found;
      }

      auto has_address = [](const IP_ADAPTER_ADDRESSES &adapter, const std::string &ip) {
        for (auto unicast = adapter.FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
          if (platf::from_sockaddr(unicast->Address.lpSockaddr) == ip) {
            return true;
          }
        }
        return false;
      };

      auto first = (PIP_ADAPTER_ADDRESSES) buffer.data();
      const IP_ADAPTER_ADDRESSES *match = nullptr;
      for (auto adapter = first; adapter != nullptr && !match && !address.empty(); adapter = adapter->Next) {
        if (adapter->PhysicalAddressLength != 0 && has_address(*adapter, address)) {
          match = adapter;
        }
      }
      for (auto adapter = first; adapter != nullptr && !match && !guid_hint.empty(); adapter = adapter->Next) {
        if (adapter->AdapterName && _stricmp(adapter->AdapterName, guid_hint.c_str()) == 0) {
          match = adapter;
        }
      }
      if (!match) {
        auto gateway_ip = platf::get_local_ip_for_gateway();
        for (auto adapter = first; adapter != nullptr && !match && !gateway_ip.empty(); adapter = adapter->Next) {
          if (adapter->PhysicalAddressLength != 0 && has_address(*adapter, gateway_ip)) {
            match = adapter;
          }
        }
      }
      if (!match) {
        return found;
      }

      found.name = match->FriendlyName ? platf::to_utf8(match->FriendlyName) : std::string {};
      found.description = match->Description ? platf::to_utf8(match->Description) : std::string {};
      found.mac = mac_text(match->PhysicalAddress, match->PhysicalAddressLength);
      found.guid = match->AdapterName ? match->AdapterName : "";
      found.link = link_of(*match, found.description);
      if (auto key = find_driver_key(found.guid)) {
        found.driver_key = *key;
        found.params = read_params(*key);
        auto network_address = read_string(HKEY_LOCAL_MACHINE, driver_path(*key), L"NetworkAddress");
        found.mac_override = network_address && !network_address->empty();
      }
      log_params(found);
      return found;
    }

    policy::fast_startup_t read_fast_startup() {
      policy::fast_startup_t fast;
      SYSTEM_POWER_CAPABILITIES capabilities {};
      if (GetPwrCapabilities(&capabilities)) {
        fast.hibernation = capabilities.HiberFilePresent;
      }
      fast.value = read_dword(power_key, hiberboot_value);
      fast.policy = read_dword(policy_key, hiberboot_value);
      return fast;
    }

    policy::facts_t gather(const std::string &address, const std::string &guid_hint = {}) {
      policy::facts_t facts;
      facts.adapter = find_adapter(address, guid_hint);
      facts.fast_startup = read_fast_startup();
      facts.last_power_on = power_on;
      facts.uptime_s = GetTickCount64() / 1000;
      facts.streaming = streaming();
      facts.installing = updater::installing();
      facts.applying = applying;
      return facts;
    }

    std::optional<std::filesystem::path> journal_folder() {
      wchar_t program_data[MAX_PATH];
      auto length = GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH);
      if (length == 0 || length >= MAX_PATH) {
        return std::nullopt;
      }
      // With nvprefs' undo file and the NVIDIA journal
      return std::filesystem::path(program_data) / L"Sunshine";
    }

    std::optional<std::filesystem::path> journal_path() {
      auto folder = journal_folder();
      return folder ? std::optional(*folder / L"fork_wake_setup.json") : std::nullopt;
    }

    /// The journal, or an empty one; a file that isn't one is logged and left alone
    std::optional<policy::journal_t> read_journal() {
      auto path = journal_path();
      std::error_code ec;
      if (!path || !std::filesystem::is_regular_file(*path, ec)) {
        return policy::journal_t {};
      }
      std::ifstream in(*path, std::ios::binary);
      std::stringstream text;
      text << in.rdbuf();
      auto journal = policy::from_json(text.str());
      if (!journal) {
        BOOST_LOG(warning) << "fork: Wake: "sv << path->string() << " isn't a journal this Apollo can read; nothing it lists is undone"sv;
      }
      return journal;
    }

    /// Writes the journal in full or not at all, or deletes it when it's empty
    bool write_journal(const policy::journal_t &journal) {
      auto folder = journal_folder();
      // SYSTEM and Administrators only: the journal says what SYSTEM writes back
      if (!folder || !nvprefs::prepare_undo_folder(*folder)) {
        BOOST_LOG(warning) << "fork: Wake: no folder for the journal in %ProgramData%"sv;
        return false;
      }
      auto path = *folder / L"fork_wake_setup.json";
      std::error_code ec;
      if (journal.changes.empty()) {
        std::filesystem::remove(path, ec);
        return !ec;
      }
      auto temp = *folder / L"fork_wake_setup.json.new";
      {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << policy::to_json(journal);
        out.flush();
        if (!out.good()) {
          BOOST_LOG(warning) << "fork: Wake: couldn't write the journal"sv;
          return false;
        }
      }
      std::filesystem::rename(temp, path, ec);
      if (ec) {
        BOOST_LOG(warning) << "fork: Wake: couldn't write the journal: "sv << ec.message();
        return false;
      }
      return true;
    }

    std::string shown(const std::optional<std::string> &value) {
      return value ? *value : "unset"s;
    }

    /// Writes a change's value, or deletes it for none
    LSTATUS write(const policy::change_t &change, const std::optional<std::string> &value) {
      std::wstring key = change.kind == policy::change_t::kind_e::hiberboot ? std::wstring {power_key} : driver_path(change.driver_key);
      auto name = platf::from_utf8(change.keyword);
      if (!value) {
        auto status = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), name.c_str());
        return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
      }
      if (change.kind == policy::change_t::kind_e::hiberboot) {
        DWORD data = *value == "0" ? 0 : 1;
        return RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), name.c_str(), REG_DWORD, &data, sizeof(data));
      }
      // As Device Manager writes them
      auto text = platf::from_utf8(*value);
      return RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), name.c_str(), REG_SZ, text.c_str(), (DWORD) ((text.size() + 1) * sizeof(wchar_t)));
    }

    /// What a change's value is now, read back
    std::optional<std::string> read_back(const policy::change_t &change) {
      if (change.kind == policy::change_t::kind_e::hiberboot) {
        auto value = read_dword(power_key, hiberboot_value);
        return value ? std::optional(std::to_string(*value)) : std::nullopt;
      }
      auto value = read_string(HKEY_LOCAL_MACHINE, driver_path(change.driver_key), platf::from_utf8(change.keyword).c_str());
      return value ? std::optional(platf::to_utf8(*value)) : std::nullopt;
    }

    /// Stops and starts the adapter, so its driver reads its settings again
    bool restart_adapter(const std::string &driver_key, bool &restart_needed) {
      HDEVINFO devices = SetupDiGetClassDevsW(&net_class_guid, nullptr, nullptr, DIGCF_PRESENT);
      if (devices == INVALID_HANDLE_VALUE) {
        return false;
      }
      auto destroy = util::fail_guard([&]() {
        SetupDiDestroyDeviceInfoList(devices);
      });
      auto wanted = std::wstring {net_class} + L"\\" + platf::from_utf8(driver_key);
      SP_DEVINFO_DATA device {};
      device.cbSize = sizeof(device);
      for (DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &device); i++) {
        wchar_t driver[256] {};
        DWORD type = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_DRIVER, &type, (PBYTE) driver, sizeof(driver) - sizeof(wchar_t), nullptr) ||
            _wcsicmp(driver, wanted.c_str()) != 0) {
          continue;
        }
        SP_PROPCHANGE_PARAMS params {};
        params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        params.StateChange = DICS_PROPCHANGE;
        params.Scope = DICS_FLAG_GLOBAL;
        params.HwProfile = 0;
        if (!SetupDiSetClassInstallParamsW(devices, &device, &params.ClassInstallHeader, sizeof(params)) ||
            !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devices, &device)) {
          return false;
        }
        SP_DEVINSTALL_PARAMS_W install {};
        install.cbSize = sizeof(install);
        if (SetupDiGetDeviceInstallParamsW(devices, &device, &install)) {
          restart_needed = (install.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT)) != 0;
        }
        return true;
      }
      SetLastError(ERROR_NOT_FOUND);
      return false;
    }

    std::string error_key(LSTATUS status) {
      return status == ERROR_ACCESS_DENIED ? "access_denied"s : "error "s + std::to_string(status);
    }

    std::string setting_name(const policy::facts_t &facts, const policy::change_t &change) {
      if (change.kind == policy::change_t::kind_e::hiberboot) {
        return "Fast Startup"s;
      }
      auto param = policy::find_param(facts.adapter, change.keyword);
      return param && !param->desc.empty() ? param->desc + " ("s + change.keyword + ')' : change.keyword;
    }

    /// Writes the changes; restarts the adapter when one of its settings changed
    void write_all(const policy::facts_t &facts, const std::vector<std::pair<policy::change_t, std::optional<std::string>>> &writes, policy::result_t &result, std::vector<policy::change_t> &failed) {
      std::string restart_key;
      for (const auto &[change, value] : writes) {
        auto before = read_back(change);
        auto status = write(change, value);
        if (status != ERROR_SUCCESS || read_back(change) != value) {
          BOOST_LOG(warning) << "fork: Wake: couldn't set "sv << setting_name(facts, change) << " to "sv << shown(value) << " ("sv << error_key(status) << ')';
          result.failed.push_back(change.keyword);
          if (status != ERROR_SUCCESS && result.error.empty()) {
            result.error = error_key(status);
          }
          failed.push_back(change);
          continue;
        }
        result.changed++;
        if (change.kind == policy::change_t::kind_e::hiberboot) {
          BOOST_LOG(info) << "fork: Wake: Fast Startup "sv << (value == "0"s ? "turned off"sv : value ? "turned on"sv : "back to Windows' default"sv);
        } else {
          BOOST_LOG(info) << "fork: Wake: ["sv << facts.adapter.name << "] "sv << setting_name(facts, change) << ": "sv << shown(before) << " -> "sv << shown(value);
          restart_key = change.driver_key;
        }
      }
      if (!restart_key.empty()) {
        BOOST_LOG(info) << "fork: Wake: restarting ["sv << facts.adapter.name << "] so its driver reads the change"sv;
        if (!restart_adapter(restart_key, result.restart_needed)) {
          BOOST_LOG(warning) << "fork: Wake: couldn't restart ["sv << facts.adapter.name << "]: "sv << GetLastError() << "; the change applies after a restart"sv;
          result.restart_needed = true;
        } else if (result.restart_needed) {
          BOOST_LOG(info) << "fork: Wake: Windows says the change applies after a restart"sv;
        }
      }
    }

    void fix(const policy::facts_t &facts, policy::result_t &result) {
      auto changes = policy::plan(facts);
      if (changes.empty()) {
        result.ok = true;
        return;
      }
      auto journal = read_journal();
      if (!journal) {
        result.error = "journal";
        return;
      }
      // What each setting was goes first: no change without a way back
      if (!write_journal(policy::merge(*journal, changes))) {
        result.error = "journal";
        return;
      }
      std::vector<std::pair<policy::change_t, std::optional<std::string>>> writes;
      for (const auto &change : changes) {
        writes.emplace_back(change, change.after);
      }
      std::vector<policy::change_t> failed;
      write_all(facts, writes, result, failed);
      result.ok = failed.empty();
    }

    void undo(const policy::facts_t &facts, policy::result_t &result) {
      auto journal = read_journal();
      if (!journal) {
        result.error = "journal";
        return;
      }
      auto steps = policy::undo_plan(*journal, facts);
      for (const auto &change : steps.skipped) {
        BOOST_LOG(info) << "fork: Wake: not undoing "sv << setting_name(facts, change) << ": it changed since, or isn't a setting a fix changes"sv;
      }
      std::vector<std::pair<policy::change_t, std::optional<std::string>>> writes;
      for (const auto &step : steps.steps) {
        writes.emplace_back(step.change, step.write);
      }
      std::vector<policy::change_t> failed;
      write_all(facts, writes, result, failed);

      // What's left: another adapter's changes, and the writes that failed, for another try
      policy::journal_t left;
      left.changes = steps.kept;
      left.changes.insert(left.changes.end(), failed.begin(), failed.end());
      write_journal(left);
      if (!steps.steps.empty() || !failed.empty()) {
        BOOST_LOG(info) << "fork: Wake: undone "sv << result.changed << " of "sv << steps.steps.size() << " change(s)"sv;
      }
      result.ok = failed.empty();
    }

    /// Carries out a fix or undo once the answer is out, unless a stream or an update started meanwhile
    void apply_soon(policy::action_e action, std::string address, std::string guid, std::string client) {
      std::thread([action, address = std::move(address), guid = std::move(guid), client = std::move(client)]() {
        std::this_thread::sleep_for(policy::settle);
        policy::result_t result;
        result.action = action == policy::action_e::fix ? "fix" : "undo";
        if (streaming() || updater::installing()) {
          result.error = streaming() ? "streaming" : "updating";
          BOOST_LOG(info) << "fork: Wake: not changing anything after all for client ["sv << client << "]: "sv << result.error;
        } else {
          BOOST_LOG(info) << "fork: Wake: "sv << (action == policy::action_e::fix ? "fixing"sv : "undoing"sv) << " the wake settings for client ["sv << client << ']';
          auto facts = gather(address, guid);
          if (action == policy::action_e::fix) {
            fix(facts, result);
          } else {
            undo(facts, result);
          }
        }
        {
          std::lock_guard lock {state_mutex};
          last_result = result;
        }
        applying = false;
      }).detach();
    }

    std::string new_token() {
      return crypto::rand_alphabet(32, "0123456789abcdef"sv);
    }
  }  // namespace

  bool supported() {
    return client_wake_setup;
  }

  void start() {
    auto size = GetSystemFirmwareTable(rsmb, 0, nullptr, 0);
    policy::power_on_e read = policy::power_on_e::unknown;
    if (size > 0) {
      std::vector<uint8_t> table(size);
      auto written = GetSystemFirmwareTable(rsmb, 0, table.data(), size);
      if (written > 0 && written <= size) {
        read = policy::parse_wake_up_type(table.data(), written);
      }
    }
    power_on = read;
    BOOST_LOG(info) << "fork: Wake: this PC was turned on by "sv << policy::describe(read) << ", "sv << GetTickCount64() / 60000 << " min ago"sv;
  }

  reply_t handle(const client_t &client, const std::string &local_address, const std::string &method, const std::string &body) {
    if (!client.permitted) {
      BOOST_LOG(debug) << "Permission Sleep and Shut Down PC denied for ["sv << client.name << "] (wake setup)"sv;
      return refusal(401, "permission");
    }
    if (!supported()) {
      return refusal(404, "unsupported");
    }

    auto facts = gather(local_address);
    auto checks = policy::checks(facts);
    auto journal = read_journal();
    bool undoable = journal && !policy::undo_plan(*journal, facts).steps.empty();
    bool can_run = policy::blocker(facts) == "none"sv;
    bool fixable = !policy::plan(facts, checks).empty();

    if (method == "GET") {
      std::string token;
      std::optional<policy::result_t> last;
      {
        std::lock_guard lock {state_mutex};
        if (can_run && (fixable || undoable)) {
          token = new_token();
          tokens.issue(client.uuid, token, host_sleep_policy::clock::now());
        }
        last = last_result;
      }
      return {200, policy::status_json(facts, checks, undoable, last, token)};
    }
    if (method != "POST") {
      return refusal(400, "method");
    }

    std::string problem;
    auto request = policy::parse_request(body, problem);
    if (!request) {
      return refusal(400, problem);
    }
    {
      std::lock_guard lock {state_mutex};
      if (!tokens.redeem(client.uuid, request->token, host_sleep_policy::clock::now())) {
        return refusal(403, "token");
      }
    }
    if (!can_run) {
      BOOST_LOG(info) << "fork: Wake: client ["sv << client.name << "] asked, but "sv << policy::blocker(facts);
      return refusal(409, policy::blocker(facts));
    }
    if ((request->action == policy::action_e::fix && !fixable) || (request->action == policy::action_e::undo && !undoable)) {
      return refusal(409, "nothing");
    }
    if (applying.exchange(true)) {
      return refusal(409, "applying");
    }

    apply_soon(request->action, local_address, facts.adapter.guid, client.name);
    return {202, policy::accepted_json(request->action)};
  }
#else
  bool supported() {
    return false;
  }

  void start() {
  }

  reply_t handle(const client_t &client, const std::string &, const std::string &, const std::string &) {
    return client.permitted ? refusal(404, "unsupported") : refusal(401, "permission");
  }
#endif
}  // namespace fork_tweaks::wake_setup
