/**
 * FLEET-TRACKER Frontend v2.0 - With 10k FIFO + Optimization
 */

const API_BASE = window.location.origin;
let map, markers = {}, polylines = {}, playbackMarker = null;
let devices = [];
let selectedDeviceId = null;
let currentHistory = [];
let followMode = false;
let showTrail = true;
let playbackInterval = null;
let playbackIndex = 0;
let maxPointsPerDevice = 10000;

const layers = {
  osm: L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
    attribution: '&copy; OpenStreetMap contributors',
    maxZoom: 19
  }),
  satellite: L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
    attribution: 'Esri',
    maxZoom: 19
  }),
  dark: L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', {
    attribution: '&copy; OpenStreetMap & CARTO',
    maxZoom: 19
  })
};

function initMap() {
  map = L.map('map', {
    center: [6.5244, 3.3792],
    zoom: 12,
    zoomControl: false
  });
  layers.osm.addTo(map);
  L.control.zoom({ position: 'bottomleft' }).addTo(map);
  map.on('mousemove', (e) => {
    document.getElementById('coords-display').textContent =
      `${e.latlng.lat.toFixed(5)}, ${e.latlng.lng.toFixed(5)}`;
  });
  window.carIcon = L.divIcon({
    html: '<div style="background:#3b82f6;color:white;width:32px;height:32px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:18px;border:2px solid white;box-shadow:0 2px 8px rgba(0,0,0,0.3)">🚗</div>',
    className: 'custom-car-icon',
    iconSize: [32, 32],
    iconAnchor: [16, 16]
  });
  window.carIconOnline = L.divIcon({
    html: '<div style="background:#10b981;color:white;width:32px;height:32px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:18px;border:2px solid white;box-shadow:0 0 0 4px rgba(16,185,129,0.3),0 2px 8px rgba(0,0,0,0.3)">🚗</div>',
    className: 'custom-car-icon',
    iconSize: [32, 32],
    iconAnchor: [16, 16]
  });
}

async function fetchConfig() {
  try {
    const res = await fetch(`${API_BASE}/api/config`);
    const cfg = await res.json();
    maxPointsPerDevice = cfg.max_points_per_device || 10000;
  } catch {}
}

async function fetchDevices() {
  try {
    const res = await fetch(`${API_BASE}/api/devices`);
    if (!res.ok) throw new Error('Failed');
    const data = await res.json();
    devices = data;
    renderDeviceList();
    updateStats();
    updateMarkers();
    setBackendStatus(true);
  } catch (e) {
    console.error('Fetch devices failed', e);
    setBackendStatus(false);
  }
}

async function fetchStats() {
  try {
    const res = await fetch(`${API_BASE}/api/stats`);
    const data = await res.json();
    document.getElementById('stat-devices').textContent = data.total_devices;
    document.getElementById('stat-points').textContent = data.total_points;
    // Show max in subtitle if needed
  } catch {}
}

function setBackendStatus(ok) {
  const dot = document.getElementById('backend-status');
  const text = document.getElementById('backend-text');
  if (ok) {
    dot.className = 'status-dot ok';
    text.textContent = `Connected (max ${maxPointsPerDevice}/device)`;
  } else {
    dot.className = 'status-dot error';
    text.textContent = 'Disconnected';
  }
}

function updateStats() {
  document.getElementById('stat-devices').textContent = devices.length;
  document.getElementById('device-count').textContent = `(${devices.length})`;
  const online = devices.filter(d => d.is_online).length;
  document.getElementById('stat-online').textContent = online;
}

function renderDeviceList() {
  const list = document.getElementById('device-list');
  if (devices.length === 0) {
    list.innerHTML = '<div class="empty">No devices yet<br><small>Send data from tracker or run<br><code>python simulate_device.py --loop</code></small></div>';
    return;
  }
  list.innerHTML = devices.map(d => {
    const ago = d.seconds_ago != null ? formatAgo(d.seconds_ago) : 'unknown';
    const coords = d.last_lat && d.last_lon ? `${d.last_lat.toFixed(5)}, ${d.last_lon.toFixed(5)}` : 'No fix';
    const fill = d.fill_percent || 0;
    const fillColor = fill > 90 ? '#ef4444' : fill > 70 ? '#f59e0b' : '#3b82f6';
    return `
      <div class="device-item ${selectedDeviceId === d.device_id ? 'active' : ''}" data-id="${d.device_id}">
        <div class="device-item-header">
          <span class="device-id">${escapeHtml(d.device_id)}</span>
          <span class="device-status ${d.is_online ? 'online' : 'offline'}">${d.is_online ? 'online' : 'offline'}</span>
        </div>
        <div class="device-meta">
          <span>📍 ${coords}</span>
          <span>🕒 ${ago} • ${d.total_points}/${maxPointsPerDevice} (${fill}%)</span>
          <div style="height:4px; background:#1e293b; border-radius:2px; margin-top:4px; overflow:hidden">
            <div style="height:100%; width:${Math.min(fill,100)}%; background:${fillColor}; transition:width 0.3s"></div>
          </div>
        </div>
      </div>
    `;
  }).join('');
  list.querySelectorAll('.device-item').forEach(el => {
    el.addEventListener('click', () => selectDevice(el.dataset.id));
  });
}

