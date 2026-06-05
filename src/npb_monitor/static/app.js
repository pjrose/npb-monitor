const stateUrl = "/api/state";
const fmt = (value, digits = 1, unit = "") =>
  value === null || value === undefined || Number.isNaN(Number(value))
    ? "--"
    : `${Number(value).toFixed(digits)}${unit}`;
const alertSeen = new Set();
let alertsMuted = localStorage.getItem("npb_alerts_muted") === "true";
let chargeProfiles = [];

const flagLabels = {
  operation_on: "Operation on",
  over_temperature: "Over temperature",
  output_over_voltage: "Output over-voltage",
  output_over_current: "Output over-current",
  output_short_circuit: "Output short circuit",
  ac_abnormal: "AC abnormal",
  output_off: "Output off",
  internal_temperature_abnormal: "Internal temp abnormal",
  fully_charged: "Fully charged",
  constant_current_mode: "Constant current",
  constant_voltage_mode: "Constant voltage",
  float_mode: "Float",
  wake_up_unfinished: "Wake-up unfinished",
  temperature_compensation_short: "Temp compensation short",
  no_battery_detected: "No battery detected",
  cc_timeout: "CC timeout",
  cv_timeout: "CV timeout",
  float_timeout: "Float timeout",
  dc_output_normal_range: "DC output normal",
  initial_state: "Initial state",
  eeprom_access_error: "EEPROM access error",
  charger_mode: "Charger mode",
  restart_after_full_enabled: "Restart after full",
  disable_eeprom_writes: "EEPROM writes disabled",
};

function byId(id) {
  return document.getElementById(id);
}

async function api(path, payload = {}) {
  const response = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  const data = await response.json();
  if (!response.ok || data.ok === false) {
    throw new Error(data.error || "Request failed");
  }
  return data;
}

function setPill(element, text, good, bad = false) {
  element.textContent = text;
  element.classList.toggle("good", Boolean(good));
  element.classList.toggle("bad", Boolean(bad));
}

function updateReadings(readings, meta = {}) {
  byId("outputVoltage").textContent = fmt(readings.output_voltage_v, 2, " V");
  byId("outputCurrent").textContent = fmt(readings.output_current_a, 2, " A");
  byId("outputPower").textContent = fmt(readings.output_power_w, 0, " W");
  byId("temperature").textContent = fmt(readings.temperature_c, 1, " C");
  byId("targetVoltage").textContent = fmt(readings.target_voltage_v, 2, " V");
  byId("targetCurrent").textContent = fmt(readings.target_current_a, 2, " A");
  byId("operationState").textContent = readings.operation_on === true ? "On" : readings.operation_on === false ? "Off" : "--";
  byId("batteryVoltage").textContent = fmt(readings.output_voltage_v, 1, " V");

  const volts = Number(readings.output_voltage_v || 0);
  const fill = Math.max(0, Math.min(100, ((volts - 21) / (29.2 - 21)) * 100));
  const fillEl = byId("batteryFill");
  fillEl.style.width = `${fill}%`;
  fillEl.style.background = volts > 29.2 ? "var(--red)" : volts > 28.8 ? "var(--amber)" : "linear-gradient(90deg, var(--green), var(--cyan))";
  updateStaleReadings(meta);
  updateReadbackInputs(readings, meta);
}

function updateReadbackInputs(readings, meta = {}) {
  document.querySelectorAll("input[data-readback]").forEach((input) => {
    const key = input.dataset.readback;
    const value = readings[key];
    input.classList.toggle("stale", Boolean(meta[key]?.stale));
    input.title = meta[key]?.stale
      ? `Stale ${meta[key].source || "readback"}; waiting for charger reply`
      : `Read back from charger${meta[key]?.at ? ` at ${meta[key].at}` : ""}`;
    if (value === null || value === undefined || Number.isNaN(Number(value))) return;
    if (document.activeElement === input || input.dataset.dirty === "true") return;
    const digits = Number(input.dataset.digits || 2);
    input.value = Number(value).toFixed(digits);
  });
}

function updateStaleReadings(meta = {}) {
  document.querySelectorAll("[data-reading]").forEach((element) => {
    const keys = element.dataset.reading.split(/\s+/).filter(Boolean);
    const stale = keys.some((key) => meta[key]?.stale);
    const ages = keys.map((key) => meta[key]?.age_s).filter((age) => age !== null && age !== undefined);
    element.classList.toggle("stale", stale);
    element.title = stale ? `Waiting for fresh readback${ages.length ? `; latest age ${Math.max(...ages).toFixed(0)}s` : ""}` : "";
  });
}

