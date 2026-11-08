#pragma once
#include "npb_protocol.h"
namespace history {
constexpr size_t CAPACITY = 1441;
constexpr uint32_t INTERVAL_MS = 60000;
constexpr uint32_t RETENTION_MS = 86400000;
struct Sample {
  uint32_t ms = 0;
  uint16_t voltage = 0, current = 0;
  bool valid = false;
};
// This bounded ring belongs to the HTTP task. Missing readings become gaps.
class Ring {
  Sample samples[CAPACITY];
  size_t head = 0, used = 0;
  uint32_t last = 0;
  bool started = false;
public:
  void record(const npb::State& state, uint32_t now) {
    if (started && uint32_t(now - last) < INTERVAL_MS) return;
    started = true; last = now;
    auto& sample = samples[head];
    sample.ms = now;
    sample.valid = state.get(0x0060).fresh(now) && state.get(0x0061).fresh(now);
    sample.voltage = state.get(0x0060).raw; sample.current = state.get(0x0061).raw;
    head = (head + 1) % CAPACITY;
    if (used < CAPACITY) ++used;
  }
  size_t count() const { return used; }
  const Sample& at(size_t index) const { return samples[(head + CAPACITY - used + index) % CAPACITY]; }
};
}
