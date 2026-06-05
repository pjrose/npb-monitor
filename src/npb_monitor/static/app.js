const stateUrl = "/api/state";
const fmt = (value, digits = 1, unit = "") =>
  value === null || value === undefined || Number.isNaN(Number(value))
    ? "--"
    : `${Number(value).toFixed(digits)}${unit}`;

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

function updateReadings(readings) {
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
}

function updateEvents(events) {
  byId("events").innerHTML = events.slice(0, 12).map((event) => (
    `<div class="event"><strong>${event.kind}</strong> ${event.message}<br><small>${event.at}</small></div>`
  )).join("");
}

function updateFrames(frames) {
  byId("frames").innerHTML = frames.slice(0, 24).map((frame) => (
    `<div class="frame"><strong>${frame.direction.toUpperCase()}</strong> ${frame.topic}<br><small>${frame.at}</small><code>${escapeHtml(JSON.stringify(frame.payload, null, 2))}</code></div>`
  )).join("");
}

function escapeHtml(text) {
  return text.replace(/[&<>"']/g, (char) => ({
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
  setPill(byId("rxPill"), data.last_rx_at ? "RX active" : "No RX", Boolean(data.last_rx_at), false);
  byId("lastSeen").textContent = data.last_rx_at ? `Last RX ${data.last_rx_at}` : "No traffic yet";
  updateReadings(data.readings);
  updateFlags(data.flags);
  updateEvents(data.events);
  updateFrames(data.frames);
}

async function loadConfig() {
  const config = await fetch("/api/config").then((response) => response.json());
  byId("canIds").textContent = `${config.controller_can_id} -> ${config.charger_can_id}`;
  document.querySelector("#rawForm input[name='can_id']").value = config.controller_can_id;
}

document.addEventListener("click", async (event) => {
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

document.addEventListener("submit", async (event) => {
  const form = event.target;
  if (!(form instanceof HTMLFormElement)) return;
  event.preventDefault();
  try {
    if (form.id === "rawForm") {
      await api("/api/raw", Object.fromEntries(new FormData(form)));
    } else if (form.dataset.control) {
      await api("/api/control", { action: form.dataset.control, ...Object.fromEntries(new FormData(form)) });
    }
    await refresh();
  } catch (error) {
    alert(error.message);
  }
});

loadConfig().catch(console.error);
refresh().catch(console.error);
setInterval(() => refresh().catch(console.error), 1000);
