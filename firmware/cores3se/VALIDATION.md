# Hardware validation - 2026-11-07

Target: actual CoreS3-SE / Base DIN / isolated Unit CAN on blue Port C, Mean Well NPB-450-24NFC address 0. Source branch: `feature/cores3se-direct-can`. Installed release: 0.5.1. No changes to the original Python application or its tests.

0.5.1 binary SHA-256: `6647073A40F8C45B76F86890C87CBF995906D67EB1C98261573F773CE6EC5FFC`. After the touchscreen charging test, the snapshot shows Operation OFF, 27.22 V / 0 A, website controls locked, zero TX/RX error counters, zero failed transmissions and max CAN task gap 3 ms. System configuration `0x0401` previously survived the charger's AC cycle.

## Touchscreen update: 0.5.0 / 0.5.1

- Built and installed by authenticated Wi-Fi update; running version and reboot confirmed. 0.5.1 uses 72,304 bytes static RAM and 1,103,997 bytes application flash; the 320x240 framebuffer is allocated in PSRAM.
- Inspected an actual framebuffer capture from the installed device: large Charge now / Stop charging buttons, readable live values, status and navigation fit without overlap.
- All 13 host tests passed, including two-second hold duration, required release after opening, one submission per hold, early release, sliding out, multiple fingers, review expiry and timer rollover. Existing charger guards remain covered.
- HTTP validation and manual-read regression passed after installation; remote writes remain locked. Local touchscreen actions have Operation-only authority. Stop can replace an unfinished start sequence and is still verified by readback.
- Observed the user's physical touch activity over Wi-Fi on 0.5.0: review opened and Cancel returned home without starting; a later hold displayed the full-screen progress ring (actual framebuffer captured), queued local job 4, verified OFF then ON, and reached 4.96 A. A single Stop tap queued job 5 and verified OFF within about 400 ms. Current returned to 0 A. Website controls stayed locked. The user subsequently confirmed: "Ring, cancellation, start and stop all work." This completes the requested physical checks for animation, early-release cancellation, charge start and one-tap stop; the reply did not identify which firmware revision was under test.
- Repeated the slow-header regression on 0.5.0: no reboot, 34 new replies, max CAN service gap 3 ms. Screenshot requests without the device key return 403. mDNS resolves to 192.168.4.165.
- 0.5.1 enlarges the ring to leave more room for its text and rejects a replacement finger inheriting a prior hold; all 13 tests pass again. No charger settings were changed for this refinement.

## Confirmed results

| Check | Evidence / result |
|---|---|
| Build | PlatformIO 6.1.19; pinned Espressif platform 7.0.1 and M5Unified/M5GFX 0.3.0. 0.4.3 uses 72,304 bytes static RAM and 1,090,001 bytes application flash. |
| USB identification / initial flash | ESP32-S3 rev 0.2, COM3, MAC F4:12:FA:B9:3A:10. Flash verification passed. |
| Screen | User confirmed NPB-450 Monitor and readings. M5Unified reported touch present, PSRAM present and board type 10. The subsequent landscape three-tab UI runs in firmware; final touch-tab and physical stale-screen inspection are still user checks. |
| Direct CAN | All 16 polled registers decoded with valid ages. Initial communication was restored when the user corrected reversed H/L wiring. |
| Live readings | Before connecting a battery: 0.05 V, 0 A, about 42 C, output-off flag. With the user's TimeUSB battery: 26.99 V, 0 A, about 43 C. No independent meter comparison yet. |
| Physical CAN disconnect | User unplugged the Grove cable while Wi-Fi remained powered. `/api/status` returned null measurements, unknown state, stale per-register quality and `can_online=false`; browser showed `--` and CAN stale. Unlock was refused. Replies and valid measurements recovered after reconnection. |
| Network | RoseHome station connection, NVS credentials saved, IP 192.168.4.165. `npb-monitor.local` resolves on this PC and serves the page. Actual STA hostname readback is `npb-monitor`; eero's displayed name was still generic in the user's screenshot. |
| Wi-Fi updates | Multiple authenticated uploads and reboots succeeded with no USB cable in the garage. Uploads without the key returned HTTP 403. |
| Protocol unit tests | 10 C++ tests passed: frames, decoding, signed temperature, freshness/rollover, flags, limits, charge guards, dependent voltage limits, history and preservation of startup behavior when changing persistence. |
| Existing app tests | All 15 Python tests passed using `src` on the module search path. |
| HTTP controls | Unauthorized requests, locked writes, invalid numeric values, reserved system bits and arbitrary raw reads were rejected. Manual read jobs transmitted successfully. |
| Operation writes / actual charging | After the user fitted the pin 7-to-8 hardware enable jumper, a supervised Charge now OFF/readback/ON/readback sequence started real charging. Readings reached 4.95-4.96 A at 27.75-28.09 V, with Constant current phase and fault flags 0. OFF was then sent and verified; current returned to 0 A, output-off flag was set, and controls were locked. |
| Parameter writes | With Operation OFF, CC changed from 5.00 A to 4.90 A and was read back, then restored to 5.00 A and read back. No curve voltage was changed. |
| System configuration | Startup OFF selected by changing `0x00C2` from `0x0403` to `0x0401`, then verified. After the user's physical AC cycle, configuration still read `0x0401` and Operation remained OFF. CAN remains enabled and EEPROM parameter writes remain disabled. |
| Responsive UI | Desktop and 390-pixel viewport checked. Two metric columns and one content column on mobile; no body overflow, including expanded configuration-byte forms. Profile staging sent no writes. Charge confirmation required both checks. Browser console showed no errors. |
| Slow HTTP regression | An incomplete header request reproducibly rebooted 0.4.1. 0.4.2 added bounded, nonblocking header admission before the Arduino parser. The same six-second test then passed: no reboot, 34 new replies, no receive drops, max CAN service gap 3 ms. |