function updateFlags(flags) {
  const host = byId("flags");
  const entries = Object.entries(flagLabels).filter(([key]) => key in flags);
  if (!entries.length) {
    host.innerHTML = '<div class="flag"><span>Waiting for status readback</span><strong>--</strong></div>';
    byId("faultSummary").textContent = "Waiting for readback";
    byId("chargeMode").textContent = "Unknown mode";
    return;
  }
  host.innerHTML = entries
    .map(([key, label]) => `<div class="flag ${flags[key] ? "active" : ""}"><span>${label}</span><strong>${flags[key] ? "Yes" : "No"}</strong></div>`)
    .join("");

  const badKeys = [
    "over_temperature",
    "output_over_voltage",
    "output_over_current",
    "output_short_circuit",
    "ac_abnormal",
    "internal_temperature_abnormal",
    "temperature_compensation_short",
    "no_battery_detected",
    "cc_timeout",
    "cv_timeout",
    "float_timeout",
    "eeprom_access_error",
  ];
  const activeBad = badKeys.filter((key) => flags[key]);
  byId("faultSummary").textContent = activeBad.length ? `${activeBad.length} active` : "No active faults";
  byId("chargeMode").textContent = flags.charger_mode ? "Charger mode" : flags.charger_mode === false ? "PSU mode" : "Unknown mode";

  const btnPsu = byId("btnPsuMode");
  const btnCharger = byId("btnChargerMode");
  if (btnPsu && btnCharger) {
    if (flags.charger_mode === true) {
      btnCharger.className = "good";
      btnPsu.className = "ghost";
    } else if (flags.charger_mode === false) {
      btnCharger.className = "ghost";
      btnPsu.className = "good";
    } else {
      btnCharger.className = "ghost";
      btnPsu.className = "ghost";
    }
  }

  const eepromBtn = byId("btnEepromToggle");
  if (eepromBtn) {
    if (flags.disable_eeprom_writes === true) {
      eepromBtn.textContent = "Enable EEPROM writes";
      eepromBtn.dataset.action = "enable_eeprom_writes";
      eepromBtn.className = "danger";
    } else if (flags.disable_eeprom_writes === false) {
      eepromBtn.textContent = "Disable EEPROM writes";
      eepromBtn.dataset.action = "disable_eeprom_writes";
      eepromBtn.className = "good";
    } else {
      eepromBtn.textContent = "Enable CAN control";
      eepromBtn.dataset.action = "disable_eeprom_writes";
      eepromBtn.className = "";
    }
  }
}

function updateAlerts(alerts) {
  const host = byId("alerts");
  if (!alerts || !alerts.length) {
    host.hidden = true;
    host.innerHTML = "";
    return;
  }
  host.hidden = false;
  host.innerHTML = alerts.map((alert) => (
    `<article class="alert ${escapeHtml(alert.severity)}">
      <strong>${escapeHtml(alert.title)}</strong>
      <span>${escapeHtml(alert.message)}</span>
    </article>`
  )).join("");

  const audible = alerts.filter((alert) => alert.severity !== "info" && !alertSeen.has(alert.id));
  alerts.forEach((alert) => alertSeen.add(alert.id));
  if (audible.length && !alertsMuted) beep(audible.some((alert) => alert.severity === "critical"));
}

function updateEvents(events) {
  byId("events").innerHTML = events.slice(0, 12).map((event) => (
    `<div class="event"><strong>${event.kind}</strong> ${event.message}<br><small>${event.at}</small></div>`
  )).join("");
}

function updateNotices(notices) {
  const host = byId("notices");
  if (!host) return;
  if (!notices || !notices.length) {
    host.innerHTML = "";
    host.hidden = true;
    return;
  }
  host.hidden = false;
  host.innerHTML = notices.map((notice) => (
    `<article class="notice ${notice.severity}">
      <strong>${escapeHtml(notice.title)}</strong>
      <span>${escapeHtml(notice.message)}</span>
      <small>${escapeHtml(notice.at)}</small>
    </article>`
  )).join("");
}

function updateFrames(frames) {
  byId("frames").innerHTML = frames.slice(0, 24).map((frame) => (
    `<div class="frame"><strong>${frame.direction.toUpperCase()}</strong> ${frame.topic}<br><small>${frame.at}</small><code>${escapeHtml(JSON.stringify(frame.payload, null, 2))}</code></div>`
  )).join("");
}

