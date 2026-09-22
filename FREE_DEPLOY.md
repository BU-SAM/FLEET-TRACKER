# FREE Production Deploy - Oracle Free Tier + DuckDNS ($0/month)

Run your 50 trackers 24/7 for free, no PC needed. When ready to scale, switch to Hetzner + .com domain (same code).

```
[50 Cars] --2G--> [Oracle Free VM Frankfurt] --Internet--> [fleet.duckdns.org map, any phone]
     $0/month, PC off forever
```

**Total cost: $0/month for server + $0 for custom name**

---

## Part 1: Oracle Free Tier VM (4 vCPU, 24GB RAM, Free Forever)

### 1. Create Oracle Account
- Go to https://www.oracle.com/cloud/free/
- Sign up (needs credit card for verification, but not charged - free tier is free forever)
- Choose Home Region: **Frankfurt (eu-frankfurt-1)** — best latency to Nigeria

### 2. Create Network (VCN)
- Oracle Cloud Console → Networking → Virtual Cloud Networks → Create VCN
- Name: `fleet-vcn`
- IPv4 CIDR: `10.0.0.0/16`
- Check: Create Internet Gateway, Create NAT Gateway, Create Service Gateway
- Create

### 3. Open Ports (Security List)
- In your VCN → Security Lists → Default Security List → Add Ingress Rules:
  ```
  Rule 1: Source 0.0.0.0/0, Protocol TCP, Port 22 (SSH)
  Rule 2: Source 0.0.0.0/0, Protocol TCP, Port 80 (HTTP for trackers)
  Rule 3: Source 0.0.0.0/0, Protocol TCP, Port 443 (HTTPS for dashboard)
  Rule 4: Source 0.0.0.0/0, Protocol TCP, Port 8000 (direct backend, optional)
  ```
- Save

### 4. Create Free VM
- Compute → Instances → Create Instance
- Name: `fleet-tracker`
- Image: **Ubuntu 22.04**
- Shape: Click Change Shape → Ampere → **VM.Standard.A1.Flex**
  - OCPU: **4** (free max)
  - Memory: **24 GB** (free max)
- Networking: Use your `fleet-vcn`, Public subnet, **Assign public IPv4: Yes**
- Add SSH Key: Generate new or upload your existing (save private key!)
- Boot Volume: 100GB (free max is 200GB total)
- Create → Wait 1-2 min → Running

### 5. Reserve Public IP (so IP never changes)
- Networking → IP Management → Reserved Public IPs → Reserve Public IP
- Name: `fleet-ip`, Pool: Oracle, Compartment: your compartment
- Reserve → Then Assign to your `fleet-tracker` instance
- **Copy this IP**, e.g. `130.61.45.123` — this is your permanent server IP

### 6. SSH into VM
```bash
# On your PC (Linux/Mac/WSL)
chmod 400 your_private_key.key
ssh -i your_private_key.key ubuntu@130.61.45.123

# Windows: Use PuTTY with same key
```

### 7. Install Docker + Deploy (inside VM)
```bash
# Update
sudo apt update && sudo apt upgrade -y

# Docker
sudo apt install -y docker.io docker-compose git
sudo usermod -aG docker ubuntu
# Logout and login again (or newgrp docker)

# Clone
git clone https://github.com/BU-SAM/FLEET-TRACKER.git
cd FLEET-TRACKER
git checkout arena/01a0c3f3-fleet-tracker
mkdir -p data

# Run (HTTP only for now, works for both trackers and map)
sudo docker-compose up -d --build

# Check logs
sudo docker-compose logs -f
# You should see: Backend: http://0.0.0.0:8000

# Test
curl http://localhost:8000/api/health
```

**Your server is now live at `http://130.61.45.123:8000` and `http://130.61.45.123` (via Nginx)**

---

## Part 2: DuckDNS Free Custom Name ($0)

### 1. Create Free Subdomain
- Go to https://www.duckdns.org → Sign in with Google/GitHub
- Create subdomain: e.g. `bu-sam-fleet` → you get `bu-sam-fleet.duckdns.org`
- Set IP: Enter your Oracle reserved IP `130.61.45.123` → Add/Update
- Done! Now `http://bu-sam-fleet.duckdns.org` points to your VM

