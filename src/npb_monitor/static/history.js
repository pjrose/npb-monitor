const canvas = document.getElementById("historyChart");
const ctx = canvas.getContext("2d");
const countEl = document.getElementById("historyCount");
const rangeEl = document.getElementById("visibleRange");
const eventsEl = document.getElementById("chartEvents");
const readoutEl = document.getElementById("readout");

let samples = [];
let events = [];
let viewStart = Date.now() - 24 * 3600 * 1000;
let viewEnd = Date.now();
let dragging = false;
let dragStartX = 0;
let dragStartView = [viewStart, viewEnd];
let hoverX = null;

const colors = {
  bg: "#111317",
  grid: "#343a42",
  text: "#f4f1ea",
  muted: "#aaa69b",
  voltage: "#76c7d8",
  current: "#f7bd52",
  event: "#ff6b6b",
};

function padRange(min, max) {
  if (!Number.isFinite(min) || !Number.isFinite(max)) return [0, 1];
  if (min === max) return [min - 1, max + 1];
  const pad = (max - min) * 0.12;
  return [min - pad, max + pad];
}

function fitCanvas() {
  const ratio = window.devicePixelRatio || 1;
  const rect = canvas.getBoundingClientRect();
  canvas.width = Math.max(600, Math.floor(rect.width * ratio));
  canvas.height = Math.max(360, Math.floor(rect.height * ratio));
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
}

async function loadHistory() {
  const data = await fetch("/api/history").then((response) => response.json());
  samples = data.samples || [];
  events = (data.events || []).filter((event) => event.annotate).sort((a, b) => a.ts - b.ts);
  if (samples.length) {
    const lastTs = samples[samples.length - 1].ts;
    if (!viewStart || viewEnd > Date.now() - 5000) {
      setRangeHours(24, lastTs);
    }
  }
  countEl.textContent = `${samples.length} samples`;
  draw();
}

function setRangeHours(hours, anchor = latestTime()) {
  const span = hours * 3600 * 1000;
  viewEnd = anchor;
  viewStart = anchor - span;
  clampView();
  draw();
}

function latestTime() {
  return samples.length ? samples[samples.length - 1].ts : Date.now();
}

function clampView() {
  const day = 24 * 3600 * 1000;
  const latest = Math.max(latestTime(), Date.now());
  const earliest = latest - day;
  const span = Math.min(Math.max(viewEnd - viewStart, 60 * 1000), day);
  if (viewStart < earliest) {
    viewStart = earliest;
    viewEnd = earliest + span;
  }
  if (viewEnd > latest) {
    viewEnd = latest;
    viewStart = latest - span;
  }
}

function chartRect() {
  const rect = canvas.getBoundingClientRect();
  return { left: 62, right: rect.width - 62, top: 24, bottom: rect.height - 54, width: rect.width - 124, height: rect.height - 78 };
}

function timeToX(ts, area) {
  return area.left + ((ts - viewStart) / (viewEnd - viewStart)) * area.width;
}

function xToTime(x, area) {
  return viewStart + ((x - area.left) / area.width) * (viewEnd - viewStart);
}

function valueToY(value, min, max, area) {
  return area.bottom - ((value - min) / (max - min)) * area.height;
}

function visibleSamples() {
  return samples.filter((sample) => sample.ts >= viewStart && sample.ts <= viewEnd);
}

function visibleEvents() {
  return events.filter((event) => event.ts >= viewStart && event.ts <= viewEnd);
}

function draw() {
  fitCanvas();
  const rect = canvas.getBoundingClientRect();
  const area = chartRect();
  ctx.clearRect(0, 0, rect.width, rect.height);
  ctx.fillStyle = colors.bg;
  ctx.fillRect(0, 0, rect.width, rect.height);

  const shown = visibleSamples();
  if (!shown.length) {
    ctx.fillStyle = colors.muted;
    ctx.font = "16px sans-serif";
    ctx.fillText("Waiting for voltage/current readback history.", area.left, area.top + 28);
    updateEventList([]);
    return;
  }

  const [vMin, vMax] = padRange(Math.min(...shown.map((s) => s.voltage)), Math.max(...shown.map((s) => s.voltage)));
  const [iMin, iMax] = padRange(Math.min(...shown.map((s) => s.current)), Math.max(...shown.map((s) => s.current)));

  drawGrid(area, vMin, vMax, iMin, iMax);
  drawEvents(area);
  drawLine(shown, "voltage", vMin, vMax, area, colors.voltage);
  drawLine(shown, "current", iMin, iMax, area, colors.current);
  drawHover(shown, area, vMin, vMax, iMin, iMax);

  rangeEl.textContent = `${formatTime(viewStart)} to ${formatTime(viewEnd)}`;
  updateEventList(visibleEvents());
}

function drawGrid(area, vMin, vMax, iMin, iMax) {
  ctx.strokeStyle = colors.grid;
  ctx.fillStyle = colors.muted;
  ctx.lineWidth = 1;
  ctx.font = "12px sans-serif";
  for (let i = 0; i <= 5; i += 1) {
    const y = area.top + (area.height * i) / 5;
    ctx.beginPath();
    ctx.moveTo(area.left, y);
    ctx.lineTo(area.right, y);
    ctx.stroke();
    const voltage = vMax - ((vMax - vMin) * i) / 5;
    const current = iMax - ((iMax - iMin) * i) / 5;
    ctx.fillText(`${voltage.toFixed(2)} V`, 8, y + 4);
    ctx.fillText(`${current.toFixed(2)} A`, area.right + 8, y + 4);
  }
  for (let i = 0; i <= 6; i += 1) {
    const x = area.left + (area.width * i) / 6;
    ctx.beginPath();
    ctx.moveTo(x, area.top);
    ctx.lineTo(x, area.bottom);
    ctx.stroke();
    ctx.fillText(formatTime(xToTime(x, area)), x - 34, area.bottom + 28);
  }
  ctx.fillStyle = colors.voltage;
  ctx.fillText("Voltage", 8, area.top - 8);
  ctx.fillStyle = colors.current;
  ctx.fillText("Current", area.right + 8, area.top - 8);
}

