# Production Deploy - No Need to Keep Your PC Online

Yes! Once deployed to a hosting VPS, **you don't need your computer online**. The server runs 24/7 in Hetzner/DigitalOcean data center. Trackers connect directly to it via public IP/domain.

```
[50 Cars with SIM800L] --2G--> [Hetzner VPS Frankfurt:80] --Internet--> [Your Phone/PC Dashboard anywhere]
     No PC needed ↑
```

---

## Option 1: Hetzner / DigitalOcean VPS (Recommended, $4.50/mo)

### 1. Create VPS
- Hetzner: https://hetzner.com → CX11 (€4.15/mo) → Ubuntu 22.04 → Frankfurt region
- DigitalOcean: $6 droplet → Frankfurt
- You get public IP like `65.21.12.34`

### 2. SSH into server
```bash
ssh root@65.21.12.34
```

### 3. Install Docker
```bash
apt update && apt install -y docker.io docker-compose git
```

### 4. Clone and Run
```bash
git clone https://github.com/BU-SAM/FLEET-TRACKER.git
cd FLEET-TRACKER
git checkout arena/01a0c3f3-fleet-tracker
mkdir -p data

# Simple run (HTTP only, works for both trackers and dashboard)
docker-compose up -d --build

# Check logs
docker-compose logs -f

# Server now at http://65.21.12.34:8000 and http://65.21.12.34
```

### 5. Update Trackers Firmware
```cpp
static const char* LOCAL_SERVER_HOST = "65.21.12.34"; // your VPS IP
static const int   LOCAL_SERVER_PORT = 80; // Nginx forwards 80 -> 8000
static const char* LOCAL_API_PATH    = "/api/v1/geolinker";
```

Flash all 50 trackers with different DEVICE_IDs. Done! They will send to VPS forever, no PC needed.

### 6. (Optional) Add Domain + HTTPS for Dashboard
```bash
# Point fleet.yourdomain.com A record to 65.21.12.34
# Then:
apt install certbot
certbot certonly --webroot -w ./data/certbot/www -d fleet.yourdomain.com
# Uncomment HTTPS block in nginx.conf and update server_name
docker-compose --profile prod up -d
```
Now:
- Trackers still use `http://fleet.yourdomain.com:80/api/v1/geolinker` (plain HTTP for SIM800L)
- Dashboard uses `https://fleet.yourdomain.com` (secure)

---

## Option 2: Render.com (Easiest, No Linux knowledge, $7/mo)

1. Go to render.com → New Web Service → Connect your GitHub repo `BU-SAM/FLEET-TRACKER`
2. Build: `pip install -r backend/requirements.txt`
3. Start: `python backend/app.py`
4. Env: `PORT=10000`, `MAX_POINTS_PER_DEVICE=10000`
5. Deploy → you get URL `https://fleet-tracker-xxx.onrender.com`
6. Trackers: Use `HOST = "fleet-tracker-xxx.onrender.com"` `PORT = 80` (Render auto handles HTTP)

No server management, auto restarts, logs in dashboard.

---

## Option 3: Oracle Free Tier ($0/mo Forever)

1. Create Oracle Cloud account
2. Create Ampere A1 VM (4 OCPU, 24GB RAM free)
3. Same steps as Hetzner VPS above

---

## How Much Will 50 Trackers Cost?

| Item | Cost |
|------|------|
| Hetzner CX11 VPS | $4.50/mo |
| Domain (optional) | $10/year |
| SIM data (50 × 100MB) | ~₦5,000/mo (your carrier) |
| **Total server cost** | **~$5/mo, PC can be off forever** |

With 10k FIFO limit, DB stays at ~50MB max, so CX11 never fills up.

---

## Managing Without PC

- Dashboard: Open `http://65.21.12.34` or `https://fleet.yourdomain.com` from any phone/PC anywhere
- Logs: `ssh root@IP docker-compose logs -f`
- Backup DB: `scp root@IP:/root/FLEET-TRACKER/backend/fleet.db ./backup.db`
- Updates: `ssh root@IP "cd FLEET-TRACKER && git pull && docker-compose up -d --build"`

Your PC is only needed to flash trackers once. After that, everything runs in the cloud.