function updateDecoded(commands, meta = {}) {
  const host = byId("decodedFrames");
  const rows = Object.values(commands || {})
    .sort((a, b) => Number(a.command || 0) - Number(b.command || 0))
    .map((command) => {
      const readingMeta = Object.values(meta).find((item) => item.source === command.name);
      const raw = command.raw_value === null || command.raw_value === undefined ? "--" : `0x${Number(command.raw_value).toString(16).toUpperCase().padStart(4, "0")}`;
      const value = command.value === null || command.value === undefined ? "--" : `${command.value}${command.unit ? ` ${command.unit}` : ""}`;
      const data = (command.data || []).map((byte) => Number(byte).toString(16).toUpperCase().padStart(2, "0")).join(" ");
      return `<div class="decoded-row ${readingMeta?.stale ? "stale" : ""}">
        <span>${escapeHtml(command.name)}</span>
        <span>0x${Number(command.command).toString(16).toUpperCase().padStart(4, "0")}</span>
        <span>${escapeHtml(raw)}</span>
        <span>${escapeHtml(value)}</span>
        <span>${escapeHtml(formatAge(readingMeta?.age_s))}</span>
        <code>${escapeHtml(data)}</code>
      </div>`;
    });
  host.innerHTML = rows.length
    ? `<div class="decoded-head"><span>Name</span><span>Cmd</span><span>Raw</span><span>Value</span><span>Age</span><span>Data</span></div>${rows.join("")}`
    : '<div class="event">Waiting for decoded charger replies.</div>';
}

function updateSession(session) {
  if (!session) return;
  byId("sessionElapsed").textContent = `${formatDuration(session.elapsed_seconds)} this run`;
  byId("sessionStats").innerHTML = [
    ["Samples", session.sample_count ?? 0],
    ["Energy", fmt(session.watt_hours, 2, " Wh")],
    ["Max V", fmt(session.max_voltage_v, 2, " V")],
    ["Max A", fmt(session.max_current_a, 2, " A")],
    ["Max W", fmt(session.max_power_w, 0, " W")],
    ["Max C", fmt(session.max_temperature_c, 1, " C")],
    ["Events", session.event_count ?? 0],
    ["Started", session.started_at || "--"],
  ].map(([label, value]) => `<div class="stat"><span>${label}</span><strong>${value}</strong></div>`).join("");
}

function renderProfiles() {
  const select = byId("profileSelect");
  if (!select) return;
  select.innerHTML = chargeProfiles
    .map((profile) => `<option value="${escapeHtml(profile.id)}">${escapeHtml(profile.label)}</option>`)
    .join("");
  updateProfileNote();
}

function selectedProfile() {
  const id = byId("profileSelect")?.value;
  return chargeProfiles.find((profile) => profile.id === id) || chargeProfiles[0];
}

function updateProfileNote() {
  const profile = selectedProfile();
  const note = byId("profileNote");
  if (!profile || !note) return;
  note.innerHTML = `<strong>${escapeHtml(profile.label)}</strong> ${escapeHtml(profile.description)}<br>${profile.notes.map(escapeHtml).join("<br>")}`;
}

function loadSelectedProfile() {
  const profile = selectedProfile();
  if (!profile) return;
  const fields = [
    ["curveCurrent", profile.cc_current_a, 2],
    ["curveCv", profile.cv_voltage_v, 2],
    ["curveFloat", profile.float_voltage_v, 2],
    ["curveTaper", profile.taper_current_a, 2],
    ["curveRestart", profile.restart_voltage_v, 2],
  ];
  fields.forEach(([id, value, digits]) => {
    const input = byId(id);
    if (!input) return;
    input.value = Number(value).toFixed(digits);
    input.dataset.dirty = "true";
    input.classList.add("profile-loaded");
  });
  byId("profileNote").innerHTML = `<strong>${escapeHtml(profile.label)} loaded into inputs.</strong> Review current for your battery capacity/BMS, then press Set on each row to write.`;
}

function formatAge(age) {
  if (age === null || age === undefined || Number.isNaN(Number(age))) return "--";
  if (age < 60) return `${Number(age).toFixed(0)}s`;
  return `${(Number(age) / 60).toFixed(1)}m`;
}

function formatDuration(seconds) {
  const total = Math.max(0, Math.floor(Number(seconds || 0)));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  if (h) return `${h}h ${m}m`;
  if (m) return `${m}m ${s}s`;
  return `${s}s`;
}

