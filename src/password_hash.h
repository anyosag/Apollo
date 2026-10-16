/**
 * @file src/password_hash.h
 * @brief Hashing and checking the Web UI password.
 *
 * New hashes are PBKDF2-HMAC-SHA256, stored as "pbkdf2-sha256$<iterations>$<hex>". Hashes from
 * earlier versions are a single SHA-256 of the password and salt; they're still accepted, and
 * replaced with a PBKDF2 hash at the next login. Header-only (OpenSSL's libcrypto), so tests can
 * build it on its own.
 */
#pragma once

// standard includes
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

// lib includes
#include <openssl/crypto.h>
#include <openssl/evp.h>

namespace http::password_hash {
  /// OWASP's recommendation for PBKDF2-HMAC-SHA256 (2023): slow enough that a stolen hash takes
  /// years to crack for a decent password, at a fraction of a second per login
  constexpr int default_iterations = 600'000;

  constexpr std::string_view pbkdf2_prefix = "pbkdf2-sha256$";

  namespace detail {
    inline std::string to_hex(const unsigned char *data, std::size_t size, bool reversed) {
      static constexpr char digits[] = "0123456789ABCDEF";
      std::string out;
      out.reserve(size * 2);
      for (std::size_t i = 0; i < size; i++) {
        unsigned char byte = data[reversed ? size - 1 - i : i];
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
      }
      return out;
    }

    inline bool equal(std::string_view a, std::string_view b) {
      return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
    }

    /// The hash earlier versions stored: SHA-256 of password + salt, in util::hex's form
    /// (uppercase, last byte first)
    inline std::string legacy(std::string_view password, std::string_view salt) {
      std::string input;
      input.reserve(password.size() + salt.size());
      input.append(password).append(salt);
      std::array<unsigned char, 32> digest {};
      EVP_Digest(input.data(), input.size(), digest.data(), nullptr, EVP_sha256(), nullptr);
      return to_hex(digest.data(), digest.size(), true);
    }

    inline std::string pbkdf2(std::string_view password, std::string_view salt, int iterations) {
      std::array<unsigned char, 32> key {};
      if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), reinterpret_cast<const unsigned char *>(salt.data()), static_cast<int>(salt.size()), iterations, EVP_sha256(), static_cast<int>(key.size()), key.data()) != 1) {
        return {};
      }
      return to_hex(key.data(), key.size(), false);
    }
  }  // namespace detail

  /**
   * @brief Hashes a password for storing.
   * @return "pbkdf2-sha256$<iterations>$<hex>", or empty if hashing failed.
   */
  inline std::string hash(std::string_view password, std::string_view salt, int iterations = default_iterations) {
    auto key = detail::pbkdf2(password, salt, iterations);
    if (key.empty()) {
      return {};
    }
    return std::string(pbkdf2_prefix) + std::to_string(iterations) + "$" + key;
  }

  /** @brief Whether a stored hash is from an earlier version (and should be replaced). */
  inline bool is_legacy(std::string_view stored) {
    return stored.substr(0, pbkdf2_prefix.size()) != pbkdf2_prefix;
  }

  /**
   * @brief Checks a password against a stored hash, in constant time for a given hash.
   * @param stored The stored hash, in either format.
   */
  inline bool verify(std::string_view stored, std::string_view password, std::string_view salt) {
    if (is_legacy(stored)) {
      return !stored.empty() && detail::equal(stored, detail::legacy(password, salt));
    }

    auto rest = stored.substr(pbkdf2_prefix.size());
    auto separator = rest.find('$');
    if (separator == std::string_view::npos) {
      return false;
    }
    int iterations = 0;
    auto [end, ec] = std::from_chars(rest.data(), rest.data() + separator, iterations);
    if (ec != std::errc {} || end != rest.data() + separator || iterations < 1 || iterations > 100'000'000) {
      return false;
    }
    auto key = detail::pbkdf2(password, salt, iterations);
    return !key.empty() && detail::equal(rest.substr(separator + 1), key);
  }
}  // namespace http::password_hash
