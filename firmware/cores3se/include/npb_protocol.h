#pragma once
#include <stddef.h>
#include <stdint.h>

// Pure protocol/state code: shared by firmware and on-device unit tests.
// Register definitions and masks mirror src/npb_monitor/protocol.py.
namespace npb {
constexpr uint32_t STALE_MS = 15000;
constexpr uint8_t ADDRESS = 0;
constexpr uint16_t COMMANDS[] = {
  0x0060, 0x0061, 0x0062, 0x0040, 0x00B8, 0x00C1,
  0x0000, 0x0020, 0x0030, 0x00B0, 0x00B1, 0x00B2,
  0x00B3, 0x00B4, 0x00B9, 0x00C2
};
constexpr size_t REGISTER_COUNT = sizeof(COMMANDS) / sizeof(COMMANDS[0]);
constexpr uint32_t controllerId(uint8_t address) { return 0x000C0100u | address; }
constexpr uint32_t chargerId(uint8_t address) { return 0x000C0000u | address; }
inline int indexOf(uint16_t command) {
  for (size_t i = 0; i < REGISTER_COUNT; ++i) if (COMMANDS[i] == command) return i;
  return -1;
}
inline uint8_t replyLength(uint16_t command) {
  return command == 0x0000 || command == 0x0040 || command == 0x00C1 ? 3 : 4;
}
inline float scale(uint16_t command) {
  if (command == 0x0062) return 0.1f;
  switch (command) {
    case 0x0020: case 0x0030: case 0x0060: case 0x0061:
    case 0x00B0: case 0x00B1: case 0x00B2: case 0x00B3: case 0x00B9: return 0.01f;
    default: return 1.0f;
  }
}
inline float decodedValue(uint16_t command, uint16_t raw) {
  return (command == 0x0062 ? float(int16_t(raw)) : float(raw)) * scale(command);
}
struct Frame {
  uint32_t id = 0;
  bool extended = false;
  bool rtr = false;
  uint8_t length = 0;
  uint8_t data[8] = {};
};
inline bool makeRead(uint16_t command, uint8_t address, Frame& out) {
  if (address > 3 || indexOf(command) < 0) return false;
  out = Frame{};
  out.id = controllerId(address);
  out.extended = true;
  out.length = 2;
  out.data[0] = command & 0xff;
  out.data[1] = command >> 8;
  return true;
}
// Polling/read paths pass this gate. Validated writes have a separate gate.
inline bool permittedRead(const Frame& f, uint8_t address) {
  return address <= 3 && f.id == controllerId(address) && f.extended && !f.rtr
    && f.length == 2 && indexOf(uint16_t(f.data[0]) | uint16_t(f.data[1]) << 8) >= 0;
}
struct Register {
  uint16_t raw = 0;
  uint32_t updated = 0;
  bool seen = false;
  bool fresh(uint32_t now) const { return seen && uint32_t(now - updated) < STALE_MS; }
  const char* quality(uint32_t now) const { return !seen ? "unavailable" : fresh(now) ? "valid" : "stale"; }
};
struct State {
  Register registers[REGISTER_COUNT] = {};
  uint32_t received = 0;
  uint32_t lastReply = 0;
  const Register& get(uint16_t cmd) const {
    static const Register unknown;
    const int index = indexOf(cmd);
    return index < 0 ? unknown : registers[index];
  }
  bool online(uint32_t now) const { return received && uint32_t(now - lastReply) < STALE_MS; }
  bool accept(const Frame& frame, uint32_t now, uint8_t address = ADDRESS) {
    if (address > 3 || frame.id != chargerId(address) || !frame.extended || frame.rtr
      || frame.length < 3 || frame.length > 8) return false;
    const uint16_t command = uint16_t(frame.data[0]) | uint16_t(frame.data[1]) << 8;
    const int index = indexOf(command);
    if (index < 0 || frame.length < replyLength(command)) return false;
    uint16_t raw = frame.data[2];
    if (replyLength(command) == 4) raw |= uint16_t(frame.data[3]) << 8;
    registers[index].raw = raw;
    registers[index].updated = now;
    registers[index].seen = true;
    lastReply = now;
    ++received;
    return true;
  }
};
struct Flag { uint16_t command; uint16_t mask; const char* name; };
constexpr Flag WARNINGS[] = {
  {0x0040, 0x0002, "Over temperature"}, {0x0040, 0x0004, "Output over voltage"},
  {0x0040, 0x0008, "Output over current"}, {0x0040, 0x0010, "Output short circuit"},
  {0x0040, 0x0020, "AC abnormal"}, {0x0040, 0x0040, "Output off"},
  {0x0040, 0x0080, "Internal temperature abnormal"}, {0x00B8, 0x0040, "Wake-up unfinished"},
  {0x00B8, 0x0400, "Temperature compensation short"}, {0x00B8, 0x0800, "No battery detected"},
  {0x00B8, 0x2000, "CC timeout"}, {0x00B8, 0x4000, "CV timeout"},
  {0x00B8, 0x8000, "Float timeout"}, {0x00C1, 0x0040, "EEPROM access error"}
};
inline bool active(const State& s, const Flag& flag, uint32_t now) {
  const auto& reg = s.get(flag.command);
  return reg.fresh(now) && (reg.raw & flag.mask);
}
inline const char* phase(const State& s, uint32_t now) {
  const auto& reg = s.get(0x00B8);
  if (!reg.fresh(now)) return "Unknown";
  if (reg.raw & 0x01) return "Fully charged";
  if (reg.raw & 0x02) return "Constant current";
  if (reg.raw & 0x04) return "Constant voltage";
  if (reg.raw & 0x08) return "Float";
  return "Idle";
}
inline const char* mode(const State& s, uint32_t now) {
  const auto& reg = s.get(0x00B4);
  return !reg.fresh(now) ? "Unknown" : reg.raw & 0x80 ? "Charger" : "Power supply";
}
inline bool statusFresh(const State& s, uint32_t now) {
  return s.get(0x0040).fresh(now) && s.get(0x00B8).fresh(now) && s.get(0x00C1).fresh(now);
}
} // namespace npb
