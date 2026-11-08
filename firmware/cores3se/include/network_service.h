#pragma once
#include <stdint.h>
namespace network {
struct Snapshot {
  bool connected = false, apActive = false, mdns = false, credentialsSaved = false;
  int rssi = 0;
  uint8_t disconnectReason = 0;
  char ip[16] = "0.0.0.0", apIp[16] = "192.168.8.1";
  char ssid[33] = {}, apPassword[17] = {};
  char dhcpHostname[33] = {};
};
// begin/service run only in the HTTP task; UI consumes a copied snapshot.
void begin();
void service();
Snapshot snapshot();
bool saveCredentials(const char* ssid, const char* password);
const char* token();
constexpr const char* HOSTNAME = "npb-monitor";
}
