/**
 * @file src/httpcommon.cpp
 * @brief Definitions for common HTTP.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <filesystem>
#include <fstream>
#include <utility>

// lib includes
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <curl/curl.h>
#include <Simple-Web-Server/server_http.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "utility.h"
#include "atomic_file.h"
#include "password_hash.h"

namespace http {
  using namespace std::literals;
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  int reload_user_creds(const std::string &file);

  /// What the credentials file holds
  enum class creds_state_e {
    missing,  ///< No file, or a file (the state file) without credentials: the Web UI sets the first ones
    valid,  ///< Username, password and salt
    unreadable,  ///< A file that isn't JSON, or holds only some of the three
  };
  creds_state_e user_creds_state(const std::string &file);

  std::string unique_id;
  uuid_util::uuid_t uuid;
  net::net_e origin_web_ui_allowed;
  bool credentials_unreadable = false;

  int init() {
    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];
    origin_web_ui_allowed = net::from_enum_string(config::nvhttp.origin_web_ui_allowed);

    if (clean_slate) {
      uuid = uuid_util::uuid_t::generate();
      unique_id = uuid.string();
      auto dir = std::filesystem::temp_directory_path() / "Sunshine"sv;
      config::nvhttp.cert = (dir / ("cert-"s + unique_id)).string();
      config::nvhttp.pkey = (dir / ("pkey-"s + unique_id)).string();
    }

    if ((!fs::exists(config::nvhttp.pkey) || !fs::exists(config::nvhttp.cert)) &&
        create_creds(config::nvhttp.pkey, config::nvhttp.cert)) {
      return -1;
    }
    switch (user_creds_state(config::sunshine.credentials_file)) {
      case creds_state_e::missing:
        BOOST_LOG(info) << "Open the Web UI to set your new username and password and getting started";
        break;
      case creds_state_e::valid:
        if (reload_user_creds(config::sunshine.credentials_file)) {
          return -1;
        }
        break;
      case creds_state_e::unreadable:
        // Starting over would let anyone who reaches the Web UI first choose the password
        credentials_unreadable = true;
        BOOST_LOG(error) << "The Web UI credentials in "sv << config::sunshine.credentials_file
                         << " can't be read. Set them again with: sunshine --creds <username> <password>"sv;
        break;
    }
    return 0;
  }

  int save_user_creds(const std::string &file, const std::string &username, const std::string &password, bool run_our_mouth) {
    nlohmann::json outputTree = nlohmann::json::object();

    if (fs::exists(file)) {
      try {
        std::ifstream in(file);
        in >> outputTree;
      } catch (std::exception &e) {
        // Set the damaged file aside (it may hold paired clients worth recovering) and start a new
        // one, so --creds can always restore access
        auto aside = file + ".unreadable";
        std::error_code ec;
        fs::rename(file, aside, ec);
        BOOST_LOG(error) << "Couldn't read "sv << file << " ("sv << e.what() << "); "sv
                         << (ec ? "starting a new one"s : "moved it to "s + aside + " and started a new one"s);
        outputTree = nlohmann::json::object();
      }
    }

    auto salt = crypto::rand_alphabet(16);
    auto hash = password_hash::hash(password, salt);
    if (hash.empty()) {
      BOOST_LOG(error) << "Couldn't hash the new password"sv;
      return -1;
    }
    outputTree["username"] = username;
    outputTree["salt"] = salt;
    outputTree["password"] = hash;

    // Replaced in one step, so a crash can't leave half a file (which would read as damaged)
    if (!file_handler::write_file_atomic(file, outputTree.dump(4), true)) {
      BOOST_LOG(error) << "error writing to the credentials file, perhaps try this again as an administrator?"sv;
      return -1;
    }

    credentials_unreadable = false;
    BOOST_LOG(info) << "New credentials have been created"sv;
    return 0;
  }

  creds_state_e user_creds_state(const std::string &file) {
    if (!fs::exists(file)) {
      return creds_state_e::missing;
    }

    pt::ptree inputTree;
    try {
      pt::read_json(file, inputTree);
    } catch (std::exception &e) {
      BOOST_LOG(error) << "validating user credentials: "sv << e.what();
      return creds_state_e::unreadable;
    }

    int present = 0;
    for (const char *key : {"username", "password", "salt"}) {
      present += inputTree.find(key) != inputTree.not_found();
    }
    if (present == 0) {
      return creds_state_e::missing;  // the state file, before the first password is set
    }
    return present == 3 ? creds_state_e::valid : creds_state_e::unreadable;
  }

  int reload_user_creds(const std::string &file) {
    pt::ptree inputTree;
    try {
      pt::read_json(file, inputTree);
      config::sunshine.username = inputTree.get<std::string>("username");
      config::sunshine.password = inputTree.get<std::string>("password");
      config::sunshine.salt = inputTree.get<std::string>("salt");
    } catch (std::exception &e) {
      BOOST_LOG(error) << "loading user credentials: "sv << e.what();
      return -1;
    }
    return 0;
  }

  int create_creds(const std::string &pkey, const std::string &cert) {
    fs::path pkey_path = pkey;
    fs::path cert_path = cert;

    auto creds = crypto::gen_creds("Sunshine Gamestream Host"sv, 2048);

    auto pkey_dir = pkey_path;
    auto cert_dir = cert_path;
    pkey_dir.remove_filename();
    cert_dir.remove_filename();

    std::error_code err_code {};
    fs::create_directories(pkey_dir, err_code);
    if (err_code) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << pkey_dir << "] :"sv << err_code.message();
      return -1;
    }

    fs::create_directories(cert_dir, err_code);
    if (err_code) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << cert_dir << "] :"sv << err_code.message();
      return -1;
    }

    if (file_handler::write_file(pkey.c_str(), creds.pkey)) {
      BOOST_LOG(error) << "Couldn't open ["sv << config::nvhttp.pkey << ']';
      return -1;
    }

    if (file_handler::write_file(cert.c_str(), creds.x509)) {
      BOOST_LOG(error) << "Couldn't open ["sv << config::nvhttp.cert << ']';
      return -1;
    }

    fs::permissions(pkey_path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, err_code);

    if (err_code) {
      BOOST_LOG(error) << "Couldn't change permissions of ["sv << config::nvhttp.pkey << "] :"sv << err_code.message();
      return -1;
    }

    fs::permissions(cert_path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read | fs::perms::owner_write, fs::perm_options::replace, err_code);

    if (err_code) {
      BOOST_LOG(error) << "Couldn't change permissions of ["sv << config::nvhttp.cert << "] :"sv << err_code.message();
      return -1;
    }

    return 0;
  }

  bool download_file(const std::string &url, const std::string &file, long ssl_version) {
    // sonar complains about weak ssl and tls versions; however sonar cannot detect the fix
    CURL *curl = curl_easy_init();  // NOSONAR
    if (!curl) {
      BOOST_LOG(error) << "Couldn't create CURL instance";
      return false;
    }

    if (std::string file_dir = file_handler::get_parent_directory(file); !file_handler::make_directory(file_dir)) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << file_dir << ']';
      curl_easy_cleanup(curl);
      return false;
    }

    FILE *fp = fopen(file.c_str(), "wb");
    if (!fp) {
      BOOST_LOG(error) << "Couldn't open ["sv << file << ']';
      curl_easy_cleanup(curl);
      return false;
    }

    curl_easy_setopt(curl, CURLOPT_SSLVERSION, ssl_version);  // NOSONAR
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#ifdef _WIN32
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
      BOOST_LOG(error) << "Couldn't download ["sv << url << ", code:" << result << ']';
    }
    curl_easy_cleanup(curl);
    fclose(fp);
    return result == CURLE_OK;
  }

  std::string url_escape(const std::string &url) {
    char *string = curl_easy_escape(nullptr, url.c_str(), static_cast<int>(url.length()));
    std::string result(string);
    curl_free(string);
    return result;
  }

  std::string url_get_host(const std::string &url) {
    CURLU *curlu = curl_url();
    curl_url_set(curlu, CURLUPART_URL, url.c_str(), static_cast<unsigned int>(url.length()));
    char *host;
    if (curl_url_get(curlu, CURLUPART_HOST, &host, 0) != CURLUE_OK) {
      curl_url_cleanup(curlu);
      return "";
    }
    std::string result(host);
    curl_free(host);
    curl_url_cleanup(curlu);
    return result;
  }
}  // namespace http
