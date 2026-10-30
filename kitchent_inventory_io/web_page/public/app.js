const firebaseConfig = {
  databaseURL: "https://kitchen-inventory-manage-92e86-default-rtdb.asia-southeast1.firebasedatabase.app/"
};

firebase.initializeApp(firebaseConfig);
const database = firebase.database();

const deviceSelect = document.getElementById("deviceSelect");
const refreshBtn = document.getElementById("refreshBtn");
const livePill = document.getElementById("livePill");
const liveWeightEl = document.getElementById("liveWeight");
const lastUpdatedEl = document.getElementById("lastUpdated");
const batteryValueEl = document.getElementById("batteryValue");
const batteryFillEl = document.getElementById("batteryFill");
const batteryStateEl = document.getElementById("batteryState");
const batteryVoltageTextEl = document.getElementById("batteryVoltageText");
const heartbeatValueEl = document.getElementById("heartbeatValue");
const historyCountEl = document.getElementById("historyCount");
const statusBadgeEl = document.getElementById("statusBadge");
const historyTableBody = document.getElementById("historyTableBody");
const historyChart = document.getElementById("historyChart");

const MAX_BATTERY_MV = 4200;
const MIN_BATTERY_MV = 3200;

let currentDeviceId = "";
let activeDeviceRef = null;
let telemetryRef = null;

// ================== UTILITIES ==================
function formatWeight(value) {
  const num = Number(value || 0);
  return `${num.toFixed(1)} g`;
}

function formatDateTime(unixSeconds) {
  if (!unixSeconds && unixSeconds !== 0) return "--";
  const date = new Date(Number(unixSeconds) * 1000);
  if (Number.isNaN(date.getTime())) return "--";

  return new Intl.DateTimeFormat("en-IN", {
    day: "2-digit",
    month: "short",
    year: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit"
  }).format(date);
}

function formatDurationSeconds(seconds) {
  if (!seconds) return "--";
  const mins = Math.floor(seconds / 60);
  const secs = seconds % 60;
  if (mins === 0) return `${secs}s`;
  return `${mins}m ${secs}s`;
}

function safeNumber(value, fallback = 0) {
  const num = Number(value);
  return Number.isFinite(num) ? num : fallback;
}

// ================== BATTERY UI ==================
function updateBatteryUI(voltage) {
  const voltageValue = safeNumber(voltage, 0);
  
  // Calculate percentage based on 3.2V (empty) to 4.2V (full)
  let percentage = ((voltageValue - MIN_BATTERY_MV) / (MAX_BATTERY_MV - MIN_BATTERY_MV)) * 100;
  percentage = Math.max(0, Math.min(100, percentage));
  const rounded = Math.round(percentage);

  batteryValueEl.textContent = `${rounded}%`;
  batteryFillEl.style.width = `${rounded}%`;
  batteryVoltageTextEl.textContent = `${Math.round(voltageValue)} mV / ${MAX_BATTERY_MV} mV`;

  if (rounded >= 50) {
    batteryStateEl.textContent = "Healthy";
    batteryStateEl.className = "status-badge";
    batteryFillEl.style.backgroundColor = "#1f8f69"; // Green
  } else if (rounded >= 20) {
    batteryStateEl.textContent = "Medium";
    batteryStateEl.className = "status-badge warning";
    batteryFillEl.style.backgroundColor = "#f59e0b"; // Orange
  } else {
    batteryStateEl.textContent = "Low";
    batteryStateEl.className = "status-badge danger";
    batteryFillEl.style.backgroundColor = "#ef4444"; // Red
  }
}

// ================== TABLE UI ==================
function renderHistoryTable(historyArray) {
  const items = [...historyArray].reverse().slice(0, 8);

  if (!items.length) {
    historyTableBody.innerHTML = '<tr><td colspan="2" class="empty-state">No history available</td></tr>';
    return;
  }

  historyTableBody.innerHTML = items.map((entry) => {
    return `
      <tr>
        <td>${formatDateTime(entry.ts)}</td>
        <td>${formatWeight(entry.value)}</td>
      </tr>
    `;
  }).join("");
}