function formatAgo(seconds) {
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds/60)}m ago`;
  if (seconds < 86400) return `${Math.floor(seconds/3600)}h ago`;
  return `${Math.floor(seconds/86400)}d ago`;
}

function escapeHtml(s) {
  const div = document.createElement('div');
  div.textContent = s;
  return div.innerHTML;
}

function updateMarkers() {
  devices.forEach(d => {
    if (!d.last_lat || !d.last_lon) return;
    const latlng = [d.last_lat, d.last_lon];
    if (markers[d.device_id]) {
      markers[d.device_id].setLatLng(latlng);
      markers[d.device_id].setIcon(d.is_online ? window.carIconOnline : window.carIcon);
    } else {
      const marker = L.marker(latlng, {
        icon: d.is_online ? window.carIconOnline : window.carIcon,
        title: d.device_id
      }).addTo(map);
      marker.bindPopup(`
        <b>${escapeHtml(d.device_id)}</b><br>
        ${d.last_lat.toFixed(6)}, ${d.last_lon.toFixed(6)}<br>
        Last: ${d.last_seen}<br>
        Points: ${d.total_points}/${maxPointsPerDevice} (${d.fill_percent||0}%)<br>
        <small>${d.is_online ? '🟢 Online' : '⚪ Offline'}</small>
      `);
      marker.on('click', () => selectDevice(d.device_id));
      markers[d.device_id] = marker;
    }
  });
  Object.keys(markers).forEach(id => {
    if (!devices.find(d => d.device_id === id)) {
      map.removeLayer(markers[id]);
      delete markers[id];
      if (polylines[id]) {
        map.removeLayer(polylines[id]);
        delete polylines[id];
      }
    }
  });
}

async function selectDevice(deviceId) {
  selectedDeviceId = deviceId;
  renderDeviceList();
  const device = devices.find(d => d.device_id === deviceId);
  if (!device) return;
  const panel = document.getElementById('selected-device-panel');
  panel.style.display = 'block';
  document.getElementById('selected-device-id').textContent = deviceId;
  document.getElementById('detail-last-seen').textContent = device.last_seen || '-';
  document.getElementById('detail-coords').textContent = device.last_lat ? `${device.last_lat.toFixed(6)}, ${device.last_lon.toFixed(6)}` : '-';
  document.getElementById('detail-total').textContent = `${device.total_points} / ${maxPointsPerDevice}`;
  document.getElementById('detail-fill').textContent = `${device.fill_percent || 0}% full`;
  document.getElementById('detail-status').textContent = device.is_online ? `🟢 Online (${formatAgo(device.seconds_ago)})` : '⚪ Offline';
  document.getElementById('detail-optimized').textContent = device.optimized_points ? `${device.optimized_points} removed` : '0';

  // Fill bar
  const fillBar = document.getElementById('fill-bar');
  const fillInner = document.getElementById('fill-bar-inner');
  if (fillBar && fillInner) {
    fillBar.style.display = 'block';
    const pct = device.fill_percent || 0;
    fillInner.style.width = `${Math.min(pct,100)}%`;
    fillInner.style.background = pct > 90 ? '#ef4444' : pct > 70 ? '#f59e0b' : '#3b82f6';
  }

  const limit = document.getElementById('history-limit').value;
  try {
    const res = await fetch(`${API_BASE}/api/devices/${encodeURIComponent(deviceId)}/history?limit=${limit}&order=asc`);
    const history = await res.json();
    currentHistory = history;
    if (history.length > 0) {
      const last = history[history.length - 1];
      document.getElementById('detail-speed').textContent = last.speed != null ? `${last.speed} km/h` : '-';
      document.getElementById('detail-sats').textContent = last.sats != null ? last.sats : '-';
    }
    renderHistoryList(history);
    drawTrail(deviceId, history);
    if (followMode && history.length > 0) {
      const last = history[history.length - 1];
      map.setView([last.lat, last.lon], map.getZoom());
    }
  } catch (e) {
    console.error('History fetch failed', e);
  }
}

function renderHistoryList(history) {
  const container = document.getElementById('history-list');
  if (history.length === 0) {
    container.innerHTML = '<small style="color:var(--text-muted)">No history</small>';
    return;
  }
  const recent = history.slice(-20).reverse();
  container.innerHTML = recent.map(p => {
    const time = p.timestamp ? p.timestamp.split(' ')[1] || p.timestamp : '';
    return `<div class="history-entry"><span>${p.lat.toFixed(5)}, ${p.lon.toFixed(5)}</span><small>${time} ${p.speed ? p.speed+'km/h' : ''}</small></div>`;
  }).join('');
}

function drawTrail(deviceId, history) {
  if (polylines[deviceId]) {
    map.removeLayer(polylines[deviceId]);
  }
  if (!showTrail || history.length < 2) return;
  const latlngs = history.map(p => [p.lat, p.lon]);
  const poly = L.polyline(latlngs, {
    color: selectedDeviceId === deviceId ? '#3b82f6' : '#64748b',
    weight: selectedDeviceId === deviceId ? 4 : 2,
    opacity: 0.8,
    dashArray: selectedDeviceId === deviceId ? null : '5,5'
  }).addTo(map);
  polylines[deviceId] = poly;
}

function openPlayback() {
  if (!selectedDeviceId || currentHistory.length === 0) return;
  document.getElementById('playback-modal').style.display = 'flex';
  document.getElementById('playback-device').textContent = selectedDeviceId;
  document.getElementById('pb-slider').max = currentHistory.length - 1;
  document.getElementById('pb-slider').value = 0;
  playbackIndex = 0;
  if (playbackMarker) map.removeLayer(playbackMarker);
  playbackMarker = L.marker([currentHistory[0].lat, currentHistory[0].lon], {
    icon: L.divIcon({
      html: '<div style="background:#f59e0b;color:white;width:36px;height:36px;border-radius:50%;display:flex;align-items:center;justify-content:center;border:2px solid white">▶</div>',
      iconSize: [36,36],
      iconAnchor: [18,18]
    })
  }).addTo(map);
  map.setView([currentHistory[0].lat, currentHistory[0].lon], 15);
}

function closePlayback() {
  document.getElementById('playback-modal').style.display = 'none';
  if (playbackInterval) clearInterval(playbackInterval);
  playbackInterval = null;
  if (playbackMarker) {
    map.removeLayer(playbackMarker);
    playbackMarker = null;
  }
  document.getElementById('pb-play').textContent = '▶ Play';
}

function togglePlayback() {
  const btn = document.getElementById('pb-play');
  if (playbackInterval) {
    clearInterval(playbackInterval);
    playbackInterval = null;
    btn.textContent = '▶ Play';
  } else {
    btn.textContent = '⏸ Pause';
    const speedSelect = document.getElementById('pb-speed');
    const intervalMs = 1000 / parseInt(speedSelect.value);
    playbackInterval = setInterval(() => {
      playbackIndex++;
      if (playbackIndex >= currentHistory.length) playbackIndex = 0;
      const point = currentHistory[playbackIndex];
      if (playbackMarker) playbackMarker.setLatLng([point.lat, point.lon]);
      document.getElementById('pb-slider').value = playbackIndex;
      document.getElementById('pb-time').textContent = point.timestamp;
      if (followMode) map.setView([point.lat, point.lon]);
    }, intervalMs);
  }
}

async function optimizeDevice(aggressive=false) {
  if (!selectedDeviceId) { alert('Select a device first'); return; }
  const btn = document.getElementById('btn-optimize');
  const info = document.getElementById('optimization-info');
  btn.textContent = '⏳ Optimizing...';
  btn.disabled = true;
  try {
    // First preview
    const previewRes = await fetch(`${API_BASE}/api/devices/${encodeURIComponent(selectedDeviceId)}/optimization-preview?aggressive=${aggressive}`);
    const preview = await previewRes.json();
    info.style.display = 'block';
    info.textContent = `Preview: would delete ${preview.would_delete} / ${preview.original} (${preview.savings_percent}% savings)`;

    if (preview.would_delete === 0) {
      info.textContent = 'No useless points found — already optimized!';
      btn.textContent = '🧹 Optimize';
      btn.disabled = false;
      return;
    }
    if (!confirm(`Optimize ${selectedDeviceId}?\nWill delete ${preview.would_delete} useless points (jitter, stationary duplicates, straight-line):\n${preview.original} → ${preview.would_keep}\nSavings: ${preview.savings_percent}%`)) {
      btn.textContent = '🧹 Optimize';
      btn.disabled = false;
      return;
    }
    const res = await fetch(`${API_BASE}/api/devices/${encodeURIComponent(selectedDeviceId)}/optimize?aggressive=${aggressive}`, { method: 'POST' });
    const data = await res.json();
    info.textContent = `✅ Deleted ${data.optimization.deleted} points. ${data.optimization.original} → ${data.optimization.optimized}. Now ${data.current_total}/${maxPointsPerDevice}`;
    fetchDevices();
    if (selectedDeviceId) selectDevice(selectedDeviceId);
  } catch (e) {
    console.error(e);
    info.textContent = 'Optimization failed: ' + e.message;
  }
  btn.textContent = '🧹 Optimize';
  btn.disabled = false;
}

async function optimizeAll() {
  if (!confirm(`Optimize ALL devices? This will remove jitter, stationary duplicates, and straight-line redundant points from all trackers.`)) return;
  const btn = document.getElementById('btn-optimize-all');
  const info = document.getElementById('optimization-info');
  btn.textContent = '⏳ Optimizing all...';
  btn.disabled = true;
  try {
    const res = await fetch(`${API_BASE}/api/optimize-all`, { method: 'POST' });
    const data = await res.json();
    info.style.display = 'block';
    info.textContent = `✅ Optimized ${data.devices_optimized} devices, deleted ${data.total_deleted} useless points total`;
    fetchDevices();
  } catch (e) {
    info.textContent = 'Failed: ' + e.message;
  }
  btn.textContent = '🧹 All';
  btn.disabled = false;
}

document.addEventListener('DOMContentLoaded', () => {
  initMap();
  fetchConfig().then(() => {
    fetchDevices();
    fetchStats();
  });
  document.getElementById('api-url').textContent = API_BASE;
  setInterval(fetchDevices, 3000);
  setInterval(fetchStats, 10000);

  document.getElementById('btn-refresh').addEventListener('click', fetchDevices);
  document.getElementById('btn-center-all').addEventListener('click', () => {
    if (devices.length === 0) return;
    const group = new L.featureGroup(Object.values(markers));
    if (Object.keys(markers).length > 0) map.fitBounds(group.getBounds().pad(0.2));
  });
  document.getElementById('btn-clear').addEventListener('click', async () => {
    if (!selectedDeviceId) { alert('Select a device first'); return; }
    if (!confirm(`Clear all history for ${selectedDeviceId}?`)) return;
    await fetch(`${API_BASE}/api/devices/${encodeURIComponent(selectedDeviceId)}/history`, { method: 'DELETE' });
    fetchDevices();
    currentHistory = [];
    renderHistoryList([]);
    if (polylines[selectedDeviceId]) {
      map.removeLayer(polylines[selectedDeviceId]);
      delete polylines[selectedDeviceId];
    }
  });
  document.getElementById('btn-optimize').addEventListener('click', () => optimizeDevice(false));
  document.getElementById('btn-optimize-all').addEventListener('click', optimizeAll);
  document.getElementById('btn-focus').addEventListener('click', () => {
    if (!selectedDeviceId) return;
    const m = markers[selectedDeviceId];
    if (m) map.setView(m.getLatLng(), 16);
  });
  document.getElementById('btn-playback').addEventListener('click', openPlayback);
  document.getElementById('pb-play').addEventListener('click', togglePlayback);
  document.getElementById('pb-slider').addEventListener('input', (e) => {
    playbackIndex = parseInt(e.target.value);
    if (currentHistory[playbackIndex] && playbackMarker) {
      const p = currentHistory[playbackIndex];
      playbackMarker.setLatLng([p.lat, p.lon]);
      document.getElementById('pb-time').textContent = p.timestamp;
      map.setView([p.lat, p.lon]);
    }
  });
  document.getElementById('history-limit').addEventListener('change', () => {
    if (selectedDeviceId) selectDevice(selectedDeviceId);
  });
  document.getElementById('toggle-trail').addEventListener('change', (e) => {
    showTrail = e.target.checked;
    if (selectedDeviceId) drawTrail(selectedDeviceId, currentHistory);
    else {
      Object.keys(polylines).forEach(id => map.removeLayer(polylines[id]));
      polylines = {};
    }
  });
  document.getElementById('toggle-follow').addEventListener('change', (e) => {
    followMode = e.target.checked;
  });
  document.getElementById('btn-layers').addEventListener('click', () => {
    const panel = document.getElementById('layers-panel');
    panel.style.display = panel.style.display === 'none' ? 'block' : 'none';
  });
  document.querySelectorAll('#layers-panel button').forEach(btn => {
    btn.addEventListener('click', () => {
      Object.values(layers).forEach(l => map.removeLayer(l));
      layers[btn.dataset.layer].addTo(map);
      document.getElementById('layers-panel').style.display = 'none';
    });
  });
  document.addEventListener('click', (e) => {
    if (!e.target.closest('#btn-layers') && !e.target.closest('#layers-panel')) {
      document.getElementById('layers-panel').style.display = 'none';
    }
  });
});

window.closePlayback = closePlayback;
