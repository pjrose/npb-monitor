#include <Arduino.h>
#include <ESPmDNS.h>
#include <M5Unified.h>
#include <M5_ACSSR.h>
#include <SD.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <driver/twai.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "config.h"

static constexpr uint32_t CONTROLLER_CAN_ID = 0x000C0100 | NPB_ADDRESS;
static constexpr uint32_t CHARGER_CAN_ID    = 0x000C0000 | NPB_ADDRESS;
static constexpr uint16_t POLL_COMMANDS[] = {
    0x0000, 0x0020, 0x0030, 0x0040, 0x0060, 0x0061, 0x0062, 0x00B0,
    0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B8, 0x00B9, 0x00C1, 0x00C2,
};

struct ChargerFlags {
  bool operationOn = false;
  bool overTemperature = false;
  bool outputOverVoltage = false;
  bool outputOverCurrent = false;
  bool outputShortCircuit = false;
  bool acAbnormal = false;
  bool outputOff = false;
  bool internalTempAbnormal = false;
  bool fullyCharged = false;
  bool constantCurrent = false;
  bool constantVoltage = false;
  bool floatMode = false;
  bool wakeUpUnfinished = false;
  bool tempCompShort = false;
  bool noBattery = false;
  bool ccTimeout = false;
  bool cvTimeout = false;
  bool floatTimeout = false;
  bool dcOutputNormal = false;
  bool initialState = false;
  bool eepromError = false;
  bool chargerMode = false;
  bool eepromWritesDisabled = false;
};

struct ChargerState {
  float outputVoltage = NAN;
  float outputCurrent = NAN;
  float outputPower = NAN;
  float temperature = NAN;
  float targetVoltage = NAN;
  float targetCurrent = NAN;
  uint32_t lastRxMs = 0;
  uint32_t lastTxMs = 0;
  uint32_t rxFrames = 0;
  uint32_t txFrames = 0;
  ChargerFlags flags;
};

struct HistorySample {
  uint64_t epochMs;
  float voltage;
  float current;
  float power;
  float temperature;
};

enum class Screen : uint8_t { Dashboard, Flags, Maintenance, Logs };

