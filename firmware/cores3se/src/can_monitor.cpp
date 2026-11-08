#include "can_monitor.h"
#include "charger_control.h"
#include <Arduino.h>
#include <driver/twai.h>

namespace canbus {
namespace {
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Snapshot shared;
Traffic frames[TRAFFIC_CAP];
size_t head = 0, count = 0;
struct Request { npb::Frame frames[20]; uint8_t count = 0; uint32_t id = 0; bool charge = false, localOperation = false; control::ChargeReview review; };
Request pendingRequest;
ControlStatus jobStatus;
bool requestPending = false, reserved = false, enabled = false, pollPaused = false;
bool stopPending = false, cyclePending = true; // Closed until saved configuration is loaded.
uint32_t stopId = 0;
uint32_t nextId = 1;
void record(const npb::Frame& frame, uint32_t now, bool tx, bool accepted) {
  portENTER_CRITICAL(&mux);
  auto& item = frames[head];
  item.ms = now; item.frame = frame; item.tx = tx; item.accepted = accepted;
  head = (head + 1) % TRAFFIC_CAP;
  if (count < TRAFFIC_CAP) ++count;
  portEXIT_CRITICAL(&mux);
}
bool sendFrame(const npb::Frame& frame, Snapshot& local, uint32_t now) {
  twai_message_t tx = {};
  tx.identifier = frame.id; tx.extd = 1; tx.ss = 1; tx.data_length_code = frame.length;
  memcpy(tx.data, frame.data, frame.length);
  const esp_err_t result = twai_transmit(&tx, 0);
  if (result == ESP_OK) ++local.bus.txQueued;
  else { ++local.bus.txRejected; local.bus.error = result; }
  record(frame, now, true, result == ESP_OK);
  return result == ESP_OK;
}
void run(void*) {
  Snapshot local = snapshot();
  size_t pollIndex = 0;
  uint32_t lastPoll = 0, lastStatus = 0, recoveryAt = 0;
  bool recovering = false;
  enum Phase { Ready, WriteTx, NeedRead, ReadTx, Reply, ReadOnlyTx };
  Phase phase = Ready;
  Request request;
  ControlStatus job;
  bool active = false, anyWrite = false;
  uint8_t item = 0;
  uint32_t itemStarted = 0;
  auto finish = [&](const char* state, const char* error) {
    strlcpy(job.state, state, sizeof(job.state)); strlcpy(job.error, error, sizeof(job.error)); job.updated = millis();
    portENTER_CRITICAL(&mux);
    if (!stopPending) jobStatus = job;
    reserved = stopPending;
    portEXIT_CRITICAL(&mux);
    active = false;
  };
  auto advance = [&]() {
    ++item; job.step = item; job.updated = millis();
    if (item == request.count) finish(anyWrite ? "verified" : "reads_sent", "");
    else { phase = Ready; itemStarted = millis(); }
  };
  for (;;) {
    const uint32_t now = millis();
    portENTER_CRITICAL(&mux);
    if (stopPending) {
      // Replace an active sequence, including any ON that has not been sent.
      // A frame already on the wire may complete; OFF follows at normal bus spacing.
      request = Request{}; request.count = 1; request.id = stopId; request.localOperation = true;
      control::encode(0, 0, request.frames[0]);
      stopPending = false; requestPending = false; active = true;
      job = jobStatus; item = 0; itemStarted = now; phase = Ready; anyWrite = false;
    } else if (requestPending && !active) {
      request = pendingRequest; requestPending = false; active = true;
      job = jobStatus; item = 0; itemStarted = now; phase = Ready; anyWrite = false;
    }
    const bool allowWrites = enabled, stopPolling = pollPaused;
    portEXIT_CRITICAL(&mux);
    if (local.bus.lastService) local.bus.maxServiceGap = max(local.bus.maxServiceGap, uint32_t(now - local.bus.lastService));
    local.bus.lastService = now;
    twai_message_t received;
    for (unsigned i = 0; i < 32 && twai_receive(&received, 0) == ESP_OK; ++i) {
      npb::Frame frame;
      frame.id = received.identifier; frame.extended = received.extd; frame.rtr = received.rtr;
      frame.length = received.data_length_code;
      memcpy(frame.data, received.data, min(size_t(frame.length), sizeof(frame.data)));
      const bool accepted = local.charger.accept(frame, now);
      if (!accepted) {
        if (frame.id == npb::chargerId(npb::ADDRESS)) ++local.bus.invalidRx;
        else ++local.bus.otherRx;
      }
      record(frame, now, false, accepted);
      if (active && accepted && (phase == ReadTx || phase == Reply) && control::command(frame) == job.command) {
        job.observed = local.charger.get(job.command).raw;
        if (job.observed == job.requested) advance();
      }
    }
    if (now - lastStatus >= 100) {
      lastStatus = now;
      twai_status_info_t status = {};
      local.bus.error = twai_get_status_info(&status);
      local.bus.state = status.state;
      local.bus.txErrors = status.tx_error_counter;
      local.bus.rxErrors = status.rx_error_counter;
      if (status.bus_error_count != local.bus.busErrors || status.tx_failed_count != local.bus.failed || status.rx_error_counter || status.tx_error_counter) local.bus.lastErrorAt = now;
      local.bus.failed = status.tx_failed_count;
      local.bus.busErrors = status.bus_error_count;
      local.bus.missed = status.rx_missed_count + status.rx_overrun_count;
      if (status.state == TWAI_STATE_BUS_OFF && !recovering && now - recoveryAt >= 1000) {
        recoveryAt = now;
        local.bus.error = twai_initiate_recovery();
        recovering = local.bus.error == ESP_OK;
      } else if (status.state == TWAI_STATE_STOPPED && recovering) {
        local.bus.error = twai_start();
        if (local.bus.error == ESP_OK) { recovering = false; ++local.bus.recoveries; }
      }
    }
    if (active) {
      uint32_t alerts = 0;
      twai_read_alerts(&alerts, 0);
      if (now - itemStarted >= 5000) finish("failed", "Readback timeout; settings may have changed. Inspect live registers.");
      else if (phase != Ready && phase != NeedRead && phase != Reply && (alerts & TWAI_ALERT_TX_FAILED)) finish("failed", "CAN transmission failed; no automatic write retry");
      else if (phase == WriteTx && (alerts & TWAI_ALERT_TX_SUCCESS)) phase = NeedRead;
      else if (phase == ReadOnlyTx && (alerts & TWAI_ALERT_TX_SUCCESS)) advance();
      else if (phase == ReadTx && (alerts & TWAI_ALERT_TX_SUCCESS)) phase = Reply;
      if (active && phase == Ready && local.bus.state == TWAI_STATE_RUNNING && now - lastPoll >= 180) {
        const auto& frame = request.frames[item];
        const bool writing = frame.length > 2;
        const char* error = writing ? control::stateError(local.charger, frame, now) : nullptr;
        if (!error && request.charge) error = control::chargeError(local.charger, request.review, now);
        if (request.charge && acCyclePending()) error = "Manual AC power cycle acknowledgment required";
        if (writing && !allowWrites && !(request.localOperation && control::command(frame) == 0)) error = "Controls locked";
        if (writing && (control::command(frame) != 0 || control::raw(frame) != 0) && (local.bus.txErrors || local.bus.rxErrors)) error = "CAN errors block setting changes";
        if (writing && (control::command(frame) != 0 || control::raw(frame) != 0) && now - local.bus.lastErrorAt < 2000) error = "Wait for two seconds of healthy CAN";
        if (error) finish("failed", error);
        else {
          job.command = control::command(frame); job.requested = writing ? control::raw(frame) : 0;
          job.observed = local.charger.get(job.command).raw; anyWrite |= writing;
          strlcpy(job.state, writing ? "awaiting_readback" : "sending_reads", sizeof(job.state));
          if (!sendFrame(frame, local, now)) finish("failed", "CAN transmit queue unavailable");
          else { phase = writing ? WriteTx : ReadOnlyTx; lastPoll = now; }
        }
      } else if (active && (phase == NeedRead || phase == Reply) && now - lastPoll >= (phase == NeedRead ? 180u : 400u)) {
        npb::Frame read; npb::makeRead(job.command, 0, read);
        if (!sendFrame(read, local, now)) finish("failed", "Readback request could not be queued");
        else { phase = ReadTx; lastPoll = now; }
      }
      if (active) { portENTER_CRITICAL(&mux); if (!stopPending) jobStatus = job; portEXIT_CRITICAL(&mux); }
    }
    const uint32_t interval = local.bus.txErrors >= 128 ? 500 : 180;
    if (!active && !stopPolling && local.bus.state == TWAI_STATE_RUNNING && now - lastPoll >= interval) {
      lastPoll = now;
      npb::Frame frame;
      npb::makeRead(npb::COMMANDS[pollIndex++ % npb::REGISTER_COUNT], npb::ADDRESS, frame);
      if (npb::permittedRead(frame, npb::ADDRESS)) {
        sendFrame(frame, local, now);
      } else ++local.bus.txRejected;
    }
    portENTER_CRITICAL(&mux);
    shared = local;
    portEXIT_CRITICAL(&mux);
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
}
bool begin() {
  twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(GPIO_NUM_17, GPIO_NUM_18, TWAI_MODE_NORMAL);
  general.tx_queue_len = 1; general.rx_queue_len = 48;
  general.alerts_enabled = TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_TX_FAILED;
  twai_timing_config_t timing = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  shared.bus.error = twai_driver_install(&general, &timing, &filter);
  if (shared.bus.error != ESP_OK) return false;
  shared.bus.error = twai_start();
  if (shared.bus.error != ESP_OK) { twai_driver_uninstall(); return false; }
  gpio_pullup_en(GPIO_NUM_18); // Keep RX recessive if the Grove cable is unplugged.
  shared.bus.ready = true;
  shared.bus.state = TWAI_STATE_RUNNING;
  if (xTaskCreatePinnedToCore(run, "npb-can", 4096, nullptr, 3, nullptr, 1) != pdPASS) {
    twai_stop(); twai_driver_uninstall(); shared.bus.ready = false; shared.bus.error = ESP_ERR_NO_MEM;
  }
  return shared.bus.ready;
}
Snapshot snapshot() {
  portENTER_CRITICAL(&mux);
  Snapshot result = shared;
  portEXIT_CRITICAL(&mux);
  return result;
}
size_t traffic(Traffic* out, size_t capacity) {
  portENTER_CRITICAL(&mux);
  const size_t n = min(count, capacity);
  for (size_t i = 0; i < n; ++i) out[i] = frames[(head + TRAFFIC_CAP - n + i) % TRAFFIC_CAP];
  portEXIT_CRITICAL(&mux);
  return n;
}
const char* stateName(const Diagnostics& d) {
  if (!d.ready) return "Driver failed";
  if (d.state == TWAI_STATE_BUS_OFF) return "Bus off";
  if (d.state == TWAI_STATE_RECOVERING) return "Recovering";
  if (d.state == TWAI_STATE_STOPPED) return "Stopped";
  if (d.txErrors >= 128 || d.rxErrors >= 128) return "Error passive";
  if (d.txErrors >= 96 || d.rxErrors >= 96) return "Error warning";
  return "Running";
}
bool controlsEnabled() { portENTER_CRITICAL(&mux); bool result = enabled; portEXIT_CRITICAL(&mux); return result; }
bool paused() { portENTER_CRITICAL(&mux); bool result = pollPaused; portEXIT_CRITICAL(&mux); return result; }
void setPaused(bool value) { portENTER_CRITICAL(&mux); pollPaused = value; portEXIT_CRITICAL(&mux); }
ControlStatus controlStatus() { portENTER_CRITICAL(&mux); auto result = jobStatus; portEXIT_CRITICAL(&mux); return result; }
bool acCyclePending() { portENTER_CRITICAL(&mux); bool result = cyclePending; portEXIT_CRITICAL(&mux); return result; }
void setAcCyclePending(bool value) { portENTER_CRITICAL(&mux); cyclePending = value; portEXIT_CRITICAL(&mux); }
const char* setControlsEnabled(bool value) {
  const auto current = snapshot();
  if (value && (!current.bus.ready || current.bus.state != TWAI_STATE_RUNNING || current.bus.txErrors || current.bus.rxErrors || millis() - current.bus.lastErrorAt < 2000 || !npb::statusFresh(current.charger, millis()))) return "Healthy CAN and fresh status are required";
  portENTER_CRITICAL(&mux); enabled = value; portEXIT_CRITICAL(&mux); return nullptr;
}
static const char* submitImpl(const npb::Frame* data, size_t size, uint32_t& id, const control::ChargeReview* review, bool localOperation) {
  if (!size || size > 20) return "Invalid request length";
  const auto current = snapshot();
  if (!current.bus.ready || current.bus.state != TWAI_STATE_RUNNING) return "CAN driver is not running";
  if (review) { if (const char* error = control::chargeError(current.charger, *review, millis())) return error; }
  if (review && acCyclePending()) return "Manual AC power cycle acknowledgment required";
  for (size_t i = 0; i < size; ++i) {
    if (data[i].length == 2) {
      if (!npb::permittedRead(data[i], data[i].id & 0xff)) return "Unsupported read request";
    } else {
      if (!(localOperation && control::command(data[i]) == 0) && !controlsEnabled()) return "Unlock controls first";
      const char* error = control::stateError(current.charger, data[i], millis());
      if (error) return error;
    }
  }
  portENTER_CRITICAL(&mux);
  if (reserved) { portEXIT_CRITICAL(&mux); return "A command is already pending"; }
  reserved = true; pendingRequest.count = size; pendingRequest.id = nextId++;
  pendingRequest.localOperation = localOperation;
  pendingRequest.charge = review != nullptr;
  if (review) pendingRequest.review = *review;
  for (size_t i = 0; i < size; ++i) pendingRequest.frames[i] = data[i];
  id = pendingRequest.id; jobStatus = ControlStatus{}; jobStatus.id = id; jobStatus.count = size;
  jobStatus.updated = millis(); strlcpy(jobStatus.state, "queued", sizeof(jobStatus.state));
  requestPending = true; portEXIT_CRITICAL(&mux); return nullptr;
}
const char* submit(const npb::Frame* data, size_t size, uint32_t& id, const control::ChargeReview* review) {
  return submitImpl(data, size, id, review, false);
}
const char* submitLocalCharge(const control::ChargeReview& review, uint32_t& id) {
  npb::Frame data[2]; control::encode(0, 0, data[0]); control::encode(0, 1, data[1]);
  return submitImpl(data, 2, id, &review, true);
}
const char* submitLocalStop(uint32_t& id) {
  const auto current = snapshot();
  if (!current.bus.ready || current.bus.state != TWAI_STATE_RUNNING) return "CAN unavailable. Ask an adult.";
  portENTER_CRITICAL(&mux);
  if (stopPending) { id = stopId; portEXIT_CRITICAL(&mux); return nullptr; }
  stopId = id = nextId++; stopPending = true; reserved = true;
  jobStatus = ControlStatus{}; jobStatus.id = id; jobStatus.count = 1; jobStatus.updated = millis();
  strlcpy(jobStatus.state, "stop_queued", sizeof(jobStatus.state));
  portEXIT_CRITICAL(&mux);
  return nullptr;
}
}
