// Tests for src/fork/update_manifest.h and update_verify.h: what a valid update manifest is, when an
// update installs, the records Apollo and its install helper leave each other, and the signature
// and SHA-256 checks. A standalone program, so CI builds it without the rest of Apollo:
//   g++ -std=c++23 -iquote src tests/fork/header/update_test.cpp -lcrypto

#include "fork/update_key.h"
#include "fork/update_manifest.h"
#include "fork/update_verify.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <openssl/evp.h>

using namespace fork_update;

static int s_Failures = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      s_Failures++; \
    } \
  } while (0)

static const std::string k_Prefix = "https://github.com/anyosag/Apollo/releases/download/";
static const std::string k_Sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const std::string k_Commit = "0123456789abcdef0123456789abcdef01234567";

static nlohmann::json good_manifest() {
  return {
    {"product", "apollo"},
    {"channel", "master"},
    {"run", 120},
    {"version", "0.4.8.110"},
    {"commit", k_Commit},
    {"published", "2026-10-18T20:00:00Z"},
    {"notes", "Fix the thing"},
    {"asset", {
      {"name", "Apollo-windows-installer.exe"},
      {"url", k_Prefix + "apollo-b120/Apollo-windows-installer.exe"},
      {"sha256", k_Sha},
      {"size", 12345678},
    }},
  };
}

static bool parses(const nlohmann::json &j) {
  return parse_manifest(j.dump(), "apollo", k_Prefix).has_value();
}

static void test_manifest() {
  std::string error;
  auto m = parse_manifest(good_manifest().dump(), "apollo", k_Prefix, &error);
  CHECK(m.has_value());
  if (m) {
    CHECK(m->run == 120);
    CHECK(m->version == "0.4.8.110");
    CHECK(m->asset.name == "Apollo-windows-installer.exe");
    CHECK(m->asset.size == 12345678);
    CHECK(m->notes == "Fix the thing");
  }

  // Each field that's wrong rejects the whole manifest
  auto with = [](const char *key, nlohmann::json value) {
    auto j = good_manifest();
    j[key] = value;
    return j;
  };
  auto with_asset = [](const char *key, nlohmann::json value) {
    auto j = good_manifest();
    j["asset"][key] = value;
    return j;
  };
  CHECK(!parses(with("product", "luzlunar")));
  CHECK(!parses(with("channel", "branch")));
  CHECK(!parses(with("run", 0)));
  CHECK(!parses(with("run", -5)));
  CHECK(!parses(with("run", "120")));
  CHECK(!parses(with("version", "")));
  CHECK(!parses(with("version", "0.4.8\n110")));
  CHECK(!parses(with("commit", "abc")));
  CHECK(!parses(with("commit", "0123456789ABCDEF0123456789abcdef01234567")));
  CHECK(!parses(with("notes", std::string(5000, 'x'))));
  CHECK(!parses(with_asset("name", "..\\sunshine.exe")));
  CHECK(!parses(with_asset("name", "a/b.exe")));
  CHECK(!parses(with_asset("name", ".hidden")));
  CHECK(!parses(with_asset("url", "https://example.com/Apollo-windows-installer.exe")));
  CHECK(!parses(with_asset("url", "https://github.com/someone/Apollo/releases/download/apollo-b120/Apollo-windows-installer.exe")));
  CHECK(!parses(with_asset("url", k_Prefix + "apollo-b120/other.exe")));
  CHECK(!parses(with_asset("url", k_Prefix + "../x/Apollo-windows-installer.exe")));
  CHECK(!parses(with_asset("url", k_Prefix + "apollo-b120/sub/Apollo-windows-installer.exe")));
  CHECK(!parses(with_asset("sha256", k_Sha.substr(1))));
  CHECK(!parses(with_asset("sha256", std::string(64, 'G'))));
  CHECK(!parses(with_asset("size", 0)));
  CHECK(!parses(with_asset("size", (int64_t) max_asset_size + 1)));

  auto missing = good_manifest();
  missing.erase("notes");
  CHECK(!parses(missing));
  auto no_asset = good_manifest();
  no_asset.erase("asset");
  CHECK(!parses(no_asset));

  CHECK(!parse_manifest("not json", "apollo", k_Prefix, &error).has_value());
  CHECK(!error.empty());
  CHECK(!parse_manifest("[1, 2]", "apollo", k_Prefix).has_value());
  CHECK(!parse_manifest(std::string(max_manifest_size + 1, ' '), "apollo", k_Prefix).has_value());
}