WebServer server(80);
M5_ACSSR acssr;
ChargerState charger;
HistorySample* history = nullptr;
size_t historyCapacity = 0;
size_t historyStart = 0;
size_t historyCount = 0;
Screen currentScreen = Screen::Dashboard;
String currentLogName;
bool sdReady = false;
bool acssrReady = false;
bool canReady = false;
bool powerCycleActive = false;
uint32_t powerCycleStartedMs = 0;
uint32_t nextPollStartMs = 0;
uint32_t lastPollTxMs = 0;
size_t pollIndex = 0;
uint32_t lastDrawMs = 0;
uint32_t lastSdPruneMs = 0;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>NPB CoreS3</title>
<style>
:root{color-scheme:dark;--bg:#101114;--panel:#1a1d21;--line:#343a42;--text:#f4f1ea;--muted:#aaa69b;--green:#58d184;--red:#ff6b6b;--amber:#f7bd52;--cyan:#76c7d8;--ink:#111317}
*{box-sizing:border-box}body{margin:0;background:linear-gradient(140deg,#101114,#171a1e 58%,#15120f);color:var(--text);font-family:Inter,system-ui,Segoe UI,sans-serif}
.shell{width:min(1180px,calc(100% - 28px));margin:0 auto;padding:22px 0 34px}.top{display:flex;justify-content:space-between;gap:14px;align-items:flex-start;margin-bottom:16px;flex-wrap:wrap}
.eyebrow{margin:0 0 4px;color:var(--cyan);font-size:.78rem;font-weight:800;text-transform:uppercase}h1{margin:0;font-size:clamp(2rem,7vw,4rem);line-height:.95}h2{font-size:1rem;margin:0}
.strip{display:flex;gap:8px;flex-wrap:wrap;justify-content:flex-end}.pill,.nav{display:inline-flex;align-items:center;min-height:2rem;border:1px solid var(--line);border-radius:999px;padding:0 .8rem;color:var(--muted);font-weight:800;font-size:.84rem;text-decoration:none}.good{background:var(--green)!important;color:#07130c!important;border-color:var(--green)!important}.bad{background:var(--red)!important;color:#1d0808!important;border-color:var(--red)!important}.nav{border-color:var(--cyan);color:var(--text);background:rgba(118,199,216,.12)}
.grid{display:grid;gap:14px}.dash{grid-template-columns:1fr 1.4fr}.cards{grid-template-columns:repeat(3,minmax(0,1fr))}.two{grid-template-columns:repeat(2,minmax(0,1fr))}
.panel,.metric,.hero{border:1px solid var(--line);border-radius:8px;background:rgba(26,29,33,.88);box-shadow:0 14px 42px rgba(0,0,0,.28)}.panel{padding:16px}.hero{display:grid;grid-template-columns:minmax(150px,1fr) 1fr;align-items:center;gap:22px;padding:24px;min-height:230px}
.battery{position:relative;display:grid;place-items:center;width:min(100%,250px);aspect-ratio:1.7;border:5px solid #d7d1c1;border-radius:8px;background:#0b0d10;overflow:hidden}.battery:before{content:"";position:absolute;left:0;bottom:0;width:var(--fill,40%);height:100%;background:linear-gradient(90deg,var(--green),var(--cyan))}.battery strong{position:relative;font-size:clamp(2rem,7vw,3.2rem)}
.mode{color:var(--muted);font-weight:800}.watts{display:block;font-size:clamp(2.6rem,9vw,5rem);line-height:1;margin:.4rem 0}.metric{min-height:116px;padding:16px}.metric span,.muted{color:var(--muted)}.metric strong{display:block;margin-top:16px;font-size:clamp(1.4rem,4vw,2rem)}
button{min-height:2.6rem;border:1px solid #4b535e;border-radius:7px;background:#e8e3d5;color:var(--ink);font-weight:800;padding:0 .9rem;cursor:pointer}.buttonrow{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}.danger{background:var(--red)}
.flags{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}.flag{display:flex;justify-content:space-between;gap:8px;border:1px solid var(--line);border-radius:7px;padding:.45rem .55rem;color:var(--muted);font-size:.86rem}.flag.active{background:var(--amber);border-color:var(--amber);color:var(--ink)}
.logs{display:grid;gap:8px;max-height:280px;overflow:auto}.log{display:flex;align-items:center;justify-content:space-between;gap:8px;border:1px solid var(--line);border-radius:7px;padding:10px;background:#111317;color:var(--muted)}a{color:var(--cyan)}
@media(max-width:760px){.shell{width:min(100% - 18px,1180px);padding-top:14px}.dash,.cards,.two,.hero{grid-template-columns:1fr}.strip{justify-content:flex-start}.buttonrow{grid-template-columns:1fr}.flags{grid-template-columns:1fr}.metric{min-height:92px}.hero{min-height:unset}.battery{width:100%;max-width:280px}.log{align-items:flex-start;flex-direction:column}}
</style>
</head>
<body><main class="shell">
<header class="top"><div><p class="eyebrow">Mean Well NPB-450-24NFC</p><h1>CoreS3 Console</h1></div><div class="strip"><span id="wifi" class="pill">WiFi</span><span id="can" class="pill">CAN</span><span id="relay" class="pill">ACSSR</span><a class="nav" href="/history.csv">CSV</a></div></header>
<section class="grid dash"><div class="hero"><div id="battery" class="battery"><strong id="voltage">-- V</strong></div><div><span id="mode" class="mode">Unknown</span><strong id="power" class="watts">-- W</strong><span class="muted">Output power</span></div></div><div class="grid cards"><article class="metric"><span>Output current</span><strong id="current">-- A</strong></article><article class="metric"><span>Temperature</span><strong id="temp">-- C</strong></article><article class="metric"><span>Operation</span><strong id="op">--</strong></article></div></section>
<section class="grid two" style="margin-top:14px"><section class="panel"><h2>Output Control</h2><p class="muted">Start/stop operation or power-cycle charger AC input for mode changes.</p><div class="buttonrow"><button onclick="operation(true)" class="good">Start</button><button onclick="operation(false)" class="danger">Stop</button><button onclick="powerCycle()">AC power cycle</button></div></section><section class="panel"><h2>Status Flags</h2><div id="flags" class="flags"></div></section></section>
<section class="grid two" style="margin-top:14px"><section class="panel"><h2>Maintenance</h2><p id="maint" class="muted">Loading</p></section><section class="panel"><h2>SD Session Logs</h2><div id="logs" class="logs"></div></section></section>
</main>
<script>
const flagLabels={overTemperature:"Over temperature",outputOverVoltage:"Output over-voltage",outputOverCurrent:"Output over-current",outputShortCircuit:"Output short circuit",acAbnormal:"AC abnormal",outputOff:"Output off",internalTempAbnormal:"Internal temp abnormal",fullyCharged:"Fully charged",constantCurrent:"Constant current",constantVoltage:"Constant voltage",floatMode:"Float",wakeUpUnfinished:"Wake-up unfinished",tempCompShort:"Temp compensation short",noBattery:"No battery",ccTimeout:"CC timeout",cvTimeout:"CV timeout",floatTimeout:"Float timeout",dcOutputNormal:"DC output normal",initialState:"Initial state",eepromError:"EEPROM error",chargerMode:"Charger mode",eepromWritesDisabled:"EEPROM writes disabled"};
const fmt=(v,d,u)=>Number.isFinite(Number(v))?`${Number(v).toFixed(d)}${u}`:"--";
async function post(path,body={}){const r=await fetch(path,{method:"POST",headers:{"content-type":"application/json"},body:JSON.stringify(body)});if(!r.ok)alert(await r.text())}
async function operation(enabled){await post("/api/operation",{enabled});refresh()}
async function powerCycle(){if(confirm("Turn charger AC off for 10 seconds, then back on?")){await post("/api/power-cycle");refresh()}}
async function refresh(){const s=await fetch("/api/state").then(r=>r.json());document.getElementById("wifi").textContent=s.ip||"WiFi";document.getElementById("wifi").className="pill good";document.getElementById("can").textContent=s.canReady?(s.lastRxAgeMs<5000?"CAN active":"CAN stale"):"CAN offline";document.getElementById("can").className="pill "+(s.canReady&&s.lastRxAgeMs<5000?"good":"bad");document.getElementById("relay").textContent=s.powerCycleActive?"AC cycling":(s.acssrOn?"AC on":"AC off");document.getElementById("relay").className="pill "+(s.acssrOn&&!s.powerCycleActive?"good":"bad");document.getElementById("voltage").textContent=fmt(s.outputVoltage,1," V");document.getElementById("current").textContent=fmt(s.outputCurrent,2," A");document.getElementById("power").textContent=fmt(s.outputPower,0," W");document.getElementById("temp").textContent=fmt(s.temperature,1," C");document.getElementById("op").textContent=s.flags.operationOn?"On":"Off";document.getElementById("mode").textContent=s.flags.chargerMode?"Charger mode":"PSU mode";document.getElementById("battery").style.setProperty("--fill",`${Math.max(0,Math.min(100,((s.outputVoltage||0)-21)/(29.2-21)*100))}%`);document.getElementById("flags").innerHTML=Object.entries(flagLabels).map(([k,l])=>`<div class="flag ${s.flags[k]?"active":""}"><span>${l}</span><strong>${s.flags[k]?"Yes":"No"}</strong></div>`).join("");document.getElementById("maint").innerHTML=`IP: ${s.ip}<br>mDNS: ${s.mdns}.local<br>SSID: ${s.ssid}<br>SD: ${s.sdReady?`${s.sdUsedMB} / ${s.sdTotalMB} MB`:"not mounted"}<br>Current log: ${s.currentLog||"--"}`;loadLogs()}
async function loadLogs(){const files=await fetch("/api/logs").then(r=>r.json()).catch(()=>[]);document.getElementById("logs").innerHTML=files.length?files.map(f=>`<div class="log"><span>${f.name}<br><small>${f.size} bytes</small></span><a href="/logs/${encodeURIComponent(f.name)}">Download</a></div>`).join(""):"<div class='log'>No SD logs yet.</div>"}
refresh();setInterval(refresh,1000);
</script></body></html>
)HTML";

uint64_t epochMsNow();
String isoNow();
String logFilenameNow();
String jsonBool(bool value);
String jsonNumber(float value, uint8_t digits);
String urlDecode(const String& input);
bool sendCanFrame(uint32_t id, const uint8_t* data, uint8_t len);
void sendRead(uint16_t command);
void setOperation(bool enabled);
void startPowerCycle();
void servicePowerCycle();
void serviceCanRx();
void servicePoll();
void decodeFrame(const twai_message_t& msg);
void addHistorySample();
void startLogSession();
void appendLogSample(const HistorySample& sample);
void enforceSdLimit();
bool deleteOldestLogFile();
void setupWeb();
void handleState();
void handleLogs();
void handleLogDownload(const String& filename);
void handleHistoryCsv();
void drawUi(bool force = false);
void handleTouch();

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(180);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(1);
  M5.Display.println("NPB CoreS3 starting...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(250);
    M5.Display.print(".");
  }
  M5.Display.println();

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", TIME_ZONE, 1);
  tzset();
  if (WiFi.status() == WL_CONNECTED) {
    MDNS.begin(MDNS_NAME);
    MDNS.addService("http", "tcp", 80);
  }

  acssrReady = acssr.begin(&Wire, ACSSR_SDA_PIN, ACSSR_SCL_PIN, ACSSR_DEFAULT_ADDR);
  if (acssrReady) {
    acssr.on();
    acssr.setLEDColor(0x00ff00);
  }

  sdReady = SD.begin(4, SPI, 25000000);
  if (sdReady && !SD.exists("/sessions")) {
    SD.mkdir("/sessions");
  }

  historyCapacity = HISTORY_SECONDS;
  history = static_cast<HistorySample*>(heap_caps_malloc(sizeof(HistorySample) * historyCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (history == nullptr) {
    historyCapacity = 3600;
    history = static_cast<HistorySample*>(malloc(sizeof(HistorySample) * historyCapacity));
  }

  twai_general_config_t generalConfig = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_NORMAL);
  twai_timing_config_t timingConfig = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t filterConfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  canReady = twai_driver_install(&generalConfig, &timingConfig, &filterConfig) == ESP_OK && twai_start() == ESP_OK;

  setupWeb();
  drawUi(true);
}

void loop() {
  M5.update();
  server.handleClient();
  serviceCanRx();
  servicePoll();
  servicePowerCycle();
  handleTouch();
  drawUi();
  if (sdReady && millis() - lastSdPruneMs > 60000) {
    lastSdPruneMs = millis();
    enforceSdLimit();
  }
}

uint64_t epochMsNow() {
  time_t now = time(nullptr);
  if (now > 1700000000) return static_cast<uint64_t>(now) * 1000ULL;
  return millis();
}

String isoNow() {
  time_t now = time(nullptr);
  if (now <= 1700000000) return String(millis());
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
  return String(buf);
}

String logFilenameNow() {
  time_t now = time(nullptr);
  char buf[32];
  if (now > 1700000000) {
    struct tm tmNow;
    localtime_r(&now, &tmNow);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H%M%S.csv", &tmNow);
  } else {
    snprintf(buf, sizeof(buf), "boot-%lu.csv", static_cast<unsigned long>(millis()));
  }
  return String(buf);
}

String jsonBool(bool value) {
  return value ? "true" : "false";
}

String jsonNumber(float value, uint8_t digits) {
  if (isnan(value)) return "null";
  return String(value, static_cast<unsigned int>(digits));
}

String urlDecode(const String& input) {
  String out;
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (c == '%' && i + 2 < input.length()) {
      char hex[3] = {input[i + 1], input[i + 2], 0};
      out += static_cast<char>(strtol(hex, nullptr, 16));
      i += 2;
    } else if (c == '+') {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

bool sendCanFrame(uint32_t id, const uint8_t* data, uint8_t len) {
  if (!canReady || len > 8) return false;
  twai_message_t msg = {};
  msg.identifier = id;
  msg.extd = 1;
  msg.data_length_code = len;
  memcpy(msg.data, data, len);
  if (twai_transmit(&msg, pdMS_TO_TICKS(20)) == ESP_OK) {
    charger.lastTxMs = millis();
    charger.txFrames++;
    return true;
  }
  return false;
}

void sendRead(uint16_t command) {
  uint8_t data[] = {static_cast<uint8_t>(command & 0xFF), static_cast<uint8_t>(command >> 8)};
  sendCanFrame(CONTROLLER_CAN_ID, data, sizeof(data));
}

void setOperation(bool enabled) {
  uint8_t data[] = {0x00, 0x00, static_cast<uint8_t>(enabled ? 0x01 : 0x00)};
  sendCanFrame(CONTROLLER_CAN_ID, data, sizeof(data));
  if (enabled && currentLogName.isEmpty()) startLogSession();
}

void startPowerCycle() {
  if (!acssrReady || powerCycleActive) return;
  acssr.off();
  acssr.setLEDColor(0xff0000);
  powerCycleActive = true;
  powerCycleStartedMs = millis();
}

void servicePowerCycle() {
  if (!powerCycleActive) return;
  if (millis() - powerCycleStartedMs >= AC_POWER_CYCLE_MS) {
    acssr.on();
    acssr.setLEDColor(0x00ff00);
    powerCycleActive = false;
  }
}

void serviceCanRx() {
  if (!canReady) return;
  twai_message_t msg;
  while (twai_receive(&msg, 0) == ESP_OK) {
    decodeFrame(msg);
  }
}

void servicePoll() {
  uint32_t now = millis();
  if (!canReady || now < nextPollStartMs || now - lastPollTxMs < POLL_GAP_MS) return;
  sendRead(POLL_COMMANDS[pollIndex]);
  lastPollTxMs = now;
  pollIndex++;
  if (pollIndex >= sizeof(POLL_COMMANDS) / sizeof(POLL_COMMANDS[0])) {
    pollIndex = 0;
    nextPollStartMs = now + POLL_INTERVAL_MS;
  }
}

void decodeFrame(const twai_message_t& msg) {
  if (!msg.extd || msg.identifier != CHARGER_CAN_ID || msg.data_length_code < 2) return;
  uint16_t command = msg.data[0] | (msg.data[1] << 8);
  uint16_t raw = msg.data_length_code >= 4 ? (msg.data[2] | (msg.data[3] << 8)) : 0;
  charger.lastRxMs = millis();
  charger.rxFrames++;

  switch (command) {
    case 0x0000:
      if (msg.data_length_code >= 3) charger.flags.operationOn = msg.data[2] != 0;
      break;
    case 0x0020:
      charger.targetVoltage = raw * 0.01f;
      break;
    case 0x0030:
      charger.targetCurrent = raw * 0.01f;
      break;
    case 0x0040:
      if (msg.data_length_code >= 3) {
        uint8_t low = msg.data[2];
        charger.flags.overTemperature = low & 0x02;
        charger.flags.outputOverVoltage = low & 0x04;
        charger.flags.outputOverCurrent = low & 0x08;
        charger.flags.outputShortCircuit = low & 0x10;
        charger.flags.acAbnormal = low & 0x20;
        charger.flags.outputOff = low & 0x40;
        charger.flags.internalTempAbnormal = low & 0x80;
      }
      break;
    case 0x0060:
      charger.outputVoltage = raw * 0.01f;
      charger.outputPower = charger.outputVoltage * (isnan(charger.outputCurrent) ? 0.0f : charger.outputCurrent);
      break;
    case 0x0061:
      charger.outputCurrent = raw * 0.01f;
      charger.outputPower = (isnan(charger.outputVoltage) ? 0.0f : charger.outputVoltage) * charger.outputCurrent;
      addHistorySample();
      break;
    case 0x0062:
      charger.temperature = raw * 0.1f;
      break;
    case 0x00B4:
      if (msg.data_length_code >= 4) {
        charger.flags.chargerMode = msg.data[2] & 0x80;
      }
      break;
    case 0x00B8:
      if (msg.data_length_code >= 4) {
        uint8_t low = msg.data[2];
        uint8_t high = msg.data[3];
        charger.flags.fullyCharged = low & 0x01;
        charger.flags.constantCurrent = low & 0x02;
        charger.flags.constantVoltage = low & 0x04;
        charger.flags.floatMode = low & 0x08;
        charger.flags.wakeUpUnfinished = low & 0x40;
        charger.flags.tempCompShort = high & 0x04;
        charger.flags.noBattery = high & 0x08;
        charger.flags.ccTimeout = high & 0x20;
        charger.flags.cvTimeout = high & 0x40;
        charger.flags.floatTimeout = high & 0x80;
      }
      break;
    case 0x00C1:
      if (msg.data_length_code >= 3) {
        uint8_t low = msg.data[2];
        charger.flags.dcOutputNormal = low & 0x02;
        charger.flags.initialState = low & 0x20;
        charger.flags.eepromError = low & 0x40;
      }
      break;
    case 0x00C2:
      if (msg.data_length_code >= 4) charger.flags.eepromWritesDisabled = msg.data[3] & 0x04;
      break;
    default:
      break;
  }
}

void addHistorySample() {
  if (isnan(charger.outputVoltage) || isnan(charger.outputCurrent)) return;
  if (history == nullptr || historyCapacity == 0) return;
  HistorySample sample = {
      epochMsNow(),
      charger.outputVoltage,
      charger.outputCurrent,
      charger.outputVoltage * charger.outputCurrent,
      charger.temperature,
  };
  size_t index = (historyStart + historyCount) % historyCapacity;
  if (historyCount < historyCapacity) {
    historyCount++;
  } else {
    historyStart = (historyStart + 1) % historyCapacity;
  }
  history[index] = sample;
  appendLogSample(sample);
}

void startLogSession() {
  if (!sdReady) return;
  if (!SD.exists("/sessions")) SD.mkdir("/sessions");
  currentLogName = logFilenameNow();
  File file = SD.open("/sessions/" + currentLogName, FILE_WRITE);
  if (!file) return;
  file.println("timestamp,voltage,current,power,temperature,operation_on,charger_mode,constant_current,constant_voltage,float_mode,fully_charged,no_battery,output_off");
  file.close();
  enforceSdLimit();
}

void appendLogSample(const HistorySample& sample) {
  if (!sdReady) return;
  if (currentLogName.isEmpty()) startLogSession();
  File file = SD.open("/sessions/" + currentLogName, FILE_APPEND);
  if (!file) return;
  file.printf("%s,%.3f,%.3f,%.3f,", isoNow().c_str(), sample.voltage, sample.current, sample.power);
  if (isnan(sample.temperature)) file.print(",");
  else file.printf("%.2f,", sample.temperature);
  file.printf("%u,%u,%u,%u,%u,%u,%u,%u\n",
              charger.flags.operationOn, charger.flags.chargerMode, charger.flags.constantCurrent,
              charger.flags.constantVoltage, charger.flags.floatMode, charger.flags.fullyCharged,
              charger.flags.noBattery, charger.flags.outputOff);
  file.close();
}

void enforceSdLimit() {
  if (!sdReady) return;
  uint64_t total = SD.totalBytes();
  if (total == 0) return;
  uint64_t limit = static_cast<uint64_t>(total * SD_MAX_USED_FRACTION);
  while (SD.usedBytes() > limit) {
    if (!deleteOldestLogFile()) break;
  }
}

bool deleteOldestLogFile() {
  File root = SD.open("/sessions");
  if (!root) return false;
  String oldest;
  File file = root.openNextFile();
  while (file) {
    String name = String(file.name());
    if (!file.isDirectory() && name.endsWith(".csv")) {
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (oldest.isEmpty() || name < oldest) oldest = name;
    }
    file = root.openNextFile();
  }
  root.close();
  if (oldest.isEmpty()) return false;
  if (oldest == currentLogName) return false;
  return SD.remove("/sessions/" + oldest);
}

void setupWeb() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/logs", HTTP_GET, handleLogs);
  server.on("/history.csv", HTTP_GET, handleHistoryCsv);
  server.on("/api/operation", HTTP_POST, []() {
    String body = server.arg("plain");
    bool enabled = body.indexOf("true") >= 0 || body.indexOf("on") >= 0 || body.indexOf("1") >= 0;
    setOperation(enabled);
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/power-cycle", HTTP_POST, []() {
    startPowerCycle();
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.onNotFound([]() {
    String uri = server.uri();
    if (uri.startsWith("/logs/")) {
      handleLogDownload(urlDecode(uri.substring(6)));
      return;
    }
    server.send(404, "text/plain", "not found");
  });
  server.begin();
}

void handleState() {
  uint32_t rxAge = charger.lastRxMs == 0 ? UINT32_MAX : millis() - charger.lastRxMs;
  String json = "{";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"ssid\":\"" + String(WIFI_SSID) + "\",";
  json += "\"mdns\":\"" + String(MDNS_NAME) + "\",";
  json += "\"canReady\":" + jsonBool(canReady) + ",";
  json += "\"sdReady\":" + jsonBool(sdReady) + ",";
  json += "\"acssrReady\":" + jsonBool(acssrReady) + ",";
  json += "\"acssrOn\":" + jsonBool(acssrReady ? acssr.status() : false) + ",";
  json += "\"powerCycleActive\":" + jsonBool(powerCycleActive) + ",";
  json += "\"lastRxAgeMs\":" + String(rxAge) + ",";
  json += "\"rxFrames\":" + String(charger.rxFrames) + ",";
  json += "\"txFrames\":" + String(charger.txFrames) + ",";
  json += "\"outputVoltage\":" + jsonNumber(charger.outputVoltage, 3) + ",";
  json += "\"outputCurrent\":" + jsonNumber(charger.outputCurrent, 3) + ",";
  json += "\"outputPower\":" + jsonNumber(charger.outputPower, 3) + ",";
  json += "\"temperature\":" + jsonNumber(charger.temperature, 2) + ",";
  json += "\"targetVoltage\":" + jsonNumber(charger.targetVoltage, 2) + ",";
  json += "\"targetCurrent\":" + jsonNumber(charger.targetCurrent, 2) + ",";
  json += "\"currentLog\":\"" + currentLogName + "\",";
  json += "\"sdUsedMB\":" + String(sdReady ? SD.usedBytes() / 1048576 : 0) + ",";
  json += "\"sdTotalMB\":" + String(sdReady ? SD.totalBytes() / 1048576 : 0) + ",";
  json += "\"flags\":{";
  json += "\"operationOn\":" + jsonBool(charger.flags.operationOn) + ",";
  json += "\"overTemperature\":" + jsonBool(charger.flags.overTemperature) + ",";
  json += "\"outputOverVoltage\":" + jsonBool(charger.flags.outputOverVoltage) + ",";
  json += "\"outputOverCurrent\":" + jsonBool(charger.flags.outputOverCurrent) + ",";
  json += "\"outputShortCircuit\":" + jsonBool(charger.flags.outputShortCircuit) + ",";
  json += "\"acAbnormal\":" + jsonBool(charger.flags.acAbnormal) + ",";
  json += "\"outputOff\":" + jsonBool(charger.flags.outputOff) + ",";
  json += "\"internalTempAbnormal\":" + jsonBool(charger.flags.internalTempAbnormal) + ",";
  json += "\"fullyCharged\":" + jsonBool(charger.flags.fullyCharged) + ",";
  json += "\"constantCurrent\":" + jsonBool(charger.flags.constantCurrent) + ",";
  json += "\"constantVoltage\":" + jsonBool(charger.flags.constantVoltage) + ",";
  json += "\"floatMode\":" + jsonBool(charger.flags.floatMode) + ",";
  json += "\"wakeUpUnfinished\":" + jsonBool(charger.flags.wakeUpUnfinished) + ",";
  json += "\"tempCompShort\":" + jsonBool(charger.flags.tempCompShort) + ",";
  json += "\"noBattery\":" + jsonBool(charger.flags.noBattery) + ",";
  json += "\"ccTimeout\":" + jsonBool(charger.flags.ccTimeout) + ",";
  json += "\"cvTimeout\":" + jsonBool(charger.flags.cvTimeout) + ",";
  json += "\"floatTimeout\":" + jsonBool(charger.flags.floatTimeout) + ",";
  json += "\"dcOutputNormal\":" + jsonBool(charger.flags.dcOutputNormal) + ",";
  json += "\"initialState\":" + jsonBool(charger.flags.initialState) + ",";
  json += "\"eepromError\":" + jsonBool(charger.flags.eepromError) + ",";
  json += "\"chargerMode\":" + jsonBool(charger.flags.chargerMode) + ",";
  json += "\"eepromWritesDisabled\":" + jsonBool(charger.flags.eepromWritesDisabled);
  json += "}}";
  server.send(200, "application/json", json);
}

void handleLogs() {
  if (!sdReady) {
    server.send(200, "application/json", "[]");
    return;
  }
  File root = SD.open("/sessions");
  String json = "[";
  bool first = true;
  if (root) {
    File file = root.openNextFile();
    while (file) {
      String name = String(file.name());
      if (!file.isDirectory() && name.endsWith(".csv")) {
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + name + "\",\"size\":" + String(file.size()) + "}";
      }
      file = root.openNextFile();
    }
    root.close();
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleLogDownload(const String& filename) {
  if (!sdReady || filename.indexOf("..") >= 0 || filename.indexOf('/') >= 0) {
    server.send(404, "text/plain", "not found");
    return;
  }
  File file = SD.open("/sessions/" + filename, FILE_READ);
  if (!file) {
    server.send(404, "text/plain", "not found");
    return;
  }
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.streamFile(file, "text/csv");
  file.close();
}

void handleHistoryCsv() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  server.sendContent("timestamp,voltage,current,power,temperature\n");
  for (size_t i = 0; i < historyCount; ++i) {
    size_t idx = (historyStart + i) % historyCapacity;
    const HistorySample& s = history[idx];
    String line = String(static_cast<unsigned long>(s.epochMs / 1000ULL)) + "." + String(static_cast<unsigned long>(s.epochMs % 1000ULL)) + ",";
    line += String(s.voltage, 3) + "," + String(s.current, 3) + "," + String(s.power, 3) + ",";
    if (!isnan(s.temperature)) line += String(s.temperature, 2);
    line += "\n";
    server.sendContent(line);
  }
  server.sendContent("");
}

void drawButton(int x, int y, int w, int h, const char* label, uint16_t color) {
  M5.Display.fillRoundRect(x, y, w, h, 6, color);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(label, x + w / 2, y + h / 2);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(TFT_WHITE);
}

void drawNav() {
  const int y = 216;
  drawButton(4, y, 74, 22, "Main", currentScreen == Screen::Dashboard ? TFT_CYAN : TFT_DARKGREY);
  drawButton(83, y, 74, 22, "Flags", currentScreen == Screen::Flags ? TFT_CYAN : TFT_DARKGREY);
  drawButton(162, y, 74, 22, "Maint", currentScreen == Screen::Maintenance ? TFT_CYAN : TFT_DARKGREY);
  drawButton(241, y, 74, 22, "Logs", currentScreen == Screen::Logs ? TFT_CYAN : TFT_DARKGREY);
}

void drawUi(bool force) {
  if (!force && millis() - lastDrawMs < 500) return;
  lastDrawMs = millis();
  M5.Display.fillScreen(0x1082);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextSize(1);
  M5.Display.drawString("MEAN WELL NPB-450-24NFC", 8, 6);
  M5.Display.setTextSize(2);
  M5.Display.drawString(charger.flags.chargerMode ? "Charger mode" : "PSU mode", 8, 24);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(canReady && charger.lastRxMs && millis() - charger.lastRxMs < 5000 ? TFT_GREEN : TFT_ORANGE);
  M5.Display.drawString(canReady ? "CAN" : "NO CAN", 252, 8);

  if (currentScreen == Screen::Dashboard) {
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setTextSize(3);
    M5.Display.drawString(jsonNumber(charger.outputVoltage, 1) + " V", 12, 60);
    M5.Display.drawString(jsonNumber(charger.outputCurrent, 2) + " A", 12, 100);
    M5.Display.setTextSize(2);
    M5.Display.drawString(jsonNumber(charger.outputPower, 0) + " W", 190, 66);
    M5.Display.drawString(jsonNumber(charger.temperature, 1) + " C", 190, 98);
    M5.Display.setTextSize(1);
    M5.Display.drawString(charger.flags.operationOn ? "Operation ON" : "Operation OFF", 190, 130);
    drawButton(10, 166, 92, 38, "Start", TFT_GREEN);
    drawButton(114, 166, 92, 38, "Stop", TFT_RED);
    drawButton(218, 166, 92, 38, "Cycle", TFT_ORANGE);
  } else if (currentScreen == Screen::Flags) {
    M5.Display.setTextSize(1);
    int y = 54;
    auto row = [&](const char* label, bool value) {
      M5.Display.setTextColor(value ? TFT_BLACK : TFT_LIGHTGREY);
      M5.Display.fillRoundRect(8, y, 304, 15, 3, value ? TFT_ORANGE : TFT_DARKGREY);
      M5.Display.drawString(label, 12, y + 3);
      M5.Display.drawString(value ? "YES" : "no", 282, y + 3);
      y += 18;
    };
    row("Output off", charger.flags.outputOff);
    row("No battery", charger.flags.noBattery);
    row("Constant current", charger.flags.constantCurrent);
    row("Constant voltage", charger.flags.constantVoltage);
    row("Float", charger.flags.floatMode);
    row("Fully charged", charger.flags.fullyCharged);
    row("AC abnormal", charger.flags.acAbnormal);
    row("Over temp", charger.flags.overTemperature || charger.flags.internalTempAbnormal);
  } else if (currentScreen == Screen::Maintenance) {
    M5.Display.setTextColor(TFT_LIGHTGREY);
    M5.Display.setTextSize(1);
    M5.Display.drawString("IP: " + WiFi.localIP().toString(), 10, 56);
    M5.Display.drawString("mDNS: " + String(MDNS_NAME) + ".local", 10, 74);
    M5.Display.drawString("SSID: " + String(WIFI_SSID), 10, 92);
    M5.Display.drawString("ACSSR: " + String(acssrReady ? (acssr.status() ? "ON" : "OFF") : "missing"), 10, 110);
    M5.Display.drawString("SD: " + String(sdReady ? "mounted" : "missing"), 10, 128);
    if (sdReady) {
      M5.Display.drawString("Used: " + String(SD.usedBytes() / 1048576) + " / " + String(SD.totalBytes() / 1048576) + " MB", 10, 146);
    }
    M5.Display.drawString("Log: " + (currentLogName.isEmpty() ? String("--") : currentLogName), 10, 164);
  } else if (currentScreen == Screen::Logs) {
    M5.Display.setTextColor(TFT_LIGHTGREY);
    M5.Display.setTextSize(1);
    M5.Display.drawString("Download at http://" + WiFi.localIP().toString(), 10, 56);
    M5.Display.drawString("or http://" + String(MDNS_NAME) + ".local", 10, 74);
    M5.Display.drawString("Current session:", 10, 102);
    M5.Display.drawString(currentLogName.isEmpty() ? "--" : currentLogName, 10, 120);
    M5.Display.drawString("History samples: " + String(historyCount), 10, 146);
  }
  drawNav();
}

void handleTouch() {
  auto touch = M5.Touch.getDetail();
  if (!touch.wasClicked()) return;
  int x = touch.x;
  int y = touch.y;
  if (y >= 216) {
    if (x < 80) currentScreen = Screen::Dashboard;
    else if (x < 160) currentScreen = Screen::Flags;
    else if (x < 240) currentScreen = Screen::Maintenance;
    else currentScreen = Screen::Logs;
    drawUi(true);
    return;
  }
  if (currentScreen == Screen::Dashboard && y >= 166 && y <= 204) {
    if (x >= 10 && x <= 102) setOperation(true);
    else if (x >= 114 && x <= 206) setOperation(false);
    else if (x >= 218 && x <= 310) startPowerCycle();
    drawUi(true);
  }
}