function drawLine(points, field, min, max, area, color) {
  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  ctx.beginPath();
  points.forEach((point, index) => {
    const x = timeToX(point.ts, area);
    const y = valueToY(point[field], min, max, area);
    if (index === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();
}

function drawEvents(area) {
  ctx.save();
  visibleEvents().forEach((event) => {
    const x = timeToX(event.ts, area);
    ctx.strokeStyle = event.value ? colors.event : colors.grid;
    ctx.setLineDash(event.value ? [] : [4, 4]);
    ctx.beginPath();
    ctx.moveTo(x, area.top);
    ctx.lineTo(x, area.bottom);
    ctx.stroke();
    ctx.translate(x + 4, area.top + 8);
    ctx.rotate(-Math.PI / 2);
    ctx.fillStyle = event.value ? colors.event : colors.muted;
    ctx.font = "12px sans-serif";
    ctx.fillText(event.message, -150, 0);
    ctx.setTransform(window.devicePixelRatio || 1, 0, 0, window.devicePixelRatio || 1, 0, 0);
  });
  ctx.restore();
  ctx.setLineDash([]);
}

function drawHover(points, area, vMin, vMax, iMin, iMax) {
  if (hoverX === null || hoverX < area.left || hoverX > area.right) return;
  const ts = xToTime(hoverX, area);
  const nearest = points.reduce((best, point) => Math.abs(point.ts - ts) < Math.abs(best.ts - ts) ? point : best, points[0]);
  const x = timeToX(nearest.ts, area);
  ctx.strokeStyle = colors.text;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(x, area.top);
  ctx.lineTo(x, area.bottom);
  ctx.stroke();
  ctx.fillStyle = colors.text;
  ctx.beginPath();
  ctx.arc(x, valueToY(nearest.voltage, vMin, vMax, area), 4, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillStyle = colors.current;
  ctx.beginPath();
  ctx.arc(x, valueToY(nearest.current, iMin, iMax, area), 4, 0, Math.PI * 2);
  ctx.fill();
  readoutEl.innerHTML = `<strong>${new Date(nearest.ts).toLocaleString()}</strong><br>${nearest.voltage.toFixed(2)} V<br>${nearest.current.toFixed(2)} A<br>${nearest.power.toFixed(0)} W`;
}

function updateEventList(list) {
  eventsEl.innerHTML = list.length
    ? list.slice(-40).reverse().map((event) => `<div class="event"><strong>${escapeHtml(event.message)}</strong><br><small>${new Date(event.ts).toLocaleString()}</small></div>`).join("")
    : '<div class="event">No status transitions in the visible range.</div>';
}

function formatTime(ts) {
  return new Date(ts).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}

function zoom(factor, centerX = canvas.getBoundingClientRect().width / 2) {
  const area = chartRect();
  const center = xToTime(Math.max(area.left, Math.min(area.right, centerX)), area);
  const oldSpan = viewEnd - viewStart;
  const span = Math.max(60 * 1000, Math.min(24 * 3600 * 1000, oldSpan * factor));
  viewStart = center - ((center - viewStart) / oldSpan) * span;
  viewEnd = viewStart + span;
  clampView();
  draw();
}

function exportPng() {
  const link = document.createElement("a");
  link.download = "npb-history-24h.png";
  link.href = canvas.toDataURL("image/png");
  link.click();
}

function exportCsv() {
  window.location.href = "/api/history.csv";
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

canvas.addEventListener("wheel", (event) => {
  event.preventDefault();
  const rect = canvas.getBoundingClientRect();
  zoom(event.deltaY > 0 ? 1.18 : 0.82, event.clientX - rect.left);
});

canvas.addEventListener("mousedown", (event) => {
  dragging = true;
  dragStartX = event.clientX;
  dragStartView = [viewStart, viewEnd];
});

window.addEventListener("mouseup", () => {
  dragging = false;
});

window.addEventListener("mousemove", (event) => {
  const rect = canvas.getBoundingClientRect();
  hoverX = event.clientX - rect.left;
  if (dragging) {
    const area = chartRect();
    const delta = event.clientX - dragStartX;
    const shift = -(delta / area.width) * (dragStartView[1] - dragStartView[0]);
    viewStart = dragStartView[0] + shift;
    viewEnd = dragStartView[1] + shift;
    clampView();
  }
  draw();
});

document.querySelectorAll("[data-range]").forEach((button) => {
  button.addEventListener("click", () => setRangeHours(Number(button.dataset.range)));
});
document.getElementById("zoomIn").addEventListener("click", () => zoom(0.72));
document.getElementById("zoomOut").addEventListener("click", () => zoom(1.38));
document.getElementById("resetView").addEventListener("click", () => setRangeHours(24));
document.getElementById("exportPng").addEventListener("click", exportPng);
document.getElementById("exportCsv").addEventListener("click", exportCsv);
window.addEventListener("resize", draw);

loadHistory().catch(console.error);
setInterval(() => loadHistory().catch(console.error), 5000);

