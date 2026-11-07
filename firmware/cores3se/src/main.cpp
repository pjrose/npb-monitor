#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_system.h>
#include <driver/twai.h>
#include <M5Unified.h>

// Mean Well NPB-450-24NFC: read-only ESP32-S3 implementation.
// Hardware: CoreS3-SE + Base DIN PORT.C + isolated Unit CAN.
// No charger writes, relays, MQTT broker or EEPROM changes in this build.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

constexpr gpio_num_t CAN_TX = GPIO_NUM_17;
constexpr gpio_num_t CAN_RX = GPIO_NUM_18;
constexpr uint32_t TX_ID = 0x000C0100;
constexpr uint32_t RX_ID = 0x000C0000;
constexpr uint32_t STALE_MS = 15000;
constexpr uint32_t POLL_MS = 180;
constexpr uint32_t HISTORY_MS = 300000;
constexpr size_t HISTORY_CAP = 300; // 25h at 5-minute intervals
constexpr uint16_t COMMANDS[] = {
  0x0000, 0x0020, 0x0030, 0x0040, 0x0060, 0x0061, 0x0062,
  0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B8, 0x00B9, 0x00C1, 0x00C2
};

WebServer server(80);
struct Reading {
  float value = NAN;
  uint32_t updated = 0;
  bool valid = false;
};
Reading voltage, current, temperature, targetVoltage, targetCurrent;
uint32_t lastCanRx = 0, lastPoll = 0, lastDisplay = 0, lastHistory = 0;
uint32_t rxFrames = 0, txFrames = 0, badFrames = 0;
bool canReady = false, apMode = false;
uint8_t chargeFlagsLow = 0, chargeFlagsHigh = 0, faultFlags = 0;
uint32_t chargeFlagsAt = 0, faultFlagsAt = 0;
size_t pollIndex = 0;
String apPassword;

struct HistorySample {uint32_t ms; float v; float a;};
HistorySample history[HISTORY_CAP];
size_t historyHead = 0, historyCount = 0;

bool fresh(const Reading& r) {
  return r.valid && (uint32_t)(millis() - r.updated) < STALE_MS;
}
void update(Reading& r, float v) {
  r.value = v; r.updated = millis(); r.valid = true;
}
String jsonNumber(const Reading& r) {
  return fresh(r) ? String(r.value, 2) : "null";
}
String phase() {
  if (millis() - chargeFlagsAt >= STALE_MS || !chargeFlagsAt) return "Unknown";
  if (chargeFlagsLow & 0x01) return "Fully charged";
  if (chargeFlagsLow & 0x02) return "Constant current";
  if (chargeFlagsLow & 0x04) return "Constant voltage";
  if (chargeFlagsLow & 0x08) return "Float";
  return "Idle";
}
bool canOnline() {
  return canReady && rxFrames && millis() - lastCanRx < STALE_MS;
}

bool initCan() {
  twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_NORMAL);
  general.tx_queue_len = 12;
  general.rx_queue_len = 48;
  twai_timing_config_t timing = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  esp_err_t err = twai_driver_install(&general, &timing, &filter);
  if (err != ESP_OK) { Serial.printf("TWAI install failed: %d\n", err); return false; }
  err = twai_start();
  if (err != ESP_OK) { Serial.printf("TWAI start failed: %d\n", err); twai_driver_uninstall(); return false; }
  return true;
}

void readReply(const twai_message_t& msg) {
  if (!msg.extd || msg.rtr || msg.identifier != RX_ID || msg.data_length_code < 3) {
    ++badFrames; return;
  }
  const uint16_t cmd = (uint16_t)msg.data[0] | ((uint16_t)msg.data[1] << 8);
  const uint16_t raw = msg.data_length_code >= 4 ?
    (uint16_t)msg.data[2] | ((uint16_t)msg.data[3] << 8) : 0;
  const uint32_t now = millis();
  switch (cmd) {
    case 0x0020: if (msg.data_length_code >= 4) update(targetVoltage, raw * 0.01f); break;
    case 0x0030: if (msg.data_length_code >= 4) update(targetCurrent, raw * 0.01f); break;
    case 0x0060: if (msg.data_length_code >= 4) update(voltage, raw * 0.01f); break;
    case 0x0061: if (msg.data_length_code >= 4) update(current, raw * 0.01f); break;
    case 0x0062: if (msg.data_length_code >= 4) update(temperature, raw * 0.1f); break;
    case 0x0040: faultFlags = msg.data[2]; faultFlagsAt = now; break;
    case 0x00B8:
      if (msg.data_length_code >= 4) {
        chargeFlagsLow = msg.data[2]; chargeFlagsHigh = msg.data[3]; chargeFlagsAt = now;
      }
      break;
    default: break;
  }
  lastCanRx = now;
  ++rxFrames;
}