function beep(critical) {
  try {
    const context = new AudioContext();
    const oscillator = context.createOscillator();
    const gain = context.createGain();
    oscillator.type = critical ? "square" : "sine";
    oscillator.frequency.value = critical ? 880 : 660;
    gain.gain.value = 0.0001;
    oscillator.connect(gain);
    gain.connect(context.destination);
    oscillator.start();
    gain.gain.exponentialRampToValueAtTime(0.08, context.currentTime + 0.02);
    gain.gain.exponentialRampToValueAtTime(0.0001, context.currentTime + 0.24);
    oscillator.stop(context.currentTime + 0.26);
  } catch {
    // Browser autoplay policy may block audio until the first user gesture.
  }
}

function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, (char) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#039;",
  })[char]);
}

async function refresh() {
  const data = await fetch(stateUrl).then((response) => response.json());
  setPill(byId("mqttPill"), data.mqtt_connected ? "MQTT connected" : "MQTT offline", data.mqtt_connected, !data.mqtt_connected);
  setPill(byId("wicanPill"), `WiCAN ${data.wican_status}`, data.wican_status === "online", data.wican_status === "offline");
  const staleRx = data.alerts?.some((alert) => alert.id === "stale_rx" || alert.id === "no_rx");
  setPill(byId("rxPill"), data.last_rx_at ? (staleRx ? "RX stale" : "RX active") : "No RX", Boolean(data.last_rx_at) && !staleRx, staleRx);
  byId("lastSeen").textContent = data.last_rx_at ? `Last RX ${data.last_rx_at}` : "No traffic yet";
  byId("pauseExchange").textContent = data.data_exchange_paused ? "Resume" : "Pause";
  byId("pauseExchange").classList.toggle("active", Boolean(data.data_exchange_paused));
  byId("muteAlerts").textContent = alertsMuted ? "Unmute" : "Mute";
  byId("muteAlerts").classList.toggle("active", alertsMuted);
  updateReadings(data.readings, data.readings_meta);
  updateFlags(data.flags);
  updateAlerts(data.alerts);
  updateNotices(data.notices);
  updateEvents(data.events);
  updateFrames(data.frames);
  updateDecoded(data.commands, data.readings_meta);
  updateSession(data.session);
}

async function loadConfig() {
  const config = await fetch("/api/config").then((response) => response.json());
  byId("canIds").textContent = `${config.controller_can_id} -> ${config.charger_can_id}`;
  document.querySelector("#rawForm input[name='can_id']").value = config.controller_can_id;
}

async function loadProfiles() {
  chargeProfiles = await fetch("/api/profiles").then((response) => response.json());
  renderProfiles();
}

document.addEventListener("click", async (event) => {
  if (event.target.closest("#loadProfile")) {
    loadSelectedProfile();
    return;
  }
  if (event.target.closest("#muteAlerts")) {
    alertsMuted = !alertsMuted;
    localStorage.setItem("npb_alerts_muted", String(alertsMuted));
    byId("muteAlerts").textContent = alertsMuted ? "Unmute" : "Mute";
    return;
  }
  if (event.target.closest("#pauseExchange")) {
    const paused = byId("pauseExchange").textContent !== "Resume";
    await api("/api/control", { action: "set_paused", paused });
    await refresh();
    return;
  }
  const button = event.target.closest("button[data-action]");
  if (!button) return;
  const action = button.dataset.action;
  try {
    if (action === "poll") {
      await api("/api/poll");
    } else if (action === "operation") {
      await api("/api/control", { action, enabled: button.dataset.enabled === "true" });
    } else {
      await api("/api/control", { action });
    }
    await refresh();
  } catch (error) {
    alert(error.message);
  }
});

document.addEventListener("change", (event) => {
  if (event.target.closest("#profileSelect")) updateProfileNote();
});

document.addEventListener("submit", async (event) => {
  const form = event.target;
  if (!(form instanceof HTMLFormElement)) return;
  event.preventDefault();
  try {
    if (form.id === "rawForm") {
      await api("/api/raw", Object.fromEntries(new FormData(form)));
    } else if (form.dataset.control) {
      await api("/api/control", { action: form.dataset.control, ...Object.fromEntries(new FormData(form)) });
      form.querySelectorAll("input[data-readback]").forEach((input) => {
        input.dataset.dirty = "false";
      });
    }
    await refresh();
  } catch (error) {
    alert(error.message);
  }
});

document.addEventListener("input", (event) => {
  const input = event.target.closest("input[data-readback]");
  if (input) input.dataset.dirty = "true";
});

loadConfig().catch(console.error);
loadProfiles().catch(console.error);
refresh().catch(console.error);
setInterval(() => refresh().catch(console.error), 1000);
