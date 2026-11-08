#pragma once
#include "npb_protocol.h"
#include <math.h>
#include <string.h>

namespace control {
struct WriteSpec { uint16_t command; const char* action; uint16_t minimum, maximum; bool scaled; };
// NPB-450-24NFC limits from the manufacturer's manual, printed pages 61-64.
// Raw values are centivolts/centiamps. The old desktop minima included 72V-model values.
constexpr WriteSpec SPECS[] = {
  {0x0000, "operation", 0, 1, false},
  {0x0020, "set_psu_voltage", 2100, 4200, true},
  {0x0030, "set_psu_current", 270, 1350, true},
  {0x00B0, "set_curve_cc", 270, 1350, true},
  {0x00B1, "set_curve_cv", 2100, 4200, true},
  {0x00B2, "set_curve_float", 2100, 4200, true},
  {0x00B3, "set_curve_taper", 27, 405, true},
  {0x00B9, "set_restart_voltage", 2100, 4200, true},
  {0x00B4, "curve_config_raw", 0, 65535, false},
  {0x00C2, "system_config_raw", 0, 65535, false}
};
inline const WriteSpec* spec(uint16_t command) {
  for (const auto& item : SPECS) if (item.command == command) return &item;
  return nullptr;
}
inline const WriteSpec* spec(const char* action) {
  for (const auto& item : SPECS) if (!strcmp(item.action, action)) return &item;
  return nullptr;
}
inline bool encode(uint16_t command, double value, npb::Frame& frame) {
  const auto* item = spec(command);
  if (!item || !isfinite(value)) return false;
  const double raw = item->scaled ? value * 100.0 : value;
  if (raw < item->minimum - 0.000001 || raw > item->maximum + 0.000001) return false;
  if (fabs(raw - round(raw)) > 0.000001) return false;
  const uint16_t number = uint16_t(round(raw));
  if (command == 0x00C2 && ((number & ~0x0407u) || (number & 0x06) == 0x06)) return false;
  frame = npb::Frame{}; frame.id = npb::controllerId(0); frame.extended = true;
  frame.length = command == 0 ? 3 : 4;
  frame.data[0] = command & 0xff; frame.data[1] = command >> 8;
  frame.data[2] = number & 0xff; frame.data[3] = number >> 8;
  return true;
}
inline uint16_t command(const npb::Frame& f) { return uint16_t(f.data[0]) | uint16_t(f.data[1]) << 8; }
inline uint16_t raw(const npb::Frame& f) { return uint16_t(f.data[2]) | (f.length == 4 ? uint16_t(f.data[3]) << 8 : 0); }
inline bool validWrite(const npb::Frame& f) {
  if (!f.extended || f.rtr || f.id != npb::controllerId(0)) return false;
  const auto* item = spec(command(f));
  if (!item || f.length != (item->command == 0 ? 3 : 4)) return false;
  npb::Frame encoded;
  return encode(item->command, item->scaled ? raw(f) / 100.0 : raw(f), encoded);
}
inline const char* stateError(const npb::State& state, const npb::Frame& frame, uint32_t now) {
  if (!validWrite(frame)) return "Invalid or unsupported write";
  // An explicit OFF remains available even when measurement freshness is lost.
  if (command(frame) == 0 && raw(frame) == 0) return nullptr;
  if (!state.online(now) || !npb::statusFresh(state, now) || !state.get(command(frame)).fresh(now)) return "Fresh charger readback required";
  if (command(frame) == 0x00B2 && (!state.get(0x00B1).fresh(now) || raw(frame) > state.get(0x00B1).raw)) return "Float voltage cannot exceed fresh CV voltage";
  if (command(frame) == 0x00B1 && (!state.get(0x00B2).fresh(now) || raw(frame) < state.get(0x00B2).raw)) return "Lower float voltage before reducing CV below it";
  if (command(frame) == 0 && raw(frame) == 1) {
    if (!state.get(0x00B4).fresh(now)) return "Fresh mode readback required";
    if (state.get(0x0040).raw & 0xBE) return "Active charger fault blocks output-on";
    if (state.get(0x00C1).raw & 0x40) return "EEPROM fault blocks output-on";
    if (state.get(0x00B4).raw & 0x80) {
      if (state.get(0x00B8).raw & 0xEC00) return "Battery or charge fault blocks output-on";
    }
  }
  return nullptr;
}
constexpr uint16_t REVIEW_COMMANDS[] = {0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B9, 0x00B4, 0x00C2};
struct ChargeReview { uint16_t values[7] = {}; };
inline const char* chargeError(const npb::State& state, const ChargeReview& review, uint32_t now) {
  npb::Frame on; encode(0, 1, on);
  if (const char* error = stateError(state, on, now)) return error;
  for (size_t i = 0; i < 7; ++i) {
    const auto& reg = state.get(REVIEW_COMMANDS[i]);
    if (!reg.fresh(now)) return "Fresh charge settings required";
    if (reg.raw != review.values[i]) return "Settings changed; review the current values again";
  }
  if ((review.values[5] & 0x83) != 0x80) return "Select the custom charger curve before using Charge now";
  if (!(review.values[6] & 1)) return "CAN control must be enabled";
  for (size_t i = 0; i < 5; ++i) {
    npb::Frame test;
    if (!encode(REVIEW_COMMANDS[i], review.values[i] * .01, test)) return "Charge settings are outside this charger model's limits";
  }
  if (review.values[2] > review.values[1] || review.values[4] >= review.values[1] || review.values[3] >= review.values[0]) return "Review the float, restart and taper settings";
  // Conservative shortcut envelope for TimeUSB 25.6V 25Ah; not a generic battery profile.
  if (review.values[0] > 500 || review.values[1] < 2840 || review.values[1] > 2920 || review.values[2] > 2700) return "TimeUSB shortcut: CC up to 5A, CV 28.4-29.2V, float at most 27V";
  const auto& voltage = state.get(0x0060);
  if (!voltage.fresh(now) || voltage.raw < 2100 || voltage.raw > 2920) return "Battery voltage is missing or outside the TimeUSB shortcut's range";
  return nullptr;
}
inline const char* notice(uint16_t cmd) {
  if (cmd == 0x00B4) return "Mode/configuration readback is not activation. An AC power cycle is required; this device does not switch AC.";
  if ((cmd >= 0x00B0 && cmd <= 0x00B3) || cmd == 0x00B9) return "Curve readback is verified separately. With output on, activate changes by an intentional Operation off/on or manual AC power cycle.";
  if (cmd == 0x0020 || cmd == 0x0030) return "PSU setpoints apply immediately in PSU mode. Verify actual output separately.";
  if (cmd == 0x00C2) return "System configuration affects CAN control, startup behavior and EEPROM persistence.";
  return "";
}
inline bool persistenceWrite(const npb::State& state, bool disable, uint32_t now, npb::Frame& frame) {
  const auto& system = state.get(0x00C2);
  if (!system.fresh(now)) return false;
  const uint16_t value = (system.raw | 1) & ~0x0400u;
  return encode(0x00C2, value | (disable ? 0x0400u : 0), frame);
}
}
