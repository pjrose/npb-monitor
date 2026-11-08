#pragma once
#include <stdint.h>

namespace touchcontrol {
// A review must see every finger released before a new, continuous hold counts.
// Moving out or adding a finger cancels that gesture until another release.
class HoldToStart {
  uint32_t opened_ = 0, started_ = 0;
  bool armed_ = false, holding_ = false, finished_ = false;
public:
  static constexpr uint32_t HOLD_MS = 2000, REVIEW_MS = 30000;
  void open(uint32_t now) { opened_ = now; started_ = 0; armed_ = holding_ = finished_ = false; }
  bool expired(uint32_t now) const { return uint32_t(now - opened_) >= REVIEW_MS; }
  uint32_t progress(uint32_t now) const { return holding_ ? uint32_t(now - started_) : 0; }
  bool update(uint32_t now, unsigned fingers, bool inside, bool newPress) {
    if (finished_ || expired(now)) return false;
    if (!fingers) { armed_ = true; holding_ = false; return false; }
    if (fingers != 1 || !inside) { armed_ = false; holding_ = false; return false; }
    if (holding_ && newPress) { armed_ = false; holding_ = false; return false; }
    if (!armed_) return false;
    if (!holding_) {
      if (!newPress) { armed_ = false; return false; }
      started_ = now; holding_ = true;
    }
    if (uint32_t(now - started_) < HOLD_MS) return false;
    holding_ = false; armed_ = false; finished_ = true; return true;
  }
};
}
