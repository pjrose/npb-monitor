#include "network_service.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_netif.h>
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif
#if __has_include("device_token.h")
#include "device_token.h"
#else
#define DEVICE_TOKEN ""
#endif

namespace network {
namespace {
Preferences prefs;
String ssid, password, apPassword;
Snapshot shared;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t reason = 0;
bool apActive = false, wasConnected = false, mdns = false;
uint32_t lastAttempt = 0, disconnectedAt = 0, lastSnapshot = 0;
uint32_t reconnectAt = 0;
bool credentialsSaved = false;
struct Credentials { char ssid[33] = {}; char password[64] = {}; };
void connect() {
  lastAttempt = millis();
  // Set the actual STA interface too, even if another library already enabled Wi-Fi.
  if (auto* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) esp_netif_set_hostname(station, HOSTNAME);
  WiFi.begin(ssid.c_str(), password.c_str());
}
void startAp() {
  if (apActive) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,8,1), IPAddress(192,168,8,1), IPAddress(255,255,255,0));
  apActive = WiFi.softAP("NPB-Monitor", apPassword.c_str());
}
}
const char* token() { return DEVICE_TOKEN; }
void begin() {
  prefs.begin("npb-network", false);
  Credentials saved;
  if (prefs.getBytesLength("credentials") == sizeof(saved) && prefs.getBytes("credentials", &saved, sizeof(saved)) == sizeof(saved)) {
    saved.ssid[32] = 0; saved.password[63] = 0;
    ssid = saved.ssid; password = saved.password; credentialsSaved = true;
  } else {
    ssid = WIFI_SSID; password = WIFI_PASSWORD;
    if (ssid.length()) {
      strlcpy(saved.ssid, ssid.c_str(), sizeof(saved.ssid)); strlcpy(saved.password, password.c_str(), sizeof(saved.password));
      credentialsSaved = prefs.putBytes("credentials", &saved, sizeof(saved)) == sizeof(saved);
    }
  }
  apPassword = prefs.getString("ap-password", "");
  if (apPassword.length() != 16) {
    char value[17];
    snprintf(value, sizeof(value), "%08lX%08lX", (unsigned long)esp_random(), (unsigned long)esp_random());
    apPassword = value;
    prefs.putString("ap-password", apPassword);
  }
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    reason = info.wifi_sta_disconnected.reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  disconnectedAt = millis();
  if (ssid.length()) connect();
  else startAp();
}
// Called only by the HTTP task after it has authenticated the configuration request.
bool saveCredentials(const char* name, const char* pass) {
  const size_t n = strlen(name), p = strlen(pass);
  if (n == 0 || n > 32 || p > 63 || (p > 0 && p < 8)) return false;
  Credentials saved;
  strlcpy(saved.ssid, name, sizeof(saved.ssid)); strlcpy(saved.password, pass, sizeof(saved.password));
  if (prefs.putBytes("credentials", &saved, sizeof(saved)) != sizeof(saved)) return false;
  credentialsSaved = true;
  ssid = name; password = pass;
  reconnectAt = millis() + 750; // Let the HTTP response reach the client first.
  return true;
}
void service() {
  const uint32_t now = millis();
  if (reconnectAt && int32_t(now - reconnectAt) >= 0) {
    reconnectAt = 0; WiFi.disconnect(false, false); disconnectedAt = now; connect();
  }
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    reason = 0;
    mdns = MDNS.begin(HOSTNAME);
    if (mdns) MDNS.addService("http", "tcp", 80);
    if (apActive) { WiFi.softAPdisconnect(true); apActive = false; WiFi.mode(WIFI_STA); }
    Serial.printf("NETWORK connected hostname=%s ip=%s mdns=%d\n", HOSTNAME, WiFi.localIP().toString().c_str(), mdns);
  } else if (!connected && wasConnected) {
    if (mdns) MDNS.end();
    mdns = false;
    disconnectedAt = now;
  }
  if (!connected) {
    if (now - disconnectedAt >= 15000 || !ssid.length()) startAp();
    if (ssid.length() && now - lastAttempt >= 30000) connect();
  }
  wasConnected = connected;
  if (now - lastSnapshot < 250) return;
  lastSnapshot = now;
  Snapshot next;
  next.connected = connected; next.apActive = apActive; next.mdns = mdns;
  next.credentialsSaved = credentialsSaved; next.disconnectReason = reason;
  next.rssi = connected ? WiFi.RSSI() : 0;
  strlcpy(next.ssid, ssid.c_str(), sizeof(next.ssid));
  if (auto* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
    const char* hostname = nullptr;
    if (esp_netif_get_hostname(station, &hostname) == ESP_OK && hostname) strlcpy(next.dhcpHostname, hostname, sizeof(next.dhcpHostname));
  }
  strlcpy(next.apPassword, apPassword.c_str(), sizeof(next.apPassword));
  strlcpy(next.ip, (connected ? WiFi.localIP() : IPAddress(0,0,0,0)).toString().c_str(), sizeof(next.ip));
  portENTER_CRITICAL(&mux);
  shared = next;
  portEXIT_CRITICAL(&mux);
}
Snapshot snapshot() {
  portENTER_CRITICAL(&mux);
  Snapshot result = shared;
  portEXIT_CRITICAL(&mux);
  return result;
}
}
