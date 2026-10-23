/**
 * @file src/fork/cursor_pulse.h
 * @brief Fork-only: LuzLunar's cursor pulse, drawn on the PC's own cursor in the stream.
 *
 * In LuzLunar's Mac trackpad mode the pointer pushed up against the top of the picture pulses
 * twice before it goes up into the notch bar, and once when it comes back. The cursor in the
 * picture is the one Apollo draws onto each captured frame from the shape Windows gives it
 * (display_vram.cpp), so Apollo pulses it there, in whatever shape it has: larger around its
 * hotspot, its light parts tinted toward LuzLunar's accent. Only the stream sees it; Windows'
 * cursor is untouched. While a pulse runs, the capture sends frames even when nothing else
 * changes. LuzLunar starts and stops pulses with POST /actions/cursor (cursor_position.h).
 *
 * The rules here are pure and tested (tests/fork/header/cursor_pulse_test.cpp); the state the
 * HTTPS thread starts and the capture thread reads is in cursor_pulse.cpp.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

// lib includes
#include <nlohmann/json.hpp>

namespace fork_tweaks::cursor_pulse {
  /// One pulse: larger and tinted, then back, ease-in-out
  constexpr double pulse_ms = 170;
  /// Settling back when a pulse is cut short, ease-out
  constexpr double settle_ms = 150;
  /// At the height of a pulse
  constexpr float peak_scale = 1.35f;
  /// At most this many pulses a request
  constexpr int max_pulses = 3;

  /// LuzLunar's accent, #9FB4FF, as fractions
  constexpr float accent_r = 159.f / 255.f;
  constexpr float accent_g = 180.f / 255.f;
  constexpr float accent_b = 1.f;

  /// How the cursor is drawn now: its scale around the hotspot and how far it's tinted
  struct look_t {
    float scale = 1.f;
    float tint = 0.f;

    bool rest() const {
      return scale == 1.f && tint == 0.f;
    }
  };

  /// What was asked for: pulses, or settling back from a look
  struct plan_t {
    int pulses = 0;
    bool motion = true;  ///< false with Reduce Motion: the tint only
    bool settle = false;
    look_t from;  ///< what it settles back from
  };

  /// How long a plan runs
  inline double length_ms(const plan_t &plan) {
    return plan.settle ? settle_ms : plan.pulses * pulse_ms;
  }

  /// The look ms into a plan; at rest before and after it
  inline look_t look_at(const plan_t &plan, double ms) {
    if (ms < 0 || ms >= length_ms(plan)) {
      return {};
    }
    if (plan.settle) {
      double t = ms / settle_ms;
      double eased = 1 - (1 - t) * (1 - t);  // ease-out
      return {
        static_cast<float>(plan.from.scale + (1 - plan.from.scale) * eased),
        static_cast<float>(plan.from.tint * (1 - eased)),
      };
    }
    // Each pulse rises and falls smoothly: (1 - cos) / 2 eases in and out of both halves
    double phase = std::fmod(ms, pulse_ms) / pulse_ms;
    double w = (1 - std::cos(2 * 3.14159265358979323846 * phase)) / 2;
    return {
      plan.motion ? static_cast<float>(1 + (peak_scale - 1) * w) : 1.f,
      static_cast<float>(w),
    };
  }

  /// A viewport in pixels
  struct view_t {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
  };

  /**
   * @brief The hotspot within the cursor's viewport, as the capture rotates the cursor
   * (gpu_cursor_t::update_viewport()). rotation: 0 none, 1 90°, 2 180°, 3 270°; tw, th: the
   * cursor image's width and height.
   */
  inline std::pair<float, float> hotspot_in_view(int rotation, float hx, float hy, float tw, float th) {
    switch (rotation) {
      case 1:
        return {hy, tw - hx};
      case 2:
        return {tw - hx, th - hy};
      case 3:
        return {th - hy, hx};
      default:
        return {hx, hy};
    }
  }

  /// The viewport scaled around a point in it, so that point stays where it is
  inline view_t scaled(const view_t &view, float px, float py, float scale) {
    return {view.x + px * (1 - scale), view.y + py * (1 - scale), view.w * scale, view.h * scale};
  }

  /**
   * @brief Tints BGRA pixels toward the accent: each color multiplied by 1 - t + t × accent, so
   * white becomes the accent, black stays black, and alpha stays as it is.
   */
  inline void tint_bgra(std::uint8_t *pixels, std::size_t count, float t) {
    float fb = 1 - t + t * accent_b;
    float fg = 1 - t + t * accent_g;
    float fr = 1 - t + t * accent_r;
    for (std::size_t i = 0; i < count; i++) {
      std::uint8_t *p = pixels + 4 * i;
      p[0] = static_cast<std::uint8_t>(std::lround(p[0] * fb));
      p[1] = static_cast<std::uint8_t>(std::lround(p[1] * fg));
      p[2] = static_cast<std::uint8_t>(std::lround(p[2] * fr));
    }
  }

  /// POST /actions/cursor's body
  struct request_t {
    int pulses = 0;  ///< 0: settle back now
    bool if_at_top = false;  ///< only while the cursor is at the top of the display
    bool reduce_motion = false;
  };

  /// {"pulse": N, "ifAtTop": bool, "reduceMotion": bool}; none for a body that isn't one
  inline std::optional<request_t> parse_request(const std::string &body) {
    auto json = nlohmann::json::parse(body, nullptr, false);
    if (!json.is_object() || !json.contains("pulse") || !json["pulse"].is_number_integer()) {
      return std::nullopt;
    }
    request_t request;
    request.pulses = std::clamp(json["pulse"].get<int>(), 0, max_pulses);
    if (json.contains("ifAtTop") && json["ifAtTop"].is_boolean()) {
      request.if_at_top = json["ifAtTop"].get<bool>();
    }
    if (json.contains("reduceMotion") && json["reduceMotion"].is_boolean()) {
      request.reduce_motion = json["reduceMotion"].get<bool>();
    }
    return request;
  }

  /**
   * @brief The pulse running now, as the HTTPS thread starts it and the capture thread reads it.
   * Not thread safe by itself: cursor_pulse.cpp holds it under a mutex.
   */
  class state_t {
  public:
    using clock = std::chrono::steady_clock;

    /// Starts pulses, or settles back from the look it has now
    void start(const request_t &request, clock::time_point now) {
      plan_t plan;
      if (request.pulses > 0) {
        plan.pulses = request.pulses;
        plan.motion = !request.reduce_motion;
      } else {
        look_t from = look(now);
        if (from.rest()) {
          plan_ = {};
          return;
        }
        plan.settle = true;
        plan.from = from;
      }
      plan_ = plan;
      start_ = now;
    }

    look_t look(clock::time_point now) const {
      return look_at(plan_, std::chrono::duration<double, std::milli>(now - start_).count());
    }

    /// While it runs, and for a little after, so the frame that draws the cursor at rest again goes out
    bool animating(clock::time_point now) const {
      double ms = std::chrono::duration<double, std::milli>(now - start_).count();
      return length_ms(plan_) > 0 && ms >= 0 && ms < length_ms(plan_) + 50;
    }

  private:
    plan_t plan_;
    clock::time_point start_ {};
  };

  // cursor_pulse.cpp

  /// The capture can pulse the cursor: Desktop Duplication on the GPU, where Apollo draws it
  void set_capable(bool capable);
  bool capable();

  /// From the HTTPS thread
  void start(const request_t &request);

  /// From the capture thread
  look_t look();
  bool animating();
}  // namespace fork_tweaks::cursor_pulse
