# Firmware

## Files
- `fleet_tracker_local.ino` — **USE THIS** for local server. Points to your PC's IP.
- `fleet_tracker_geolinker.ino` — Original CircuitDigest cloud version (reference)

## How to switch from GeoLinker to Local Server

### 1. Find your PC's Local IP
- **Windows:** Open CMD → `ipconfig` → look for IPv4 Address, e.g. `192.168.1.50`
- **Linux/Mac:** `ifconfig` or `ip a` → `inet 192.168.x.x`

Your tracker and PC must be on same WiFi network? **No** — tracker uses 2G cellular, so it goes via internet. For local testing:
- Option A: Port forward router port 8000 to your PC (then use your public IP)
- Option B: Use ngrok / localtunnel to expose local server: `ngrok http 8000`
- Option C: For testing on same network, the SIM800L's carrier NAT can still reach your local IP if your router has public IP? Usually need Option A/B. Simplest for development: use `simulate_device.py` to test, then deploy backend to VPS.

**For pure local network without internet (if you have WiFi version of tracker):** Use local IP directly.

### 2. Configure firmware
Edit in `fleet_tracker_local.ino`:
```cpp
static const char* LOCAL_SERVER_HOST = "192.168.1.50"; // YOUR PC IP or ngrok host
static const int   LOCAL_SERVER_PORT = 8000;           // 8000 for local, 80 for ngrok http
static const char* LOCAL_API_PATH    = "/api/v1/geolinker";
static const char* DEVICE_ID = "car_tracker_01"; // Unique per car!
```

### 3. Per-car IDs
Flash each device with different DEVICE_ID:
- Car 1: `car_tracker_01`
- Car 2: `car_tracker_02`
- Car 3: `car_tracker_03`
- etc.

Server will automatically create device entries and log history per ID.

### 4. APN Settings
Set according to your SIM:
```cpp
// MTN:     "web.gprs.mtnnigeria.net"
// Airtel:  "internet.ng.airtel.com"
// Glo:     "gloflat" user "flat" pass "flat"
// 9mobile: "9mobile"
```

### 5. Upload
- Board: "ESP32 WROVER Module"
- Baud: 115200
- Library: TinyGPSPlus

## Testing without hardware
```bash
cd backend
pip install requests
python simulate_device.py --device car_tracker_01 --loop --interval 5
python simulate_device.py --device car_tracker_02 --loop --interval 7
```
Then open http://localhost:8000 to see both cars moving.

## Payload Format
Local server accepts both:

**GeoLinker compatible (what current firmware sends):**
```json
{
  "device_id": "car_tracker_01",
  "timestamp": ["2025-09-21 14:30:00"],
  "lat": [6.5244],
  "long": [3.3792],
  "payload": [{"speed": 45.5, "sats": 8}]
}
```

**Simple (future):**
```json
{
  "device_id": "car_tracker_01",
  "lat": 6.5244,
  "lon": 3.3792,
  "timestamp": "2025-09-21 14:30:00",
  "speed": 45.5,
  "sats": 8
}
```
