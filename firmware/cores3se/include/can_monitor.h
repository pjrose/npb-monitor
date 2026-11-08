#pragma once
#include "npb_protocol.h"
#include "charger_control.h"

namespace canbus {
struct Diagnostics {
  bool ready = false;
  int state = 0;
  int error = 0;
  uint32_t txQueued = 0, txRejected = 0, invalidRx = 0, otherRx = 0;
  uint32_t txErrors = 0, rxErrors = 0, failed = 0, busErrors = 0, missed = 0;
  uint32_t recoveries = 0, lastService = 0, maxServiceGap = 0;
  uint32_t lastErrorAt = 0;
};
struct Snapshot { npb::State charger; Diagnostics bus; };
struct Traffic { uint32_t ms = 0; npb::Frame frame; bool tx = false; bool accepted = false; };
constexpr size_t TRAFFIC_CAP = 48;
bool begin();
Snapshot snapshot();
size_t traffic(Traffic* out, size_t capacity);
const char* stateName(const Diagnostics& d);
struct ControlStatus {
  uint32_t id = 0, updated = 0;
  uint16_t command = 0, requested = 0, observed = 0;
  uint8_t step = 0, count = 0;
  char state[20] = "idle", error[96] = {};
};
bool controlsEnabled();
const char* setControlsEnabled(bool enabled);
void setPaused(bool paused);
bool paused();
ControlStatus controlStatus();
const char* submit(const npb::Frame* frames, size_t count, uint32_t& id, const control::ChargeReview* review = nullptr);
// Physical controls have Operation-only authority; they never unlock remote writes.
const char* submitLocalCharge(const control::ChargeReview& review, uint32_t& id);
const char* submitLocalStop(uint32_t& id);
bool acCyclePending();
void setAcCyclePending(bool pending);
}
