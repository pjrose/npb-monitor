#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <unity.h>
#include "npb_protocol.h"
#include "charger_control.h"
#include "history.h"
#include "touch_controls.h"
using namespace npb;
void setUp() {}
void tearDown() {}
Frame reply(uint16_t command, uint16_t raw, uint8_t length = 4) {
  Frame f;
  f.id = chargerId(0); f.extended = true; f.length = length;
  f.data[0] = command & 0xff; f.data[1] = command >> 8;
  f.data[2] = raw & 0xff; f.data[3] = raw >> 8;
  return f;
}
void test_encoding_and_read_only_gate() {
  Frame f;
  TEST_ASSERT_EQUAL_HEX32(0x000C0103, controllerId(3));
  TEST_ASSERT_EQUAL_HEX32(0x000C0003, chargerId(3));
  for (auto command : COMMANDS) {
    TEST_ASSERT_TRUE(makeRead(command, 0, f));
    TEST_ASSERT_TRUE(permittedRead(f, 0));
    TEST_ASSERT_EQUAL_UINT8(command & 0xff, f.data[0]);
    TEST_ASSERT_EQUAL_UINT8(command >> 8, f.data[1]);
    f.length = 3; // Includes OPERATION on/off: every write is denied.
    TEST_ASSERT_FALSE(permittedRead(f, 0));
    f.length = 4;
    TEST_ASSERT_FALSE(permittedRead(f, 0));
  }
  TEST_ASSERT_FALSE(makeRead(0x1234, 0, f));
  TEST_ASSERT_FALSE(makeRead(0x0060, 4, f));
  makeRead(0x0060, 0, f); f.extended = false;
  TEST_ASSERT_FALSE(permittedRead(f, 0));
  f.extended = true; f.rtr = true;
  TEST_ASSERT_FALSE(permittedRead(f, 0));
  f.rtr = false; f.id = chargerId(0);
  TEST_ASSERT_FALSE(permittedRead(f, 0));
}
void test_scaling_and_payload_validation() {
  State s;
  TEST_ASSERT_TRUE(s.accept(reply(0x0060, 2400), 0));
  TEST_ASSERT_TRUE(s.accept(reply(0x0061, 500), 1));
  TEST_ASSERT_TRUE(s.accept(reply(0x0062, 250), 2));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 24.0, s.get(0x0060).raw * scale(0x0060));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 5.0, s.get(0x0061).raw * scale(0x0061));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 25.0, s.get(0x0062).raw * scale(0x0062));
  TEST_ASSERT_FLOAT_WITHIN(0.001, -5.0, decodedValue(0x0062, uint16_t(int16_t(-50))));
  TEST_ASSERT_FLOAT_WITHIN(0.001, 26.99, decodedValue(0x0060, 2699));
  TEST_ASSERT_FALSE(s.accept(reply(0x0060, 0, 3), 100));
  TEST_ASSERT_FALSE(s.accept(reply(0x1234, 0), 100));
  auto f = reply(0x0060, 0); f.extended = false;
  TEST_ASSERT_FALSE(s.accept(f, 100));
  f.extended = true; f.rtr = true;
  TEST_ASSERT_FALSE(s.accept(f, 100));
  f.rtr = false; f.id = chargerId(1);
  TEST_ASSERT_FALSE(s.accept(f, 100));
  f.id = chargerId(0); f.length = 9;
  TEST_ASSERT_FALSE(s.accept(f, 100));
  TEST_ASSERT_EQUAL_UINT32(3, s.received);
  TEST_ASSERT_EQUAL_UINT32(2, s.lastReply);
}
void test_staleness_and_millis_wrap() {
  State s;
  TEST_ASSERT_EQUAL_STRING("unavailable", s.get(0x0060).quality(0));
  s.accept(reply(0x0060, 2400), 0);
  TEST_ASSERT_TRUE(s.get(0x0060).fresh(14999));
  TEST_ASSERT_FALSE(s.get(0x0060).fresh(15000));
  TEST_ASSERT_EQUAL_STRING("stale", s.get(0x0060).quality(15000));
  s.accept(reply(0x0061, 200), 16000);
  TEST_ASSERT_TRUE(s.online(16000));
  TEST_ASSERT_FALSE(s.get(0x0060).fresh(16000));
  s.accept(reply(0x0060, 2400), UINT32_MAX - 999);
  TEST_ASSERT_TRUE(s.get(0x0060).fresh(13999));
  TEST_ASSERT_FALSE(s.get(0x0060).fresh(14000));
}
void test_flags_and_phase() {
  State s;
  for (const auto& flag : WARNINGS) {
    s = State{};
    TEST_ASSERT_TRUE(s.accept(reply(flag.command, flag.mask), 0));
    TEST_ASSERT_TRUE(active(s, flag, 1));
    TEST_ASSERT_FALSE(active(s, flag, STALE_MS));
  }
  s.accept(reply(0x00B8, 0x0802), 0);
  TEST_ASSERT_EQUAL_STRING("Constant current", phase(s, 1));
  TEST_ASSERT_EQUAL_STRING("Unknown", phase(s, STALE_MS));
  s.accept(reply(0x00B4, 0x0084), 0);
  TEST_ASSERT_EQUAL_STRING("Charger", mode(s, 1));
  TEST_ASSERT_EQUAL_STRING("Unknown", mode(s, STALE_MS));
  s.accept(reply(0x0040, 0x0040, 3), 0);
  s.accept(reply(0x00C1, 0x0002, 3), 0);
  TEST_ASSERT_TRUE(statusFresh(s, 1));
  TEST_ASSERT_FALSE(statusFresh(s, STALE_MS));
}
void test_control_encoding_limits_and_reserved_bits() {
  Frame f;
  TEST_ASSERT_TRUE(control::encode(0, 1, f));
  TEST_ASSERT_EQUAL_UINT8(3, f.length);
  TEST_ASSERT_EQUAL_UINT8(1, f.data[2]);
  TEST_ASSERT_TRUE(control::validWrite(f));
  TEST_ASSERT_FALSE(permittedRead(f, 0));
  TEST_ASSERT_TRUE(control::encode(0x0020, 30.0, f));
  TEST_ASSERT_EQUAL_HEX8(0xB8, f.data[2]);
  TEST_ASSERT_EQUAL_HEX8(0x0B, f.data[3]);
  TEST_ASSERT_TRUE(control::encode(0x0030, 13.5, f));
  TEST_ASSERT_FALSE(control::encode(0x0030, 13.51, f));
  TEST_ASSERT_FALSE(control::encode(0x0020, 20.99, f));
  TEST_ASSERT_FALSE(control::encode(0x0020, 42.01, f));
  TEST_ASSERT_FALSE(control::encode(0x0020, NAN, f));
  TEST_ASSERT_FALSE(control::encode(0x0020, INFINITY, f));
  TEST_ASSERT_FALSE(control::encode(0x0020, 28.401, f));
  TEST_ASSERT_FALSE(control::encode(0x0060, 24, f));
  TEST_ASSERT_FALSE(control::encode(0x00B3, .26, f));
  TEST_ASSERT_TRUE(control::encode(0x00B3, .27, f));
  TEST_ASSERT_TRUE(control::encode(0x00B3, 4.05, f));
  TEST_ASSERT_FALSE(control::encode(0x00B3, 4.06, f));
  TEST_ASSERT_FALSE(control::encode(0x0030, 0, f));
  TEST_ASSERT_FALSE(control::encode(0x00B0, 2.69, f));
  TEST_ASSERT_TRUE(control::encode(0x00B0, 2.7, f));
  TEST_ASSERT_FALSE(control::encode(0x00C2, 0xffff, f));
  TEST_ASSERT_FALSE(control::encode(0x00C2, 0x0407, f));
  TEST_ASSERT_TRUE(control::encode(0x00C2, 0x0403, f));
  f.id = controllerId(1); TEST_ASSERT_FALSE(control::validWrite(f));
  f.id = controllerId(0); f.length = 3; TEST_ASSERT_FALSE(control::validWrite(f));
}
void test_control_freshness_and_fault_interlocks() {
  State s; Frame command;
  control::encode(0, 1, command);
  TEST_ASSERT_NOT_NULL(control::stateError(s, command, 0));
  s.accept(reply(0, 0, 3), 1); s.accept(reply(0x0040, 0x40), 1);
  s.accept(reply(0x00B8, 0), 1); s.accept(reply(0x00C1, 0), 1);
  s.accept(reply(0x00B4, 0x84), 1);
  TEST_ASSERT_NULL(control::stateError(s, command, 2));
  s.accept(reply(0x0040, 0x42), 2);
  TEST_ASSERT_NOT_NULL(control::stateError(s, command, 3));
  s.accept(reply(0x0040, 0x40), 3); s.accept(reply(0x00B8, 0x0800), 3);
  TEST_ASSERT_NOT_NULL(control::stateError(s, command, 4));
  s.accept(reply(0x00B8, 0), 4);
  TEST_ASSERT_NOT_NULL(control::stateError(s, command, 15005));
  control::encode(0, 0, command);
  TEST_ASSERT_NULL(control::stateError(s, command, 15005));
}
void test_history_retains_gaps_and_bounds_memory() {
  static history::Ring ring; State s;
  ring.record(s, 0);
  TEST_ASSERT_EQUAL_UINT32(1, ring.count());
  TEST_ASSERT_FALSE(ring.at(0).valid);
  s.accept(reply(0x0060, 2400), 60000); s.accept(reply(0x0061, 500), 60000);
  ring.record(s, 60000); TEST_ASSERT_TRUE(ring.at(1).valid);
  ring.record(s, 120000); TEST_ASSERT_FALSE(ring.at(2).valid);
  for (uint32_t i = 3; i < 1500; ++i) ring.record(s, i * 60000);
  TEST_ASSERT_EQUAL_UINT32(history::CAPACITY, ring.count());
  TEST_ASSERT_EQUAL_UINT32(59 * 60000, ring.at(0).ms);
  TEST_ASSERT_EQUAL_UINT32(1499 * 60000, ring.at(ring.count() - 1).ms);
}
void test_charge_review_guards_and_changed_settings() {
  State s;
  for (auto cmd : COMMANDS) s.accept(reply(cmd, 0), 100);
  const uint16_t raw[] = {500,2840,2700,50,2640,0x84,0x403};
  control::ChargeReview review;
  for (size_t i=0; i<7; ++i) {
    review.values[i]=raw[i]; s.accept(reply(control::REVIEW_COMMANDS[i],raw[i]),100);
  }
  s.accept(reply(0x0060,2699),100); s.accept(reply(0x0040,64),100);
  TEST_ASSERT_NULL(control::chargeError(s,review,101));
  s.accept(reply(0x00B1,2880),102);
  TEST_ASSERT_NOT_NULL(control::chargeError(s,review,103)); // Changed after confirmation.
  review.values[1]=2880;
  TEST_ASSERT_NULL(control::chargeError(s,review,103));
  s.accept(reply(0x0060,5),104);
  TEST_ASSERT_NOT_NULL(control::chargeError(s,review,105)); // No plausible battery voltage.
  s.accept(reply(0x0060,2699),106); s.accept(reply(0x00B8,0x0800),106);
  TEST_ASSERT_NOT_NULL(control::chargeError(s,review,107)); // Charger says no battery.
  s.accept(reply(0x00B8,0),108);
  review.values[0]=600; s.accept(reply(0x00B0,600),108);
  TEST_ASSERT_NOT_NULL(control::chargeError(s,review,109)); // Shortcut's recommended-current cap.
  review.values[0]=500; s.accept(reply(0x00B0,500),110);
  TEST_ASSERT_NOT_NULL(control::chargeError(s,review,15111));
}
void test_float_cannot_exceed_cv() {
  State s; Frame f;
  for (auto cmd : COMMANDS) s.accept(reply(cmd,0),1);
  s.accept(reply(0x00B1,2840),1); s.accept(reply(0x00B2,2700),1);
  control::encode(0x00B2,28.41,f);
  TEST_ASSERT_NOT_NULL(control::stateError(s,f,2));
  control::encode(0x00B2,28.4,f);
  TEST_ASSERT_NULL(control::stateError(s,f,2));
  control::encode(0x00B1,26.9,f);
  TEST_ASSERT_NOT_NULL(control::stateError(s,f,2));
}
void test_persistence_changes_preserve_startup_behavior() {
  State s; Frame f;
  TEST_ASSERT_FALSE(control::persistenceWrite(s,true,0,f));
  const uint16_t startupModes[] = {0,2,4};
  for (uint16_t startup : startupModes) {
    s.accept(reply(0x00C2,0x401|startup),100);
    TEST_ASSERT_TRUE(control::persistenceWrite(s,false,101,f));
    TEST_ASSERT_EQUAL_HEX16(1|startup,control::raw(f));
    s.accept(reply(0x00C2,1|startup),102);
    TEST_ASSERT_TRUE(control::persistenceWrite(s,true,103,f));
    TEST_ASSERT_EQUAL_HEX16(0x401|startup,control::raw(f));
  }
}
void test_touch_hold_requires_release_and_two_seconds() {
  touchcontrol::HoldToStart hold;
  hold.open(100);
  TEST_ASSERT_FALSE(hold.update(101, 1, true, true)); // Opening tap cannot carry through.
  TEST_ASSERT_FALSE(hold.update(3000, 1, true, false));
  TEST_ASSERT_FALSE(hold.update(3001, 0, false, false));
  TEST_ASSERT_FALSE(hold.update(3100, 1, true, true));
  TEST_ASSERT_FALSE(hold.update(5099, 1, true, false));
  TEST_ASSERT_EQUAL_UINT32(1999, hold.progress(5099));
  TEST_ASSERT_TRUE(hold.update(5100, 1, true, false));
  TEST_ASSERT_FALSE(hold.update(7100, 1, true, false)); // One submission only.
}
void test_touch_hold_release_move_and_second_finger_cancel() {
  for (unsigned cancel = 0; cancel < 3; ++cancel) {
    touchcontrol::HoldToStart hold; hold.open(0);
    hold.update(1, 0, false, false); hold.update(2, 1, true, true);
    TEST_ASSERT_FALSE(hold.update(1900, cancel == 0 ? 0 : cancel == 1 ? 1 : 2, cancel != 1, false));
    TEST_ASSERT_FALSE(hold.update(2100, 1, true, false)); // Sliding back cannot resume.
    TEST_ASSERT_EQUAL_UINT32(0, hold.progress(2100));
    hold.update(2200, 0, false, false); hold.update(2300, 1, true, true);
    TEST_ASSERT_TRUE(hold.update(4300, 1, true, false));
  }
}
void test_touch_review_expiry_and_clock_wrap() {
  touchcontrol::HoldToStart hold; hold.open(100);
  hold.update(101, 0, false, false); hold.update(29000, 1, true, true);
  TEST_ASSERT_TRUE(hold.expired(30100));
  TEST_ASSERT_FALSE(hold.update(31000, 1, true, false));
  hold.open(0xfffffff0u); hold.update(0xfffffff1u, 0, false, false);
  hold.update(0xfffffff2u, 1, true, true);
  TEST_ASSERT_FALSE(hold.update(1985, 1, true, false));
  TEST_ASSERT_TRUE(hold.update(1986, 1, true, false));
  hold.open(10000); hold.update(10001, 0, false, false); hold.update(10002, 1, true, true);
  TEST_ASSERT_FALSE(hold.update(11999, 1, true, true)); // Replacement finger cannot inherit elapsed time.
  TEST_ASSERT_FALSE(hold.update(12002, 1, true, false));
}
int runTests() {
#ifdef ARDUINO
  Serial.begin(115200);
  delay(2000);
#endif
  UNITY_BEGIN();
  RUN_TEST(test_encoding_and_read_only_gate);
  RUN_TEST(test_scaling_and_payload_validation);
  RUN_TEST(test_staleness_and_millis_wrap);
  RUN_TEST(test_flags_and_phase);
  RUN_TEST(test_control_encoding_limits_and_reserved_bits);
  RUN_TEST(test_control_freshness_and_fault_interlocks);
  RUN_TEST(test_history_retains_gaps_and_bounds_memory);
  RUN_TEST(test_charge_review_guards_and_changed_settings);
  RUN_TEST(test_float_cannot_exceed_cv);
  RUN_TEST(test_persistence_changes_preserve_startup_behavior);
  RUN_TEST(test_touch_hold_requires_release_and_two_seconds);
  RUN_TEST(test_touch_hold_release_move_and_second_finger_cancel);
  RUN_TEST(test_touch_review_expiry_and_clock_wrap);
  return UNITY_END();
}
#ifdef ARDUINO
void setup() { runTests(); }
void loop() { delay(100); }
#else
int main() { return runTests(); }
#endif
