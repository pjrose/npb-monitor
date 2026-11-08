#pragma once
#include "can_monitor.h"
#include "network_service.h"
namespace displayui {
void begin();
void service(const canbus::Snapshot& can, const network::Snapshot& net);
uint32_t touches();
struct Status { uint32_t touches = 0, localJob = 0; uint8_t page = 0; bool confirming = false, holding = false; };
Status status();
// RGB bytes for an authenticated, read-only screenshot. Network page is redacted.
bool capture(uint8_t* rgb, size_t size);
}