static void test_ordering() {
  auto m = *parse_manifest(good_manifest().dump(), "apollo", k_Prefix);

  build_t older {100, "master", "0.4.8.100", ""};
  build_t same {120, "master", "0.4.8.110", ""};
  build_t newer {130, "master", "0.4.8.120", ""};
  build_t branch {500, "branch", "0.4.8.200", ""};
  build_t local {0, "local", "0.4.8", ""};

  CHECK(is_update(m, older));
  CHECK(!is_update(m, same));
  CHECK(!is_update(m, newer));
  // A branch or local build is offered master's, whatever its run number
  CHECK(is_update(m, branch));
  CHECK(is_update(m, local));

  CHECK(may_install_automatically(m, older, true));
  CHECK(!may_install_automatically(m, older, false));
  CHECK(!may_install_automatically(m, same, true));
  // Only a click replaces a build installed on purpose
  CHECK(!may_install_automatically(m, branch, true));
  CHECK(!may_install_automatically(m, local, true));
  // A "master" build without a run number is a local build
  CHECK(!(build_t {0, "master", "", ""}.is_master()));
}

static void test_urls() {
  CHECK(allowed_download_url("https://github.com/anyosag/Apollo/releases/latest/download/update.json"));
  CHECK(allowed_download_url("https://objects.githubusercontent.com/github-production-release-asset/1?x=y"));
  CHECK(allowed_download_url("https://release-assets.githubusercontent.com/github-production-release-asset/1"));
  CHECK(!allowed_download_url("http://github.com/anyosag/Apollo"));
  CHECK(!allowed_download_url("https://github.com.evil.com/x"));
  CHECK(!allowed_download_url("https://github.com@evil.com/x"));
  CHECK(!allowed_download_url("https://github.com:8443/x"));
  CHECK(!allowed_download_url("https://evilgithubusercontent.com/x"));
  CHECK(!allowed_download_url("https://.githubusercontent.com/x"));
  CHECK(!allowed_download_url("https://example.com/?https://github.com/"));
  CHECK(!allowed_download_url("file:///C:/Windows/notepad.exe"));
  CHECK(!allowed_download_url(""));

  CHECK(release_asset_url(k_Prefix, "apollo-b", 42, "update.json") == k_Prefix + "apollo-b42/update.json");
  // The key file's own addresses pass the checks the updater makes
  CHECK(allowed_download_url(manifest_url));
  CHECK(allowed_download_url(signature_url));
  CHECK(std::string {asset_url_prefix} == k_Prefix);
}

static void test_decide_install() {
  using enum install_decision_e;
  // Nothing downloaded: nothing to do
  CHECK(decide_install(false, true, true, false, false) == none);
  // Asked for: never during a stream, but it may end an app nobody streams
  CHECK(decide_install(true, true, false, false, false) == install);
  CHECK(decide_install(true, true, false, true, false) == wait);
  CHECK(decide_install(true, true, false, false, true) == install);
  // Automatic: waits for both the stream and the app
  CHECK(decide_install(true, false, true, false, false) == install);
  CHECK(decide_install(true, false, true, true, false) == wait);
  CHECK(decide_install(true, false, true, false, true) == wait);
  // Neither: it waits for a click
  CHECK(decide_install(true, false, false, false, false) == none);
}

