/**
 * @file src/fork/extend_layout.h
 * @brief The displays the Virtual Display app streams with: the PC's own, as they were, and the
 * virtual display next to them as one more extended display.
 *
 * Header-only, without Windows types, so tests can build it on its own.
 */
#pragma once

// standard includes
#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace extend_layout {
  /// Windows' active displays by device id, as display_device::ActiveTopology has them: the
  /// displays in one group duplicate each other
  using topology_t = std::vector<std::vector<std::string>>;

  /// The same displays in the same groups, in whatever order
  inline bool same(topology_t a, topology_t b) {
    for (auto *topology : {&a, &b}) {
      for (auto &group : *topology) {
        std::sort(group.begin(), group.end());
      }
      std::sort(topology->begin(), topology->end());
    }
    return a == b;
  }

  /**
   * @brief The displays to have on once the virtual display has arrived.
   *
   * Windows applies the layout it saved for a set of displays when that set connects again. A
   * stream that left only the virtual display on had that layout saved, so the virtual display
   * can arrive with the PC's displays switched off.
   *
   * @param before The active displays before the virtual display arrived.
   * @param now The active displays after it arrived.
   * @param virtual_id The virtual display's device id, or empty if it isn't known.
   * @return The displays to have on, or nothing when `now` is already that. Every display on
   * before or now is on, each group of `before` as it was, and the virtual display in a group of
   * its own. A display is never switched off.
   */
  inline std::optional<topology_t> extended(const topology_t &before, const topology_t &now, const std::string &virtual_id) {
    topology_t want;
    std::set<std::string> placed;

    auto add = [&](const std::vector<std::string> &group) {
      std::vector<std::string> kept;
      for (const auto &id : group) {
        if (!id.empty() && placed.insert(id).second) {
          kept.push_back(id);
        }
      }
      if (!kept.empty()) {
        want.push_back(std::move(kept));
      }
    };

    for (const auto &group : before) {
      std::vector<std::string> without_virtual;
      std::copy_if(group.begin(), group.end(), std::back_inserter(without_virtual), [&](const std::string &id) {
        return id != virtual_id;
      });
      add(without_virtual);
    }

    // Shown alone, not mirroring one of the PC's displays
    if (!virtual_id.empty()) {
      add({virtual_id});
    }

    // Whatever else Windows has on stays on
    for (const auto &group : now) {
      add(group);
    }

    if (same(want, now)) {
      return std::nullopt;
    }
    return want;
  }

  /// For the log: `[a, b] [c]`
  inline std::string describe(const topology_t &topology) {
    std::string out;
    for (const auto &group : topology) {
      if (!out.empty()) {
        out += ' ';
      }
      out += '[';
      for (std::size_t i = 0; i < group.size(); ++i) {
        out += (i ? ", " : "") + group[i];
      }
      out += ']';
    }
    return out.empty() ? "[]" : out;
  }
}  // namespace extend_layout
