#include "display_ui.h"
#include "touch_controls.h"
#include <M5Unified.h>
#include <driver/twai.h>
#include <freertos/semphr.h>

namespace displayui {
namespace {
constexpr uint16_t BG = 0x1082, PANEL = 0x2124, TEXT = 0xf79d, MUTED = 0xad55;
constexpr uint16_t GREEN = 0x35cd, AMBER = 0xfdca, CYAN = 0x763b, RED = 0xd924;
M5Canvas canvas(&M5.Display);
SemaphoreHandle_t canvasLock = nullptr;
portMUX_TYPE statusLock = portMUX_INITIALIZER_UNLOCKED;
Status sharedStatus;
uint8_t page = 0;
uint32_t lastDraw = 0, touchCount = 0, localJob = 0;
bool canvasReady = false, confirming = false, stopRequested = false;
String cached, localError;
control::ChargeReview review;
touchcontrol::HoldToStart hold;

void text(int x, int y, const String& value, int size = 1, uint16_t color = TEXT) {
  canvas.setTextSize(size); canvas.setTextColor(color); canvas.setCursor(x, y); canvas.print(value);
}
void center(int x, int y, const String& value, int size = 1, uint16_t color = TEXT) {
  canvas.setTextSize(size); text(x - canvas.textWidth(value) / 2, y, value, size, color);
}
void wrapped(const String& value, int x, int y, int width, uint16_t color = TEXT) {
  String line;
  for (size_t start = 0; start < value.length();) {
    int end = value.indexOf(' ', start); if (end < 0) end = value.length();
    const String word = value.substring(start, end);
    if (line.length() && int(line.length() + word.length() + 1) * 6 > width) {
      text(x, y, line, 1, color); y += 12; line = "";
    }
    if (line.length()) line += ' ';
    line += word; start = end + 1;
  }
  text(x, y, line, 1, color);
}
String number(const npb::State& s, uint16_t cmd, uint32_t now, int decimals = 2) {
  const auto& reg = s.get(cmd);
  return reg.fresh(now) ? String(npb::decodedValue(cmd, reg.raw), decimals) : "--";
}
bool inside(int x, int y, int left, int top, int width, int height) {
  return x >= left && x < left + width && y >= top && y < top + height;
}
const char* readyError(const canbus::Snapshot& can, const control::ChargeReview& values, uint32_t now) {
  if (canbus::acCyclePending()) return "Ask an adult to check the charger setup on the website.";
  if (!can.bus.ready || can.bus.state != TWAI_STATE_RUNNING || can.bus.txErrors || can.bus.rxErrors || now - can.bus.lastErrorAt < 2000)
    return "Cannot reach the charger. Ask an adult to check its cable and power.";
  return control::chargeError(can.charger, values, now);
}
void row(unsigned index, const String& top, const String& bottom, uint16_t color = TEXT, uint8_t size = 2) {
  const int y = 30 + index * 34;
  canvas.fillRect(0, y, 320, 34, PANEL);
  text(10, y + 2, top, 1, MUTED); text(10, y + 14, bottom, size, color);
}
void chrome(const network::Snapshot& net) {
  canvas.fillSprite(BG);
  text(9, 6, page == 0 ? "BATTERY CHARGER" : "NPB-450", 2, CYAN);
  const char* labels[] = {"HOME", "DIAGNOSTICS", "NETWORK"};
  for (int i = 0; i < 3; ++i) {
    const int x = i * 106;
    canvas.fillRoundRect(x + 2, 218, 104, 22, 4, page == i ? CYAN : PANEL);
    center(x + 54, 226, labels[i], 1, page == i ? BG : MUTED);
  }
  text(10, 204, net.connected ? String("Wi-Fi  ") + net.ip : net.apActive ? String("AP  ") + net.apIp : "Wi-Fi connecting...", 1, MUTED);
}
void mainPage(const canbus::Snapshot& can, uint32_t now, bool ready, const canbus::ControlStatus& job) {
  const auto& s = can.charger;
  text(10, 32, "BATTERY", 1, MUTED);
  text(10, 46, number(s, 0x0060, now) + " V", 3);
  text(10, 83, number(s, 0x0061, now) + " A", 3);
  const bool fresh = s.get(0).fresh(now) && s.get(0x0061).fresh(now) && npb::statusFresh(s, now);
  const bool charging = fresh && s.get(0).raw && !(s.get(0x0040).raw & 0x40) && s.get(0x0061).raw >= 20;
  const bool stopped = fresh && !s.get(0).raw && s.get(0x0061).raw < 20;
  const bool pending = job.id == localJob && localJob && strcmp(job.state, "verified") && strcmp(job.state, "failed");
  String state = charging ? "Charging" : stopped ? "Stopped" : fresh && s.get(0).raw ? "Charger on" : "Checking...";
  if (pending) state = stopRequested ? "Stopping..." : "Starting...";
  if (job.id == localJob && localJob && !strcmp(job.state, "failed")) state = "Ask an adult";
  if (localError.length()) state = "Ask an adult";
  text(10, 125, state, 2, charging ? GREEN : pending ? AMBER : CYAN);
  const auto& v = s.get(0x0060); const auto& a = s.get(0x0061);
  const String power = v.fresh(now) && a.fresh(now) ? String(v.raw * .01f * a.raw * .01f, 0) : "--";
  text(10, 151, power + " W  /  charger " + number(s, 0x0062, now, 0) + " C", 1, MUTED);
  String note = ready ? "TimeUSB 24V / 25Ah" : "Charge needs a check";
  if (!npb::statusFresh(s, now)) note = "No fresh charger data";
  if (localError.length() || (job.id == localJob && !strcmp(job.state, "failed"))) note = stopRequested ? "STOP not confirmed.\nAsk an adult now." : "Start not confirmed.\nCheck the website.";
  else if (charging) note = npb::phase(s, now);
  text(10, 171, note, 1, localError.length() ? AMBER : MUTED);
  canvas.fillRoundRect(193, 32, 119, 76, 10, ready ? GREEN : PANEL);
  canvas.fillTriangle(246, 41, 246, 59, 262, 50, ready ? TEXT : MUTED);
  center(252, 65, "CHARGE", 2, ready ? TEXT : MUTED); center(252, 87, "NOW", 2, ready ? TEXT : MUTED);
  canvas.fillRoundRect(193, 116, 119, 76, 10, RED);
  canvas.fillRoundRect(245, 124, 15, 15, 2, TEXT);
  center(252, 145, "STOP", 2); center(252, 173, "CHARGING", 1);
}
void reviewPage(const char* error, uint32_t now) {
  canvas.fillSprite(BG);
  const uint32_t elapsed = hold.progress(now);
  if (elapsed) {
    canvas.fillArc(160, 95, 82, 68, 0, 360, PANEL);
    const float sweep = min(360.0f, elapsed * 360.0f / touchcontrol::HoldToStart::HOLD_MS);
    canvas.fillArc(160, 95, 82, 68, 270, 270 + sweep, GREEN);
    center(160, 77, "Keep", 2); center(160, 101, "holding...", 2);
    center(160, 184, "Let go to cancel", 2, MUTED);
    return;
  }
  center(160, 10, error ? "Ask a grown-up" : "Ready to charge?", 2, error ? AMBER : CYAN);
  if (error) wrapped(error, 16, 49, 288, AMBER);
  else {
    center(160, 42, "TimeUSB 24V / 25Ah", 2);
    center(160, 68, String(review.values[0] * .01f, 1) + " A  /  " + String(review.values[1] * .01f, 1) + " V", 2, GREEN);
    center(160, 100, "Battery connected and warm (0-50 C)?", 1);
    center(160, 116, "Cold battery? Stop and ask a grown-up.", 1, AMBER);
    center(160, 136, "Holding confirms these checks.", 1, MUTED);
    canvas.fillRoundRect(16, 156, 288, 49, 9, GREEN);
    center(160, 164, "HOLD TO START", 2); center(160, 190, "2 seconds", 1);
  }
  canvas.fillRoundRect(16, 213, 288, 26, 6, PANEL);
  center(160, 220, "CANCEL", 2);
}
}
void begin() {
  M5.Display.setRotation(0); M5.Display.setBrightness(150);
  canvasLock = xSemaphoreCreateMutex(); canvas.setColorDepth(16); canvas.setPsram(true);
  canvasReady = canvasLock && canvas.createSprite(320, 240) != nullptr;
  if (!canvasReady) {
    M5.Display.fillScreen(BG); M5.Display.setTextSize(2); M5.Display.setTextColor(AMBER);
    M5.Display.setCursor(10, 70); M5.Display.println("Display unavailable"); M5.Display.println("Use the website");
  }
}
Status status() { portENTER_CRITICAL(&statusLock); auto result = sharedStatus; portEXIT_CRITICAL(&statusLock); return result; }
uint32_t touches() { return status().touches; }
bool capture(uint8_t* rgb, size_t size) {
  if (!canvasReady || size < 320 * 240 * 3 || xSemaphoreTake(canvasLock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  if (page == 2) memset(rgb, 0, 320 * 240 * 3);
  else canvas.readRectRGB(0, 0, 320, 240, rgb);
  xSemaphoreGive(canvasLock); return true;
}
void service(const canbus::Snapshot& can, const network::Snapshot& net) {
  if (!canvasReady) return;
  const uint32_t now = millis();
  unsigned fingers = 0; int x = -1, y = -1; bool pressed = false;
  for (unsigned i = 0; i < M5.Touch.getCount(); ++i) {
    const auto& touch = M5.Touch.getDetail(i);
    if (touch.isPressed()) { ++fingers; x = touch.x; y = touch.y; pressed |= touch.wasPressed(); }
  }
  if (pressed) ++touchCount;
  if (xSemaphoreTake(canvasLock, pdMS_TO_TICKS(5)) != pdTRUE) return;
  control::ChargeReview liveReview;
  for (size_t i = 0; i < 7; ++i) liveReview.values[i] = can.charger.get(control::REVIEW_COMMANDS[i]).raw;
  const char* error = readyError(can, confirming ? review : liveReview, now);
  if (confirming) {
    if (hold.expired(now) || (fingers == 1 && pressed && y >= 213)) { confirming = false; cached = ""; }
    else if (error) hold.update(now, 2, false, false);
    else if (hold.update(now, fingers, inside(x, y, 16, 156, 288, 49), pressed)) {
      const char* failure = canbus::submitLocalCharge(review, localJob);
      localError = failure ? failure : ""; stopRequested = false; confirming = false; cached = "";
    }
  } else if (fingers == 1 && pressed) {
    if (y >= 218) { page = min(2, max(0, x / 106)); cached = ""; }
    else if (page == 0 && inside(x, y, 193, 116, 119, 76)) {
      const char* failure = canbus::submitLocalStop(localJob);
      localError = failure ? failure : ""; stopRequested = true; cached = "";
    } else if (page == 0 && inside(x, y, 193, 32, 119, 76)) {
      review = liveReview; hold.open(now); confirming = true; cached = "";
    }
  }
  const auto job = canbus::controlStatus();
  Status screen; screen.page = page; screen.touches = touchCount; screen.confirming = confirming;
  screen.holding = confirming && hold.progress(now) > 0; screen.localJob = localJob;
  portENTER_CRITICAL(&statusLock); sharedStatus = screen; portEXIT_CRITICAL(&statusLock);
  const uint32_t interval = screen.holding ? 33 : 250;
  if (cached.length() && now - lastDraw < interval) { xSemaphoreGive(canvasLock); return; }
  lastDraw = now;
  const auto& s = can.charger;
  if (confirming) reviewPage(error, now);
  else {
    chrome(net);
    if (page == 0) mainPage(can, now, error == nullptr, job);
    else if (page == 1) {
      row(0, "250 KBPS / EXTENDED / ADDRESS 0", canbus::stateName(can.bus), s.online(now) ? GREEN : AMBER);
      row(1, "REPLIES / REQUESTS QUEUED", String(s.received) + " / " + can.bus.txQueued);
      row(2, "TX ERROR COUNTER / RX ERROR COUNTER", String(can.bus.txErrors) + " / " + can.bus.rxErrors);
      row(3, "TX FAILED / BUS ERRORS", String(can.bus.failed) + " / " + can.bus.busErrors);
      row(4, "MAX CAN SERVICE GAP / TOUCHES", String(can.bus.maxServiceGap) + " ms / " + touchCount);
    } else {
      row(0, "HOSTNAME / MDNS", "npb-monitor.local", CYAN);
      row(1, net.connected ? String("WI-FI: ") + net.ssid : "WI-FI: CONNECTING / AP FALLBACK",
        net.connected ? net.ip : net.apActive ? net.apIp : "Waiting for Wi-Fi", net.connected ? GREEN : AMBER);
      row(2, net.apActive ? "AP: NPB-Monitor / PASSWORD" : "WI-FI SIGNAL / SAVED CREDENTIALS",
        net.apActive ? String(net.apPassword) : String(net.rssi) + " dBm / " + (net.credentialsSaved ? "Yes" : "No"));
      row(3, "CONFIGURATION / UPDATE KEY (FIRST HALF)", String(network::token()).substring(0, 24), MUTED, 1);
      row(4, "CONFIGURATION / UPDATE KEY (SECOND HALF)", String(network::token()).substring(24), MUTED, 1);
    }
  }
  canvas.pushSprite(0, 0); cached = "drawn";
  xSemaphoreGive(canvasLock);
}
}
