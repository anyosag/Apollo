/**
 * @file src/fork/update_verify.h
 * @brief Checks for over-the-air updates: the manifest's Ed25519 signature and a file's SHA-256,
 * with OpenSSL. crypto::verify() takes an X.509 certificate and a digest, which Ed25519 doesn't use.
 *
 * update.json.sig holds the base64 of the 64-byte Ed25519 signature over update.json's exact bytes,
 * as `openssl pkeyutl -sign -rawin` makes it in CI.
 *
 * Header-only, without Boost, so tests can build it on its own (with -lcrypto).
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// lib includes
#include <openssl/evp.h>

namespace fork_update {
  namespace detail {
    struct pkey_deleter {
      void operator()(EVP_PKEY *p) const {
        EVP_PKEY_free(p);
      }
    };

    struct md_ctx_deleter {
      void operator()(EVP_MD_CTX *p) const {
        EVP_MD_CTX_free(p);
      }
    };

    struct file_closer {
      void operator()(std::FILE *f) const {
        std::fclose(f);
      }
    };

    inline int base64_value(char c) {
      if (c >= 'A' && c <= 'Z') {
        return c - 'A';
      }
      if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
      }
      if (c >= '0' && c <= '9') {
        return c - '0' + 52;
      }
      if (c == '+') {
        return 62;
      }
      if (c == '/') {
        return 63;
      }
      return -1;
    }
  }  // namespace detail

  /**
   * @brief Decode strict, padded base64, ignoring surrounding whitespace.
   * @return The bytes, or nothing if it isn't valid base64.
   */
  inline std::optional<std::string> decode_base64(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
      text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
      text.remove_suffix(1);
    }
    if (text.empty() || text.size() % 4 != 0) {
      return std::nullopt;
    }

    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
      int v[4];
      int padding = 0;
      for (int j = 0; j < 4; j++) {
        char c = text[i + j];
        // Padding only at the very end: "xx==" or "xxx="
        if (c == '=' && i + 4 == text.size() && j >= 2 && (j == 3 || text[i + 3] == '=')) {
          v[j] = 0;
          padding++;
          continue;
        }
        v[j] = detail::base64_value(c);
        if (v[j] < 0 || padding > 0) {
          return std::nullopt;
        }
      }
      uint32_t triple = (uint32_t) v[0] << 18 | (uint32_t) v[1] << 12 | (uint32_t) v[2] << 6 | (uint32_t) v[3];
      out += (char) (triple >> 16);
      if (padding < 2) {
        out += (char) (triple >> 8);
      }
      if (padding < 1) {
        out += (char) triple;
      }
    }
    return out;
  }

  /**
   * @brief Check an Ed25519 signature.
   * @param key The raw 32-byte public key.
   * @param message The signed bytes.
   * @param signature The raw 64-byte signature.
   */
  inline bool verify_ed25519(const std::array<uint8_t, 32> &key, std::string_view message, std::string_view signature) {
    if (signature.size() != 64) {
      return false;
    }
    std::unique_ptr<EVP_PKEY, detail::pkey_deleter> pkey {EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, key.data(), key.size())};
    std::unique_ptr<EVP_MD_CTX, detail::md_ctx_deleter> ctx {EVP_MD_CTX_new()};
    if (!pkey || !ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
      return false;
    }
    return EVP_DigestVerify(ctx.get(), (const unsigned char *) signature.data(), signature.size(), (const unsigned char *) message.data(), message.size()) == 1;
  }

  /**
   * @brief Check a manifest against the contents of its .sig file (base64).
   */
  inline bool verify_manifest(const std::array<uint8_t, 32> &key, std::string_view manifest, std::string_view signature_file) {
    auto signature = decode_base64(signature_file);
    return signature && verify_ed25519(key, manifest, *signature);
  }

  /// A running SHA-256, for files too large to hold at once
  class sha256_t {
  public:
    sha256_t():
        ctx {EVP_MD_CTX_new()} {
      ok = ctx && EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1;
    }

    void update(const void *data, std::size_t size) {
      ok = ok && EVP_DigestUpdate(ctx.get(), data, size) == 1;
    }

    /// The lowercase hex digest, or nothing if OpenSSL failed
    std::optional<std::string> hex() {
      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int length = 0;
      if (!ok || EVP_DigestFinal_ex(ctx.get(), digest, &length) != 1) {
        return std::nullopt;
      }
      ok = false;
      static constexpr char digits[] = "0123456789abcdef";
      std::string out;
      for (unsigned int i = 0; i < length; i++) {
        out += digits[digest[i] >> 4];
        out += digits[digest[i] & 0xF];
      }
      return out;
    }

  private:
    std::unique_ptr<EVP_MD_CTX, detail::md_ctx_deleter> ctx;
    bool ok;
  };

  /**
   * @brief The SHA-256 of a file, and its size.
   * @return Lowercase hex, or nothing if it can't be read.
   */
  inline std::optional<std::string> sha256_file(const std::filesystem::path &path, uint64_t *size = nullptr) {
#ifdef _WIN32
    std::unique_ptr<std::FILE, detail::file_closer> file {_wfopen(path.c_str(), L"rb")};
#else
    std::unique_ptr<std::FILE, detail::file_closer> file {std::fopen(path.c_str(), "rb")};
#endif
    if (!file) {
      return std::nullopt;
    }
    sha256_t hash;
    uint64_t total = 0;
    char buffer[64 * 1024];
    std::size_t read;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file.get())) > 0) {
      hash.update(buffer, read);
      total += read;
    }
    if (std::ferror(file.get())) {
      return std::nullopt;
    }
    if (size) {
      *size = total;
    }
    return hash.hex();
  }
}  // namespace fork_update
