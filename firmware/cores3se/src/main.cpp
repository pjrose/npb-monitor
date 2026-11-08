#include <Arduino.h>
#include <M5Unified.h>
#include "can_monitor.h"
#include "display_ui.h"
#include "network_service.h"
#include "web_service.h"

void setup() {
  Serial.begin(115200);
  auto config = M5.config();
  config.internal_imu = false;
  config.internal_mic = false;
  config.internal_spk = false;
  config.external_display_value = 0;
  M5.begin(config);
  displayui::begin();
  const bool canReady = canbus::begin();
  const bool webReady = webservice::begin();
  Serial.printf("BOOT NPB %s reset=%d board=%d display=%dx%d touch=%d psram=%u CAN=%d HTTP=%d\n",
    webservice::VERSION, (int)esp_reset_reason(), (int)M5.getBoard(), M5.Display.width(), M5.Display.height(),
    M5.Touch.isEnabled(), ESP.getPsramSize(), canReady, webReady);
}
void loop() {
  M5.update();
  const auto can = canbus::snapshot();
  const auto net = network::snapshot();
  displayui::service(can, net);
  static uint32_t lastLog = 0;
  if (millis() - lastLog >= 5000) {
    lastLog = millis();
    Serial.printf("STATUS ms=%lu rx=%lu queued=%lu failed=%lu TEC=%lu REC=%lu state=%s wifi=%d ip=%s heap=%u gap=%lu touch=%lu\n",
      (unsigned long)millis(), (unsigned long)can.charger.received, (unsigned long)can.bus.txQueued,
      (unsigned long)can.bus.failed, (unsigned long)can.bus.txErrors, (unsigned long)can.bus.rxErrors,
      canbus::stateName(can.bus), net.connected, net.ip, ESP.getFreeHeap(),
      (unsigned long)can.bus.maxServiceGap, (unsigned long)displayui::touches());
  }
  delay(2);
}