void serviceCan() {
  if (!canReady) return;
  twai_message_t msg;
  for (int i = 0; i < 32 && twai_receive(&msg, 0) == ESP_OK; i++) readReply(msg);
  if (millis() - lastPoll < POLL_MS) return;
  lastPoll = millis();
  uint16_t cmd = COMMANDS[pollIndex++ % (sizeof(COMMANDS) / sizeof(COMMANDS[0]))];
  twai_message_t request = {};
  request.identifier = TX_ID;
  request.extd = 1;
  request.data_length_code = 2;
  request.data[0] = cmd & 0xff;
  request.data[1] = cmd >> 8;
  if (twai_transmit(&request, 0) == ESP_OK) ++txFrames;
}

void recordHistory() {
  if (millis() - lastHistory < HISTORY_MS) return;
  lastHistory = millis();
  if (!fresh(voltage) || !fresh(current)) return;
  history[historyHead] = { millis(), voltage.value, current.value };
  historyHead = (historyHead + 1) % HISTORY_CAP;
  if (historyCount < HISTORY_CAP) ++historyCount;
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>NPB Charger Monitor</title>
<style>
:root{color-scheme:dark;font-family:system-ui,sans-serif;background:#0a111c;color:#e6edf6}
body{max-width:850px;margin:0 auto;padding:24px}
h1{font-size:1.6rem;margin:0 0 6px}small{color:#9bacbd}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;margin:22px 0}
.card{background:#162334;padding:20px;border:1px solid #283b50;border-radius:13px}
.label{color:#afbdce;font-size:.9rem}.big{font-size:2.1rem;font-weight:700;margin-top:5px}
.ok{color:#57d6aa}.bad{color:#ff8d8d}strong{font-weight:650}
table{border-collapse:collapse;width:100%;margin-top:15px}
td{padding:10px;border-bottom:1px solid #283b50}
td:last-child{text-align:right}
.note{color:#afbdce;font-size:.88rem;margin-top:25px}
</style></head><body>
<h1>NPB-450 Monitor</h1><small>CoreS3-SE · direct CAN · read-only</small>
<div class="grid">
<div class="card"><div class="label">Output voltage</div><div class="big" id="v">--</div></div>
<div class="card"><div class="label">Output current</div><div class="big" id="a">--</div></div>
<div class="card"><div class="label">Output power</div><div class="big" id="w">--</div></div>
<div class="card"><div class="label">Internal temperature</div><div class="big" id="t">--</div></div>
</div>
<div class="card">
<table>
<tr><td>CAN bus</td><td id="bus">--</td></tr>
<tr><td>Charging phase</td><td id="phase">--</td></tr>
<tr><td>Fault status</td><td id="fault">--</td></tr>
<tr><td>Voltage setpoint</td><td id="sv">--</td></tr>
<tr><td>Current setpoint</td><td id="sa">--</td></tr>
<tr><td>Received frames</td><td id="rx">0</td></tr>
</table>
</div>
<p class="note">Values are hidden when stale. This page does not issue charger control commands.
Local API: <a href="/api/status">/api/status</a> · <a href="/api/history">/api/history</a>.</p>
<script>
const $=id=>document.getElementById(id);
const f=(v,d=2)=>v===null?'--':Number(v).toFixed(d);
async function refresh(){
try{
let d=await (await fetch('/api/status',{cache:'no-store'})).json();
$('v').textContent=f(d.voltage_v)+' V';
$('a').textContent=f(d.current_a)+' A';
$('w').textContent=d.voltage_v===null||d.current_a===null?'--':f(d.voltage_v*d.current_a,0)+' W';
$('t').textContent=f(d.temperature_c,1)+' °C';
$('bus').textContent=d.can_online?'Connected':'No recent reply';
$('bus').className=d.can_online?'ok':'bad';
$('phase').textContent=d.phase;
$('fault').textContent=d.fault_flags===null?'Unknown':d.fault_flags===0?'None':'0x'+d.fault_flags.toString(16).padStart(2,'0');
$('sv').textContent=f(d.target_voltage_v)+' V';
$('sa').textContent=f(d.target_current_a)+' A';
$('rx').textContent=d.rx_frames;
}catch(e){$('bus').textContent='Connection lost';$('bus').className='bad'}
}
refresh();setInterval(refresh,1500);
</script></body></html>)HTML";

void statusHandler() {
  String out;
  out.reserve(520);
  out = "{\"can_ready\":";
  out += canReady ? "true" : "false";
  out += ",\"can_online\":";
  out += canOnline() ? "true" : "false";
  out += ",\"voltage_v\":" + jsonNumber(voltage);
  out += ",\"current_a\":" + jsonNumber(current);
  out += ",\"temperature_c\":" + jsonNumber(temperature);
  out += ",\"target_voltage_v\":" + jsonNumber(targetVoltage);
  out += ",\"target_current_a\":" + jsonNumber(targetCurrent);
  out += ",\"phase\":\"" + phase() + "\"";
  out += ",\"fault_flags\":";
  out += (faultFlagsAt && millis() - faultFlagsAt < STALE_MS) ? String(faultFlags) : "null";
  out += ",\"rx_frames\":" + String(rxFrames);
  out += ",\"tx_frames\":" + String(txFrames);
  out += ",\"bad_frames\":" + String(badFrames);
  out += ",\"uptime_ms\":" + String(millis()) + "}";
  server.send(200, "application/json", out);
}
void historyHandler() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  for (size_t i=0; i<historyCount; i++) {
    const size_t index = (historyHead + HISTORY_CAP - historyCount + i) % HISTORY_CAP;
    const HistorySample& s = history[index];
    String item = (i ? "," : "");
    item += "{\"uptime_ms\":" + String(s.ms) + ",\"voltage_v\":" + String(s.v,2);
    item += ",\"current_a\":" + String(s.a,2) + "}";
    server.sendContent(item);
    delay(0);
  }
  server.sendContent("]");
  server.sendContent("");
}
String generateApPassword() {
  char buffer[17];
  uint64_t random = ((uint64_t)esp_random() << 32) | esp_random();
  snprintf(buffer, sizeof(buffer), "%016llX", (unsigned long long)random);
  return String(buffer);
}
void startWifi() {
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID)) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (int i=0; i<40 && WiFi.status()!=WL_CONNECTED; i++) delay(250);
  }
  if (WiFi.status()==WL_CONNECTED) {
    Serial.printf("WiFi STA: http://%s\n", WiFi.localIP().toString().c_str());
    return;
  }
  apMode = true;
  apPassword = generateApPassword();
  WiFi.mode(WIFI_AP);
  WiFi.softAP("NPB-Monitor", apPassword.c_str());
  Serial.printf("AP NPB-Monitor password: %s\n", apPassword.c_str());
  Serial.printf("Web: http://%s\n", WiFi.softAPIP().toString().c_str());
}

void drawScreen() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8,6);
  M5.Display.setTextColor(TFT_CYAN);
  M5.Display.println("NPB-450 Monitor");
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(8,43);
  M5.Display.printf("V: %s\n", fresh(voltage) ? String(voltage.value,2).c_str() : "--");
  M5.Display.setCursor(8,80);
  M5.Display.printf("A: %s\n", fresh(current) ? String(current.value,2).c_str() : "--");
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8,123);
  M5.Display.println(phase());
  M5.Display.setTextColor(canOnline() ? TFT_GREEN : TFT_RED);
  M5.Display.setCursor(8,153);
  M5.Display.printf("CAN %s  RX:%lu", canOnline()?"OK":"NO DATA", (unsigned long)rxFrames);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(8,182);
  M5.Display.printf("http://%s", (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str());
  if (apMode) {
    M5.Display.setCursor(8,200);
    M5.Display.println("AP: NPB-Monitor");
    M5.Display.setCursor(8,215);
    M5.Display.printf("Pass: %s", apPassword.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setCursor(8,12);
  M5.Display.println("Starting NPB Monitor...");
  canReady = initCan();
  startWifi();
  server.on("/", HTTP_GET, [](){server.send_P(200,"text/html; charset=utf-8",PAGE);});
  server.on("/api/status", HTTP_GET, statusHandler);
  server.on("/api/history", HTTP_GET, historyHandler);
  server.onNotFound([](){server.send(404,"text/plain","Not found");});
  server.begin();
  Serial.printf("TWAI %s at 250kbps, TX GPIO17 / RX GPIO18\n",canReady?"running":"FAILED");
  drawScreen();
}
void loop() {
  M5.update();
  serviceCan();
  recordHistory();
  server.handleClient();
  if (millis()-lastDisplay > 1000) {
    lastDisplay=millis();
    drawScreen();
  }
  delay(1);
}
