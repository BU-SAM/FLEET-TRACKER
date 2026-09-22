# FLEET-TRACKER — Local Fleet Tracking Server v2.0

Self-hosted replacement for CircuitDigest GeoLinker cloud. Built for TTGO T-Call V1.4 (ESP32 + SIM800L) trackers but works with any device that can POST JSON over HTTP.

**v2.0 Features:**
- 🗺️ Live map (Leaflet + OpenStreetMap, no API key)
- 📡 GeoLinker compatible (`POST /api/v1/geolinker`) + simple `/api/track`
- 🚗 Multi-device support with different IDs
- 📜 **10,000 points limit per tracker (FIFO)** — oldest auto-deleted, like GeoLinker
- 🧹 **Smart optimization** — removes useless points:
  - Jitter <5m (GPS noise)
  - Stationary duplicates (keep 1 per 5 min when parked)
  - Redundant close points <10m
  - Collinear points on straight roads
- ⚡ Real-time updates, fill % bar, optimization UI
- 🌐 Works over plain HTTP (required for SIM800L)

---

## Quick Start

```bash
cd backend
pip install -r requirements.txt
python app.py
# http://localhost:8000
```

### Test without hardware
```bash
pip install requests
python simulate_device.py --device car_tracker_01 --loop
```

---

## Optimization Logic (New in v2.0)

**1. On Insert (prevents useless points):**
- If distance <5m and time <30s → skip (GPS jitter)
- If speed <2 km/h and distance <15m and time <5 min → skip (stationary)
- If distance <10m and time <60s and speed change <5 km/h → skip

**2. FIFO Limit:**
- `MAX_POINTS_PER_DEVICE=10000` (env var)
- When over limit, oldest points auto-deleted
- 50 trackers × 10k = max 500k points (~50MB), not 8M

**3. Manual Optimization (API or UI):**
- `POST /api/devices/{id}/optimize` — removes:
  - Jitter, stationary clusters, straight-line redundant points
  - Keeps first/last, turns, speed changes
- `POST /api/optimize-all` — all devices
- `GET /api/devices/{id}/optimization-preview` — preview savings

**Frontend:** Select device → `🧹 Optimize` button shows preview + savings %

**Env vars to tune:**
```bash
MAX_POINTS_PER_DEVICE=10000
MIN_DISTANCE_M=10
STATIONARY_DISTANCE_M=15
STATIONARY_SPEED_KMH=2.0
STATIONARY_TIME_S=300
COLLINEAR_TOLERANCE_M=8
```

---

## API

### GeoLinker Compatible
```
POST /api/v1/geolinker
{
  "device_id": "car_tracker_01",
  "timestamp": ["2025-09-21 14:30:00"],
  "lat": [6.5244],
  "long": [3.3792],
  "payload": [{"speed": 45.5, "sats": 8}]
}
Response:
{
  "status": "stored",
  "points_stored": 1,
  "points_skipped": 0,
  "current_total": 1234,
  "max_points": 10000
}
```

### Other
- `GET /api/devices` — list with fill % and points_remaining
- `GET /api/devices/{id}/history?limit=1000&order=asc`
- `POST /api/devices/{id}/optimize?aggressive=false`
- `GET /api/config` — current thresholds
- `GET /api/stats`

---

## Production for 50 Trackers

With 10k limit + optimization, **any $5-7 server is enough**:

| Service | Specs | Price | Notes |
|---------|-------|-------|-------|
| **Hetzner CX22 (Recommended)** | 2 vCPU, 4GB, 40GB | **$6.50/mo** | Frankfurt → best for NG, 500 trackers OK |
| DigitalOcean | 2GB RAM | $12/mo | Easy |
| Render | Web + Postgres | $14/mo | Zero DevOps |
| Oracle Free | 4 vCPU, 24GB | **$0/mo** | Free forever |

**Why so cheap now?**
- Before: 50 trackers × 15s = 288k points/day → 8.6M/month → 10GB/year
- Now: Max 500k points total (10k × 50) → ~50MB total, forever. SQLite is fine.

**Deploy:**
```bash
git clone https://github.com/BU-SAM/FLEET-TRACKER.git
cd FLEET-TRACKER
git checkout arena/01a0c3f3-fleet-tracker
cd backend
pip install -r requirements.txt
python app.py
```

For permanent: Use Docker + Nginx (HTTP:80 for SIM800L, HTTPS:443 for dashboard)

---

## Project Structure
```
backend/
  app.py - FastAPI + FIFO + optimization
  database.py - Smart insert + haversine + collinear check
  fleet.db - SQLite (auto-created, max 500k points for 50 cars)
frontend/
  index.html / app.js / style.css - Map with fill bar + optimize button
firmware/
  fleet_tracker_local.ino - Local server
  fleet_tracker_tunnel.ino - For CGNAT (localtunnel/ngrok)
  fleet_tracker_wifi_test.ino - WiFi quick test
TUNNEL_GUIDE.md - How to test cellular without port forward
```

## Firmware Setup
See `firmware/README.md` and `TUNNEL_GUIDE.md`
