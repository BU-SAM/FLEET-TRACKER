# Testing Real Hardware with CGNAT (No Port Forward)

Your router has CGNAT, so your local IP `192.168.x.x` is not reachable from the internet. Since your TTGO uses **cellular (2G)**, it needs a public URL. Here are 3 quick ways - no router config needed.

## Option 1: LocalTunnel (Fastest, No Account) - RECOMMENDED FOR QUICK TEST

Requires Node.js installed.

**On your PC (where server runs):**
```bash
# 1. Start your fleet server
cd backend
pip install -r requirements.txt
python app.py
# Server at http://localhost:8000

# 2. In another terminal, expose it:
npx localtunnel --port 8000
# You'll get URL like: https://loud-paws-shout.loca.lt
# It also gives http version - copy the https URL but we'll use http
```

**For SIM800L, use HTTP (it can't do HTTPS):**
LocalTunnel supports both. Your URL will be something like `loud-paws-shout.loca.lt`

In `firmware/fleet_tracker_local.ino` change:
```cpp
static const char* LOCAL_SERVER_HOST = "loud-paws-shout.loca.lt"; // your tunnel URL without https://
static const int   LOCAL_SERVER_PORT = 80; // 80 for http
static const char* LOCAL_API_PATH    = "/api/v1/geolinker";
```

Flash and power the tracker. Data will appear at your local http://localhost:8000 map!

> Note: LocalTunnel URL changes each time you restart. For permanent URL, use Option 2 or 3.

---

## Option 2: Ngrok (More Stable, Free Account Needed)

1. Sign up at https://ngrok.com, install:
```bash
# Download ngrok, then:
ngrok config add-authtoken YOUR_TOKEN
ngrok http 8000
```

2. You'll get URL like `https://a1b2c3d4.ngrok-free.app`

3. **Critical for SIM800L:** Ngrok free shows browser warning page. You must bypass it by adding header. I already added it in the tunnel firmware.

Use this firmware config:
```cpp
static const char* LOCAL_SERVER_HOST = "a1b2c3d4.ngrok-free.app";
static const int   LOCAL_SERVER_PORT = 80;
```

And in the HTTP request, we send:
```
ngrok-skip-browser-warning: true
```

See `firmware/fleet_tracker_tunnel.ino` - it has this header already.

---

## Option 3: Cloudflare Tunnel (Best for Permanent, Free)

Most reliable, no random URLs, no warnings.

```bash
# Install cloudflared: https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/downloads/

# Login
cloudflared tunnel login

# Create tunnel
cloudflared tunnel create fleet-tracker

# Run tunnel
cloudflared tunnel --url http://localhost:8000 run fleet-tracker
```

You'll get a permanent `https://fleet-tracker-xxx.trycloudflare.com` URL.

---

## Option 4: Quick WiFi Test (No Tunnel, No Cellular)

If you want to verify server + map works in 2 minutes **before** testing cellular:

Use `firmware/fleet_tracker_wifi_test.ino` - it uses ESP32 WiFi instead of SIM800L, connects to your home WiFi and posts to your local IP `192.168.x.x:8000`.

This proves everything works without dealing with tunnels.

Steps:
1. Edit WiFi SSID/PASS and `LOCAL_SERVER_HOST = "192.168.1.50"` (your PC IP)
2. Flash
3. Open Serial Monitor - you'll see it posting
4. Check map at http://localhost:8000

Then switch to cellular + tunnel for real field test.

---

## How to Check if Data Arrives

On PC running server, watch logs:
```
INFO: 1.2.3.4 - "POST /api/v1/geolinker HTTP/1.1" 201 Created
```

Or:
```bash
curl http://localhost:8000/api/devices
curl http://localhost:8000/api/devices/car_tracker_01/history?limit=5
```

## Troubleshooting Cellular

- **No network registration:** Check SIM antenna, 2G coverage, APN. Airtel Nigeria: `internet.ng.airtel.com`
- **GPRS up but HTTP 0:** Tunnel URL wrong, or tunnel not running, or using HTTPS port 443 (SIM800L can't). Use port 80.
- **HTTP 404:** Check `LOCAL_API_PATH` is `/api/v1/geolinker`
- **HTTP 400:** Coordinates 0,0 filtered - need real GPS fix (go outside)
- **Slow cold start:** GPS needs clear sky, 30-90 sec first fix

## Running Server on Another Device

On new device:
```bash
git clone https://github.com/BU-SAM/FLEET-TRACKER.git
cd FLEET-TRACKER
git checkout arena/01a0c3f3-fleet-tracker
cd backend
pip install -r requirements.txt --break-system-packages
python app.py
```

Then open http://localhost:8000