// ================== CHART UI ==================
function renderHistoryChart(historyArray) {
  const canvas = historyChart;
  const ctx = canvas.getContext("2d");
  const rect = canvas.getBoundingClientRect();

  const pixelRatio = window.devicePixelRatio || 1;
  canvas.width = Math.max(rect.width * pixelRatio, 320);
  canvas.height = Math.max(rect.height * pixelRatio, 240);

  ctx.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0);
  ctx.clearRect(0, 0, rect.width, rect.height);

  if (!historyArray.length) {
    ctx.fillStyle = "#60707d";
    ctx.font = "16px sans-serif";
    ctx.textAlign = "center";
    ctx.fillText("No chart data", rect.width / 2, rect.height / 2);
    return;
  }

  const pad = { top: 20, right: 20, bottom: 30, left: 45 };
  const chartWidth = rect.width - pad.left - pad.right;
  const chartHeight = rect.height - pad.top - pad.bottom;

  const values = historyArray.map((item) => item.value);
  const minVal = Math.min(...values);
  const maxVal = Math.max(...values);
  
  // Add 10% padding to top and bottom of the chart data
  const valueRange = Math.max(maxVal - minVal, 1);
  const minY = Math.max(0, minVal - valueRange * 0.1); 
  const maxY = maxVal + valueRange * 0.1;

  // Draw Grid lines
  ctx.strokeStyle = "#dfeae5";
  ctx.lineWidth = 1;
  ctx.textAlign = "right";
  ctx.fillStyle = "#60707d";
  ctx.font = "12px sans-serif";

  for (let i = 0; i <= 4; i++) {
    const y = pad.top + (chartHeight / 4) * i;
    
    // Horizontal line
    ctx.beginPath();
    ctx.moveTo(pad.left, y);
    ctx.lineTo(rect.width - pad.right, y);
    ctx.stroke();

    // Y-Axis Label
    const labelVal = maxY - ((maxY - minY) / 4) * i;
    ctx.fillText(`${labelVal.toFixed(0)} g`, pad.left - 8, y + 4);
  }

  // Plot Points
  const points = historyArray.map((entry, index) => {
    const x = pad.left + (index / Math.max(historyArray.length - 1, 1)) * chartWidth;
    const y = pad.top + ((maxY - entry.value) / Math.max(maxY - minY, 1)) * chartHeight;
    return { x, y };
  });

  // Draw Line
  ctx.beginPath();
  ctx.moveTo(points[0].x, points[0].y);
  for (let i = 1; i < points.length; i++) {
    ctx.lineTo(points[i].x, points[i].y);
  }
  ctx.strokeStyle = "#1f8f69";
  ctx.lineWidth = 2.5;
  ctx.stroke();

  // Draw Dots
  points.forEach((point) => {
    ctx.beginPath();
    ctx.arc(point.x, point.y, 3, 0, Math.PI * 2);
    ctx.fillStyle = "#146d50";
    ctx.fill();
  });
}