### 2. Auto-Update IP (if Oracle IP ever changes, though reserved IP won't)
On your Oracle VM:
```bash
# Create update script
echo 'echo url="https://www.duckdns.org/update?domains=bu-sam-fleet&token=YOUR_TOKEN_HERE&ip=" | curl -k -o ~/duckdns/duck.log -K -' > ~/duckdns.sh
chmod +x ~/duckdns.sh

# Or simpler, add cron every 5 min
crontab -e
# Add line:
# */5 * * * * curl -s "https://www.duckdns.org/update?domains=bu-sam-fleet&token=YOUR_TOKEN&ip=" > /dev/null
```

### 3. Add HTTPS for Dashboard (Free Let's Encrypt)
```bash
# On Oracle VM
sudo apt install -y certbot

# Get cert (DuckDNS supports Let's Encrypt)
sudo certbot certonly --webroot -w ./data/certbot/www -d bu-sam-fleet.duckdns.org

# Edit nginx.conf: set server_name to bu-sam-fleet.duckdns.org
nano nginx.conf
# Change: server_name bu-sam-fleet.duckdns.org;

# Run with prod profile (Nginx + HTTPS)
sudo docker-compose --profile prod up -d --build
```

Now:
- Trackers: `http://bu-sam-fleet.duckdns.org:80/api/v1/geolinker` (plain HTTP for SIM800L)
- Dashboard: `https://bu-sam-fleet.duckdns.org` (secure HTTPS for you)

---

## Part 3: Update Trackers

In your Arduino firmware:
```cpp
static const char* LOCAL_SERVER_HOST = "bu-sam-fleet.duckdns.org"; // your free name
static const int   LOCAL_SERVER_PORT = 80;
static const char* LOCAL_API_PATH    = "/api/v1/geolinker";
static const char* DEVICE_ID = "car_tracker_01"; // change per car
```

Flash all trackers. They will now send to Oracle Free VM forever, no PC needed!

---

## Part 4: View Map

From any phone/PC anywhere:
```
https://bu-sam-fleet.duckdns.org
```
- Live map, 50 cars, fill % bar, optimize button
- No PC online needed

---

## Part 5: When Ready to Scale to Paid (Hetzner + .com)

Same code, just change DNS:

1. Buy domain at WhoGoHost or Namecheap: `fleet.yourdomain.com` ~$12/year
2. Create Hetzner CX11 VPS €4.15/mo (Frankfurt)
3. Same deploy steps as above: `git clone ... docker-compose up -d`
4. Change DuckDNS A record to Hetzner IP, or create new A record `fleet.yourdomain.com → Hetzner IP`
5. Update trackers HOST to new domain (or keep DuckDNS, both work)
6. Delete Oracle VM (or keep as backup)

No code change needed — just IP/domain swap.

---

## Free Tier Limits & Tips

- Oracle Free: 4 OCPU, 24GB RAM forever, but you must login to console every 30 days or they reclaim idle VM. Set a calendar reminder.
- If VM says "Out of capacity" for Ampere, try different availability domain (AD-1, AD-2, AD-3) or try early morning WAT.
- DuckDNS: Free, but update IP every 5 min via cron if you use ephemeral IP (reserved IP recommended, never changes).
- Backup DB weekly: `scp -i key ubuntu@IP:~/FLEET-TRACKER/backend/fleet.db ./backup.db`

---

## Quick Test Without Hardware (on Oracle VM)

```bash
# On Oracle VM
sudo apt install -y python3-pip
pip3 install requests
cd ~/FLEET-TRACKER/backend
python3 simulate_device.py --device car_tracker_01 --loop --interval 5 &
python3 simulate_device.py --device car_tracker_02 --loop --interval 7 &
# Check map at https://bu-sam-fleet.duckdns.org
```

---

## Troubleshooting

- **Can't SSH**: Check Security List ports 22 open, and use correct private key + ubuntu user
- **curl health fails**: `sudo docker-compose logs -f` → check if port 8000 bound
- **Tracker TCP fail**: Check Security List port 80 open, and DuckDNS IP matches Oracle reserved IP
- **HTTPS fails**: Ensure DuckDNS domain resolves: `nslookup bu-sam-fleet.duckdns.org` should show your Oracle IP