static void test_records() {
  started_t s {130, 4242, 47989, "0.4.8.120"};
  auto parsed = parse_started(to_json(s));
  CHECK(parsed.has_value());
  if (parsed) {
    CHECK(parsed->run == 130);
    CHECK(parsed->pid == 4242);
    CHECK(parsed->port == 47989);
    CHECK(parsed->version == "0.4.8.120");
  }
  CHECK(!parse_started("{}").has_value());
  CHECK(!parse_started(R"({"run": 1, "pid": 0, "port": 47989, "version": "x"})").has_value());
  CHECK(!parse_started(R"({"run": 1, "pid": 5, "port": 70000, "version": "x"})").has_value());
  CHECK(!parse_started("garbage").has_value());

  for (auto outcome : {outcome_e::updated, outcome_e::rolled_back, outcome_e::failed}) {
    result_t r {130, "0.4.8.120", outcome, "0.4.8.100", "the new build didn't start", 1760000000};
    auto back = parse_result(to_json(r));
    CHECK(back.has_value());
    if (back) {
      CHECK(back->outcome == outcome);
      CHECK(back->target_run == 130);
      CHECK(back->running_version == "0.4.8.100");
      CHECK(back->time == 1760000000);
    }
  }
  CHECK(!parse_result(R"({"target_run": 1, "target_version": "x", "outcome": "maybe", "running_version": "", "message": "", "time": 0})").has_value());

  CHECK(describe({130, "0.4.8.120", outcome_e::updated, "0.4.8.120", "", 0}) == "Updated to 0.4.8.120");
  CHECK(describe({130, "0.4.8.120", outcome_e::rolled_back, "0.4.8.100", "", 0}).find("went back to 0.4.8.100") != std::string::npos);
  CHECK(describe({130, "0.4.8.120", outcome_e::failed, "", "boom", 0}).find("failed: boom") != std::string::npos);

  CHECK(std::string {to_string(state_e::waiting)} == "waiting");
}

static void test_base64() {
  CHECK(decode_base64("aGVsbG8=") == std::optional<std::string> {"hello"});
  CHECK(decode_base64("aGVsbA==") == std::optional<std::string> {"hell"});
  CHECK(decode_base64("aGVs") == std::optional<std::string> {"hel"});
  CHECK(decode_base64("  aGVsbG8=\r\n") == std::optional<std::string> {"hello"});
  CHECK(!decode_base64("").has_value());
  CHECK(!decode_base64("aGVsbG8").has_value());
  CHECK(!decode_base64("aG=sbG8=").has_value());
  CHECK(!decode_base64("aGVsbG8=aGVs").has_value());
  CHECK(!decode_base64("aGV$bG8=").has_value());
}

static void test_signature() {
  // A throwaway key, signing the way CI does (openssl pkeyutl -sign -rawin)
  EVP_PKEY *key = nullptr;
  EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  CHECK(kctx && EVP_PKEY_keygen_init(kctx) == 1 && EVP_PKEY_keygen(kctx, &key) == 1);
  EVP_PKEY_CTX_free(kctx);
  if (!key) {
    return;
  }
  std::array<uint8_t, 32> pub {};
  std::size_t pub_size = pub.size();
  CHECK(EVP_PKEY_get_raw_public_key(key, pub.data(), &pub_size) == 1 && pub_size == 32);

  std::string manifest = good_manifest().dump();
  unsigned char sig[64];
  std::size_t sig_size = sizeof(sig);
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  CHECK(EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1);
  CHECK(EVP_DigestSign(ctx, sig, &sig_size, (const unsigned char *) manifest.data(), manifest.size()) == 1);
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);

  unsigned char encoded[128] = {};
  EVP_EncodeBlock(encoded, sig, (int) sig_size);
  std::string sig_file = std::string {(char *) encoded} + "\n";

  CHECK(verify_manifest(pub, manifest, sig_file));
  // One byte off anywhere fails
  auto tampered = manifest;
  tampered[10] ^= 1;
  CHECK(!verify_manifest(pub, tampered, sig_file));
  auto other_key = pub;
  other_key[0] ^= 1;
  CHECK(!verify_manifest(other_key, manifest, sig_file));
  CHECK(!verify_manifest(pub, manifest, "not base64"));
  CHECK(!verify_ed25519(pub, manifest, "short"));
  // Not signed by Apollo's real key
  CHECK(!verify_manifest(public_key, manifest, sig_file));
}

static void test_sha256() {
  auto path = std::filesystem::temp_directory_path() / "apollo_update_test.bin";
  {
    std::ofstream out {path, std::ios::binary};
    out << "abc";
  }
  uint64_t size = 0;
  auto hash = sha256_file(path, &size);
  CHECK(hash == std::optional<std::string> {"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
  CHECK(size == 3);
  std::filesystem::remove(path);
  CHECK(!sha256_file(path).has_value());

  sha256_t empty;
  CHECK(empty.hex() == std::optional<std::string> {"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
}

int main() {
  test_manifest();
  test_ordering();
  test_urls();
  test_decide_install();
  test_records();
  test_base64();
  test_signature();
  test_sha256();

  if (s_Failures == 0) {
    std::printf("update: all tests passed\n");
    return 0;
  }
  std::printf("update: %d failure(s)\n", s_Failures);
  return 1;
}