// ================== DASHBOARD UPDATE ==================
function updateDashboard(data) {
  if (!data) {
    liveWeightEl.textContent = "-- g";
    lastUpdatedEl.textContent = "--";
    statusBadgeEl.textContent = "Offline";
    statusBadgeEl.className = "status-badge neutral";
    heartbeatValueEl.textContent = "--";
    historyCountEl.textContent = "0 points";
    historyTableBody.innerHTML = '<tr><td colspan="2" class="empty-state">No telemetry available</td></tr>';
    renderHistoryChart([]);
    return;
  }

  const currentWeight = safeNumber(data.currentWeight, 0);
  const batteryVoltage = safeNumber(data.batteryVoltage, 0);
  const lastUpdatedTs = safeNumber(data.lastUpdated, 0);
  const heartbeat = safeNumber(data.heartbeatSec, 0);

  liveWeightEl.textContent = formatWeight(currentWeight);
  lastUpdatedEl.textContent = formatDateTime(lastUpdatedTs);
  heartbeatValueEl.textContent = formatDurationSeconds(heartbeat);

  if (currentWeight > 0) {
    statusBadgeEl.textContent = "Stable";
    statusBadgeEl.className = "status-badge";
  } else {
    statusBadgeEl.textContent = "Empty";
    statusBadgeEl.className = "status-badge neutral";
  }

  updateBatteryUI(batteryVoltage);

  // Parse history
  const history = Object.keys(data.history || {}).map((timeKey) => ({
    ts: safeNumber(timeKey, 0),
    value: safeNumber((data.history || {})[timeKey], 0)
  })).filter((entry) => Number.isFinite(entry.ts) && entry.ts > 0);

  // Sort history by timestamp ascending for the chart
  history.sort((a, b) => a.ts - b.ts);

  historyCountEl.textContent = `${history.length} points`;
  renderHistoryTable(history);
  renderHistoryChart(history);
}

// ================== FIREBASE LISTENERS ==================
function attachDeviceListener(deviceId) {
  if (!deviceId) return;

  if (activeDeviceRef) {
    activeDeviceRef.off("value");
  }

  // Attach instant realtime listener
  activeDeviceRef = database.ref(`telemetry/${deviceId}`);
  activeDeviceRef.on("value", (snapshot) => {
    const data = snapshot.val();
    updateDashboard(data);
  });
}

function updateDeviceOptions(deviceIds) {
  const selectedValue = currentDeviceId || localStorage.getItem("deviceId") || "";

  deviceSelect.innerHTML = "";
  if (!deviceIds.length) {
    const emptyOption = document.createElement("option");
    emptyOption.value = "";
    emptyOption.textContent = "No devices found";
    deviceSelect.appendChild(emptyOption);
    currentDeviceId = "";
    return;
  }

  deviceIds.forEach((deviceId) => {
    const option = document.createElement("option");
    option.value = deviceId;
    option.textContent = deviceId;
    if (deviceId === selectedValue) {
      option.selected = true;
      currentDeviceId = deviceId;
    }
    deviceSelect.appendChild(option);
  });

  if (!currentDeviceId) {
    currentDeviceId = deviceIds[0];
    deviceSelect.value = currentDeviceId;
  }

  localStorage.setItem("deviceId", currentDeviceId);
  attachDeviceListener(currentDeviceId);
}

function loadAvailableDevices() {
  if (telemetryRef) {
    telemetryRef.off("value");
  }

  telemetryRef = database.ref("telemetry");
  telemetryRef.on("value", (snapshot) => {
    const value = snapshot.val() || {};
    const devices = Object.keys(value).sort();

    if (!devices.length) {
      updateDeviceOptions([]);
      return;
    }

    if (!currentDeviceId || !devices.includes(currentDeviceId)) {
      const savedValue = localStorage.getItem("deviceId");
      currentDeviceId = devices.includes(savedValue) ? savedValue : devices[0];
    }

    updateDeviceOptions(devices);
    deviceSelect.value = currentDeviceId;
  });
}

// ================== EVENTS ==================
deviceSelect.addEventListener("change", (event) => {
  currentDeviceId = event.target.value;
  if (currentDeviceId) {
    localStorage.setItem("deviceId", currentDeviceId);
    attachDeviceListener(currentDeviceId);
  }
});

refreshBtn.addEventListener("click", () => {
  if (currentDeviceId) {
    attachDeviceListener(currentDeviceId);
  } else {
    loadAvailableDevices();
  }
});

window.addEventListener("resize", () => {
  // Re-render chart on resize if we have an active listener
  if (currentDeviceId && activeDeviceRef) {
    activeDeviceRef.once("value").then((snapshot) => updateDashboard(snapshot.val()));
  }
});

// Start the app
loadAvailableDevices();i