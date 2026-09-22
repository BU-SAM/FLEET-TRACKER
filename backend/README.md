# Backend

## Run
```bash
pip install -r requirements.txt
python app.py
```

Server runs at http://0.0.0.0:8000
- Frontend: http://localhost:8000/
- API Docs: http://localhost:8000/docs
- Health: http://localhost:8000/api/health

## Environment Variables
- `PORT` - default 8000
- `HOST` - default 0.0.0.0
- `FLEET_API_KEY` - if set, requires Authorization header to match. Leave empty for open server (recommended for local testing).

Example with API key:
```bash
FLEET_API_KEY=cd_sam_080926_JY1Jhs python app.py
```

## Database
SQLite file `fleet.db` auto-created in this folder.
- `devices` - one row per device_id
- `points` - history of coordinates

To reset:
```bash
rm fleet.db
```

## Test without hardware
```bash
pip install requests
python simulate_device.py --device car_tracker_01 --loop --interval 5
python simulate_device.py --device car_tracker_02 --loop --interval 7
```

## API Examples
```bash
# GeoLinker compatible (what TTGO sends)
curl -X POST http://localhost:8000/api/v1/geolinker \
  -H "Content-Type: application/json" \
  -d '{"device_id":"car_tracker_01","timestamp":["2025-09-21 14:30:00"],"lat":[6.5244],"long":[3.3792],"payload":[{"speed":45.5,"sats":8}]}'

# Simple
curl -X POST http://localhost:8000/api/track \
  -H "Content-Type: application/json" \
  -d '{"device_id":"car_tracker_01","lat":6.5244,"lon":3.3792,"speed":45.5}'

# List devices
curl http://localhost:8000/api/devices

# History
curl http://localhost:8000/api/devices/car_tracker_01/history?limit=100&order=asc

# Latest
curl http://localhost:8000/api/devices/car_tracker_01/latest
```