## Resolved hardware enable

The first Operation ON test verified the register but produced no charging current. The user confirmed the Remote ON/OFF jumper was absent, then fitted the manufacturer's pin 7-to-8 jumper with charger AC unplugged and restored AC. Before the second test, all 16 readbacks were fresh, CAN error counters were zero, Operation remained OFF, and the complete curve was unchanged (CC 5 A, CV 28.4 V, float 27 V, taper 0.5 A, restart 26.4 V). The second test produced real charging current and a verified stop. Accumulated CAN failures from the AC-off interval are historical; current counters returned to zero.

## Remaining checks / limitations

- Diagnostics/Network touch-tab and physical stale-screen checks; independent voltage/current comparison. Home-screen animation, cancellation, start and stop are user-confirmed.
- On-hardware validation of every setpoint and mode switch. Frame validation is tested; changing modes, persistence and every voltage setting has deliberately not been exercised on a connected battery merely for coverage.
- No automatic standby charging or storage discharge. The garage can freeze and no battery temperature sensor is installed. The charger cannot sink current; automatic discharge needs additional hardware and battery-charge measurement.
- Long-duration soak, AP failover on the final release, sustained 24-hour history and memory-pressure testing are not yet completed.
- A loss of CAN or controller power cannot guarantee the charger turns off. This is not an independent hardware protection circuit.

## Local evidence

Detailed logs are intentionally Git-ignored under `C:/repos/npb-monitor/logs/`:

- `cores3se-charger-live.json`, `cores3se-modular-status.json`
- `cores3se-disconnect-confirmed.json`, `cores3se-control-stale-checks.json`
- `cores3se-charge-cycle.json`, `cores3se-setpoint-roundtrip.json`, `cores3se-startup-off.json`
- `cores3se-after-jumper.json`, `cores3se-charge-with-jumper.json`, `cores3se-final-status.json`
- `cores3se-slow-http-reproduction.json`, `cores3se-slow-http-fixed.json`
- `cores3se-final-api-checks.json`, `cores3se-0.4.3-build.log`
- `cores3se-touch-controls.json`, `cores3se-touch-slow-http.json`, `touch-validation/observations.jsonl`

UI captures are in `docs/screenshots/cores3se-desktop.jpg` and `cores3se-mobile.jpg` (0.4.2; the 0.4.3 web UI is unchanged). `cores3se-touch-home.png`, `cores3se-touch-225347-review.png` and `cores3se-touch-229669-holding.png` capture actual 0.5.0 device frames; 0.5.1 slightly enlarges the ring. No credentials appear in these captures.
