/**
 * @file src/fork/update_key.h
 * @brief Where Apollo's over-the-air updates come from, and the key they must be signed with.
 *
 * CI's publish job (.github/workflows/build-windows.yml) signs each master build's update.json with
 * the Ed25519 key in the `release` environment's FORK_UPDATE_KEY secret. LuzLunar checks its own
 * updates against the same public key (app/fork/updatekey.h). See FORK.md, "Updates".
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <string_view>

namespace fork_update {
  /// The raw Ed25519 public key that signs update manifests
  inline constexpr std::array<uint8_t, 32> public_key {
    0xa7, 0xd6, 0xa2, 0x94, 0x4a, 0x31, 0xa7, 0xb1, 0x42, 0xb9, 0xde, 0x92, 0xc5, 0xc7, 0x0f, 0x5c,
    0x0f, 0x80, 0xb0, 0x35, 0x71, 0x10, 0x3c, 0x1c, 0x61, 0x43, 0x56, 0xe2, 0xa6, 0x32, 0x18, 0x5e,
  };

  /// The product name a manifest must carry, so LuzLunar's manifests can't pass as Apollo's
  inline constexpr std::string_view product = "apollo";

  /// The latest release's manifest and its signature (GitHub redirects these to the latest release)
  inline constexpr std::string_view manifest_url = "https://github.com/anyosag/Apollo/releases/latest/download/update.json";
  inline constexpr std::string_view signature_url = "https://github.com/anyosag/Apollo/releases/latest/download/update.json.sig";

  /// Every asset a manifest names must be under this prefix
  inline constexpr std::string_view asset_url_prefix = "https://github.com/anyosag/Apollo/releases/download/";

  /// A build's own release, for the installer kept to roll back to
  inline constexpr std::string_view release_tag_prefix = "apollo-b";
}  // namespace fork_update
