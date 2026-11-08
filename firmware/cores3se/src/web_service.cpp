#include "web_service.h"
#include "web_page.h"
#include "web_controls.h"
#include "can_monitor.h"
#include "network_service.h"
#include "history.h"
#include "charger_control.h"
#include "display_ui.h"
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <driver/twai.h>
#include "local_web_server.h"
#include <esp_system.h>

namespace webservice {
namespace {
LocalWebServer server(80);
history::Ring samples;
bool uploadAuthorized = false, uploadComplete = false;
uint32_t rebootAt = 0;
String activationNotice;
Preferences controlPreferences;
String quoted(const String& value) {
  String out = "\"";
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = value[i];
    if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
    else if (c < 0x20) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", c); out += escaped; }
    else out += char(c);
  }
  return out + '"';
}
String boolean(bool value) { return value ? "true" : "false"; }
void add(String& out, const char* key, const String& value) {
  if (out[out.length() - 1] != '{') out += ',';
  out += quoted(key) + ':' + value;
}
String value(const npb::State& s, uint16_t command, uint32_t now) {
  const auto& reg = s.get(command);
  return reg.fresh(now) ? String(npb::decodedValue(command, reg.raw), 2) : "null";
}
void sendJson(int code, const String& out) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}
String networkJson() {
  const auto net = network::snapshot();
  String out = "{";
  add(out, "connected", boolean(net.connected)); add(out, "ip", quoted(net.ip));
  add(out, "ssid", quoted(net.ssid)); add(out, "rssi", String(net.rssi));
  add(out, "mdns", boolean(net.mdns)); add(out, "saved", boolean(net.credentialsSaved));
  add(out, "dhcp_hostname", quoted(net.dhcpHostname));
  add(out, "ap_active", boolean(net.apActive)); add(out, "ap_ip", quoted(net.apIp));
  add(out, "disconnect_reason", String(net.disconnectReason));
  return out + '}';
}
void status() {
  const auto state = canbus::snapshot();
  const auto& s = state.charger; const auto& b = state.bus;
  const uint32_t now = millis();
  String out; out.reserve(6500); out = "{";
  add(out, "firmware", quoted(VERSION)); add(out, "hostname", quoted(network::HOSTNAME));
  add(out, "uptime_ms", String(now)); add(out, "heap_free", String(ESP.getFreeHeap()));
  add(out, "reset_reason", String(int(esp_reset_reason())));
  add(out, "controls_enabled", boolean(canbus::controlsEnabled())); add(out, "can_ready", boolean(b.ready));
  add(out, "poll_paused", boolean(canbus::paused()));
  add(out, "activation_notice", quoted(activationNotice));
  add(out, "ac_cycle_pending", boolean(canbus::acCyclePending()));
  control::ChargeReview review;
  for (size_t i = 0; i < 7; ++i) review.values[i] = s.get(control::REVIEW_COMMANDS[i]).raw;
  const char* chargeError = control::chargeError(s, review, now);
  if (!b.ready || b.state != TWAI_STATE_RUNNING || b.txErrors || b.rxErrors || now - b.lastErrorAt < 2000) chargeError = "Waiting for healthy CAN";
  if (canbus::acCyclePending()) chargeError = "Confirm the required AC power cycle in Controls first";
  add(out, "charge_ready", boolean(chargeError == nullptr));
  add(out, "charge_block_reason", quoted(chargeError ? chargeError : ""));
  const auto screen = displayui::status();
  String display = "{";
  add(display, "page", String(screen.page)); add(display, "touches", String(screen.touches));
  add(display, "confirming", boolean(screen.confirming)); add(display, "holding", boolean(screen.holding));
  add(display, "local_job", String(screen.localJob)); add(out, "display", display + '}');
  const auto job = canbus::controlStatus();
  String commandResult = "{";
  add(commandResult, "id", String(job.id)); add(commandResult, "state", quoted(job.state));
  add(commandResult, "error", quoted(job.error)); add(commandResult, "command", String(job.command));
  add(commandResult, "requested_raw", String(job.requested)); add(commandResult, "observed_raw", String(job.observed));
  add(commandResult, "completed_steps", String(job.step)); add(commandResult, "total_steps", String(job.count));
  add(out, "control", commandResult + '}');
  add(out, "can_online", boolean(b.ready && s.online(now)));
  add(out, "voltage_v", value(s, 0x0060, now)); add(out, "current_a", value(s, 0x0061, now));
  add(out, "temperature_c", value(s, 0x0062, now));
  add(out, "power_w", s.get(0x0060).fresh(now) && s.get(0x0061).fresh(now)
    ? String(s.get(0x0060).raw * 0.01f * s.get(0x0061).raw * 0.01f, 2) : "null");
  add(out, "target_voltage_v", value(s, 0x0020, now)); add(out, "target_current_a", value(s, 0x0030, now));
  add(out, "phase", quoted(npb::phase(s, now))); add(out, "mode", quoted(npb::mode(s, now)));
  add(out, "operation_on", s.get(0x0000).fresh(now) ? boolean(s.get(0x0000).raw != 0) : "null");
  add(out, "fault_flags", s.get(0x0040).fresh(now) ? String(s.get(0x0040).raw) : "null");
  add(out, "status_fresh", boolean(npb::statusFresh(s, now)));
  add(out, "rx_frames", String(s.received)); add(out, "tx_frames", String(b.txQueued));
  String warnings = "[";
  for (const auto& flag : npb::WARNINGS) if (npb::active(s, flag, now)) {
    if (warnings.length() > 1) warnings += ',';
    warnings += quoted(flag.name);
  }
  add(out, "warnings", warnings + ']');
  String bus = "{";
  add(bus, "state", quoted(canbus::stateName(b))); add(bus, "driver_state", String(b.state));
  add(bus, "last_error", String(b.error)); add(bus, "tx_error_counter", String(b.txErrors));
  add(bus, "rx_error_counter", String(b.rxErrors)); add(bus, "tx_failed", String(b.failed));
  add(bus, "tx_rejected", String(b.txRejected)); add(bus, "bus_errors", String(b.busErrors));
  add(bus, "invalid_rx", String(b.invalidRx)); add(bus, "other_rx", String(b.otherRx));
  add(bus, "rx_missed", String(b.missed)); add(bus, "recoveries", String(b.recoveries));
  add(bus, "last_service_age_ms", String(uint32_t(now - b.lastService)));
  add(bus, "max_service_gap_ms", String(b.maxServiceGap));
  add(out, "can", bus + '}'); add(out, "network", networkJson());
  String registers = "[";
  for (auto command : npb::COMMANDS) {
    if (registers.length() > 1) registers += ',';
    const auto& reg = s.get(command);
    String item = "{";
    add(item, "command", String(command)); add(item, "raw", reg.seen ? String(reg.raw) : "null");
    add(item, "value", value(s, command, now)); add(item, "quality", quoted(reg.quality(now)));
    add(item, "age_ms", reg.seen ? String(uint32_t(now - reg.updated)) : "null");
    registers += item + '}';
  }
  add(out, "registers", registers + ']'); sendJson(200, out + '}');
}
void historyHandler() {
  const uint32_t now = millis();
  server.sendHeader("Cache-Control", "no-store"); server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String chunk; chunk.reserve(2000); chunk = "[";
  bool comma = false;
  for (size_t i = 0; i < samples.count(); ++i) {
    const auto& sample = samples.at(i);
    if (uint32_t(now - sample.ms) > history::RETENTION_MS) continue;
    if (comma) chunk += ',';
    comma = true;
    chunk += "{\"age_ms\":" + String(uint32_t(now - sample.ms));
    chunk += ",\"voltage_v\":" + (sample.valid ? String(sample.voltage * .01f, 2) : "null");
    chunk += ",\"current_a\":" + (sample.valid ? String(sample.current * .01f, 2) : "null") + '}';
    if (chunk.length() > 1600) { server.sendContent(chunk); chunk = ""; delay(1); }
  }
  server.sendContent(chunk + ']'); server.sendContent("");
}
void trafficHandler() {
  static canbus::Traffic frames[canbus::TRAFFIC_CAP];
  const size_t count = canbus::traffic(frames, canbus::TRAFFIC_CAP);
  const uint32_t now = millis();
  String out; out.reserve(8000); out = "[";
  for (size_t i = 0; i < count; ++i) {
    const auto& t = frames[i]; if (i) out += ',';
    String item = "{";
    add(item, "age_ms", String(uint32_t(now - t.ms))); add(item, "tx", boolean(t.tx));
    add(item, "accepted", boolean(t.accepted)); add(item, "id", String(t.frame.id));
    add(item, "extended", boolean(t.frame.extended)); add(item, "rtr", boolean(t.frame.rtr));
    add(item, "dlc", String(t.frame.length));
    String data = "[";
    for (uint8_t j = 0; j < min(uint8_t(8), t.frame.length); ++j) { if (j) data += ','; data += String(t.frame.data[j]); }
    add(item, "data", data + ']'); out += item + '}';
  }
  sendJson(200, out + ']');
}
bool authorized() { return strlen(network::token()) >= 32 && server.header("X-NPB-Token") == network::token(); }
bool numberArg(const char* name, double& value) {
  if (!server.hasArg(name) || !server.arg(name).length()) return false;
  const String input = server.arg(name); char* end = nullptr;
  value = strtod(input.c_str(), &end);
  return end != input.c_str() && *end == 0 && isfinite(value);
}
bool integerArg(const char* name, uint32_t& value, uint32_t maximum) {
  if (!server.hasArg(name) || !server.arg(name).length()) return false;
  const String input = server.arg(name); char* end = nullptr;
  if (input[0] == '-') return false;
  const unsigned long parsed = strtoul(input.c_str(), &end, 0);
  if (end == input.c_str() || *end || parsed > maximum) return false;
  value = parsed; return true;
}
void submitFrames(const npb::Frame* frames, size_t count, const control::ChargeReview* review = nullptr) {
  uint32_t id = 0;
  const char* error = canbus::submit(frames, count, id, review);
  if (error) { sendJson(409, String("{\"error\":") + quoted(error) + '}'); return; }
  for (size_t i = 0; i < count; ++i) if (frames[i].length > 2 && strlen(control::notice(control::command(frames[i])))) {
    if (control::command(frames[i]) == 0x00B4) {
      canbus::setAcCyclePending(true);
      controlPreferences.putBool("ac_pending", true);
    }
    if (control::command(frames[i]) == 0x00B4 || activationNotice.indexOf("AC power cycle is required") < 0) activationNotice = control::notice(control::command(frames[i]));
  }
  sendJson(202, String("{\"queued\":true,\"id\":") + id + '}');
}
void controlHandler() {
  if (!authorized()) { sendJson(403, "{\"error\":\"Device key required\"}"); return; }
  const String action = server.arg("action");
  if (action == "acknowledge_ac_cycle") {
    if (!canbus::controlsEnabled() || server.arg("confirm") != "true") { sendJson(409, "{\"error\":\"Unlock and confirm a completed manual AC power cycle\"}"); return; }
    controlPreferences.putBool("ac_pending", false); canbus::setAcCyclePending(false); activationNotice = "";
    sendJson(200, "{\"ok\":true}"); return;
  }
  if (action == "unlock" || action == "lock") {
    const char* error = canbus::setControlsEnabled(action == "unlock");
    if (error) sendJson(409, String("{\"error\":") + quoted(error) + '}');
    else sendJson(200, "{\"ok\":true}");
    return;
  }
  if (action == "set_paused") {
    if (server.arg("paused") != "true" && server.arg("paused") != "false") { sendJson(400, "{\"error\":\"paused must be true or false\"}"); return; }
    canbus::setPaused(server.arg("paused") == "true"); sendJson(200, "{\"ok\":true}"); return;
  }
  npb::Frame frames[20]; size_t count = 0;
  if (action == "charge_now") {
    if (server.arg("confirm_battery") != "true" || server.arg("confirm_temperature") != "true") { sendJson(400, "{\"error\":\"Confirm battery settings and battery temperature\"}"); return; }
    if (canbus::acCyclePending()) { sendJson(409, "{\"error\":\"Manual AC power cycle acknowledgment required\"}"); return; }
    control::ChargeReview review;
    const char* keys[] = {"cc", "cv", "float", "taper", "restart", "curve_config", "system_config"};
    for (size_t i = 0; i < 7; ++i) {
      uint32_t raw;
      if (!integerArg(keys[i], raw, 65535)) { sendJson(400, "{\"error\":\"Review all current charge settings\"}"); return; }
      review.values[i] = raw;
    }
    control::encode(0, 0, frames[0]); control::encode(0, 1, frames[1]);
    submitFrames(frames, 2, &review); return;
  } else if (action == "read_command") {
    uint32_t command = 0;
    if (integerArg("command", command, 65535) && npb::makeRead(command, 0, frames[0])) count = 1;
  } else if (action == "scan_addresses") {
    const uint16_t commands[] = {0x0000, 0x0060, 0x0061, 0x00B8, 0x00C1};
    for (uint8_t address = 0; address < 4; ++address) for (auto command : commands) npb::makeRead(command, address, frames[count++]);
  } else if (action == "initialize_psu" || action == "charger_mode_default" || action == "enable_eeprom_writes" || action == "disable_eeprom_writes") {
    if (server.arg("confirm") != "true") { sendJson(400, "{\"error\":\"Confirm the mode/persistence change\"}"); return; }
    if (action == "charger_mode_default") control::encode(0x00B4, 0x0084, frames[count++]);
    else {
      if (action == "initialize_psu") control::encode(0x00B4, 0x0004, frames[count++]);
      if (!control::persistenceWrite(canbus::snapshot().charger, action != "enable_eeprom_writes", millis(), frames[count])) {
        sendJson(409, "{\"error\":\"Fresh, supported system configuration required\"}"); return;
      }
      ++count;
    }
  } else if (const auto* spec = control::spec(action.c_str())) {
    double value = 0;
    if (spec->command == 0) {
      if (server.arg("enabled") == "true" || server.arg("enabled") == "false") {
        if (control::encode(0, server.arg("enabled") == "true" ? 1 : 0, frames[0])) count = 1;
      }
    } else if (spec->command == 0x00B4 || spec->command == 0x00C2) {
      uint32_t low = 0, high = 0;
      if (server.arg("confirm") != "true") { sendJson(400, "{\"error\":\"Confirm the raw configuration change\"}"); return; }
      if (integerArg("low", low, 255) && integerArg("high", high, 255) && control::encode(spec->command, low | high << 8, frames[0])) count = 1;
    } else if (numberArg("value", value) && control::encode(spec->command, value, frames[0])) count = 1;
  }
  if (!count) { sendJson(400, "{\"error\":\"Unknown action or invalid/out-of-range value\"}"); return; }
  submitFrames(frames, count);
}
void rawHandler() {
  if (!authorized()) { sendJson(403, "{\"error\":\"Device key required\"}"); return; }
  uint32_t id = 0; npb::Frame frame; frame.extended = true;
  if (!integerArg("can_id", id, 0x1fffffff)) { sendJson(400, "{\"error\":\"Invalid CAN ID\"}"); return; }
  frame.id = id;
  String input = server.arg("data"); input.replace(',', ' ');
  const char* cursor = input.c_str();
  while (*cursor) {
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (!*cursor) break;
    char* end = nullptr; unsigned long byte = strtoul(cursor, &end, 0);
    if (*cursor == '-' || end == cursor || (*end && *end != ' ' && *end != '\t') || byte > 255 || frame.length >= 8) { sendJson(400, "{\"error\":\"Use up to 8 decimal or 0x-prefixed bytes\"}"); return; }
    frame.data[frame.length++] = byte; cursor = end;
  }
  if (frame.length > 2 && server.arg("confirm") != "true") { sendJson(400, "{\"error\":\"Confirm the raw write\"}"); return; }
  submitFrames(&frame, 1);
}
void firmwareUpload() {
  auto& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    uploadComplete = false; uploadAuthorized = authorized();
    if (uploadAuthorized && !Update.begin(UPDATE_SIZE_UNKNOWN)) uploadAuthorized = false;
  } else if (upload.status == UPLOAD_FILE_WRITE && uploadAuthorized) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { Update.abort(); uploadAuthorized = false; }
  } else if (upload.status == UPLOAD_FILE_END && uploadAuthorized) {
    uploadComplete = Update.end(true);
    if (!uploadComplete) { Update.abort(); uploadAuthorized = false; }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); uploadAuthorized = false; uploadComplete = false;
  }
}
void run(void*) {
  network::begin();
  const bool preferencesReady = controlPreferences.begin("npb-control", false);
  canbus::setAcCyclePending(!preferencesReady || controlPreferences.getBool("ac_pending", false));
  if (canbus::acCyclePending()) activationNotice = control::notice(0x00B4);
  const char* headers[] = {"X-NPB-Token"}; server.collectHeaders(headers, 1);
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
  server.on("/controls.js", HTTP_GET, []() { server.send_P(200, "text/javascript; charset=utf-8", CONTROL_SCRIPT); });
  server.on("/api/status", HTTP_GET, status); server.on("/api/history", HTTP_GET, historyHandler);
  server.on("/api/traffic", HTTP_GET, trafficHandler);
  server.on("/api/screen.bmp", HTTP_GET, []() {
    if (!authorized()) { sendJson(403, "{\"error\":\"Device key required\"}"); return; }
    constexpr size_t bytes = 320 * 240 * 3;
    auto* pixels = static_cast<uint8_t*>(ps_malloc(bytes));
    if (!pixels) { sendJson(503, "{\"error\":\"Screenshot memory unavailable\"}"); return; }
    if (!displayui::capture(pixels, bytes)) { free(pixels); sendJson(503, "{\"error\":\"Display busy\"}"); return; }
    // Top-down, 24-bit BMP. RGB readback is converted to BMP's BGR byte order.
    for (size_t i = 0; i < bytes; i += 3) { const auto red = pixels[i]; pixels[i] = pixels[i+2]; pixels[i+2] = red; }
    uint8_t header[54] = {'B', 'M'};
    auto u32 = [&](size_t at, uint32_t n) { for (int i = 0; i < 4; ++i) header[at+i] = n >> (8*i); };
    u32(2, bytes + 54); u32(10, 54); u32(14, 40); u32(18, 320); u32(22, uint32_t(-240));
    header[26] = 1; header[28] = 24; u32(34, bytes);
    server.sendHeader("Cache-Control", "no-store"); server.setContentLength(bytes + 54);
    server.send(200, "image/bmp", ""); auto client = server.client(); client.write(header, 54);
    for (size_t offset = 0; offset < bytes && client.connected();) {
      const size_t sent = client.write(pixels + offset, min(size_t(2048), bytes - offset));
      if (!sent) break;
      offset += sent; delay(1);
    }
    free(pixels);
  });
  server.on("/api/config", HTTP_GET, []() { sendJson(200, networkJson()); });
  server.on("/api/config", HTTP_POST, []() {
    if (!authorized()) { sendJson(403, "{\"error\":\"Configuration key required\"}"); return; }
    if (!server.hasArg("ssid") || !server.hasArg("password") || !network::saveCredentials(server.arg("ssid").c_str(), server.arg("password").c_str())) {
      sendJson(400, "{\"error\":\"Use a 1-32 byte SSID and an empty or 8-63 byte password; settings must save successfully\"}"); return;
    }
    sendJson(200, "{\"saved\":true}");
  });
  server.on("/api/firmware", HTTP_POST, []() {
    const bool ok = authorized() && uploadAuthorized && uploadComplete;
    uploadAuthorized = false; uploadComplete = false;
    if (!ok) { sendJson(403, "{\"error\":\"Update denied or invalid image\"}"); return; }
    sendJson(200, "{\"ok\":true,\"rebooting\":true}"); rebootAt = millis() + 1000;
  }, firmwareUpload);
  server.on("/api/control", HTTP_POST, controlHandler); server.on("/api/raw", HTTP_POST, rawHandler);
  server.onNotFound([]() { sendJson(404, "{\"error\":\"Not found\"}"); });
  server.begin();
  for (;;) {
    network::service(); samples.record(canbus::snapshot().charger, millis()); server.handleClient();
    if (rebootAt && int32_t(millis() - rebootAt) >= 0) ESP.restart();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
}
bool begin() { return xTaskCreatePinnedToCore(run, "npb-http", 12288, nullptr, 1, nullptr, 0) == pdPASS; }
}
