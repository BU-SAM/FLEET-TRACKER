"""
FLEET-TRACKER Backend - Production Ready with 10k Limit + Smart Optimization
- Compatible with CircuitDigest GeoLinker payload format
- Simple /api/track endpoint for new firmware
- Serves frontend static files
- SQLite storage with FIFO 10k limit + useless-point removal
"""
import os
import json
from datetime import datetime
from typing import Optional, List, Any
from pathlib import Path

from fastapi import FastAPI, Request, Header, HTTPException, Query
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from fastapi.responses import HTMLResponse, FileResponse, JSONResponse
from pydantic import BaseModel

import database as db

# Config
API_KEY_REQUIRED = os.getenv("FLEET_API_KEY", "")  # if empty, accept any key
HOST = os.getenv("HOST", "0.0.0.0")
PORT = int(os.getenv("PORT", "8000"))
MAX_POINTS = db.MAX_POINTS_PER_DEVICE

app = FastAPI(
    title="FLEET-TRACKER API",
    description=f"Local fleet tracking server - GeoLinker compatible - Max {MAX_POINTS} pts/device with smart optimization",
    version="2.0.0"
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Init DB
db.init_db()

# ---------- Models ----------
class GeoLinkerPayload(BaseModel):
    device_id: str
    timestamp: List[str]
    lat: List[float]
    long: Optional[List[float]] = None
    lon: Optional[List[float]] = None
    payload: Optional[List[Any]] = None
    battery: Optional[List[float]] = None

class SimpleTrackPayload(BaseModel):
    device_id: str
    lat: float
    lon: Optional[float] = None
    long: Optional[float] = None
    timestamp: Optional[str] = None
    speed: Optional[float] = None
    sats: Optional[int] = None
    battery: Optional[float] = None
    payload: Optional[dict] = None

# ---------- Helpers ----------
def parse_timestamp(ts_str: Optional[str]) -> str:
    if not ts_str:
        return datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S")
    return ts_str

def extract_payload_data(payload_item):
    """Extract speed, sats, battery from payload dict if present"""
    if not isinstance(payload_item, dict):
        return None, None, None, payload_item
    speed = payload_item.get("speed")
    sats = payload_item.get("sats")
    battery = payload_item.get("battery")
    return speed, sats, battery, payload_item

# ---------- API Routes ----------
@app.get("/api/health")
def health():
    return {
        "status": "ok",
        "time": datetime.utcnow().isoformat(),
        "max_points_per_device": MAX_POINTS,
        "optimization": {
            "min_distance_m": db.MIN_DISTANCE_M,
            "stationary_distance_m": db.STATIONARY_DISTANCE_M,
            "stationary_speed_kmh": db.STATIONARY_SPEED_KMH,
            "stationary_time_s": db.STATIONARY_TIME_S
        }
    }

@app.get("/api/stats")
def stats():
    return db.get_stats()

@app.get("/api/config")
def get_config():
    return {
        "max_points_per_device": MAX_POINTS,
        "min_distance_m": db.MIN_DISTANCE_M,
        "stationary_distance_m": db.STATIONARY_DISTANCE_M,
        "stationary_speed_kmh": db.STATIONARY_SPEED_KMH,
        "stationary_time_s": db.STATIONARY_TIME_S,
        "min_time_between_points_s": db.MIN_TIME_BETWEEN_POINTS_S,
        "collinear_tolerance_m": db.COLLINEAR_TOLERANCE_M
    }

@app.post("/api/v1/geolinker")
async def geolinker_endpoint(request: Request, authorization: Optional[str] = Header(None)):
    """
    Drop-in replacement for CircuitDigest GeoLinker
    POST www.circuitdigest.cloud/api/v1/geolinker
    Headers: Authorization: <API key>
    Body: {"device_id":"...", "timestamp":["..."], "lat":[...], "long":[...], "payload":[{...}]}
    
    Features:
    - 10k points limit per device (FIFO - oldest deleted)
    - Smart filtering: skips jitter, stationary duplicates, redundant close points
    - Auto-optimization when near limit
    """
    # API key check (optional)
    if API_KEY_REQUIRED:
        if not authorization or authorization.strip() != API_KEY_REQUIRED:
            if authorization and authorization.startswith("Bearer "):
                token = authorization[7:]
                if token != API_KEY_REQUIRED:
                    raise HTTPException(status_code=401, detail="Invalid API key")
            else:
                if authorization != API_KEY_REQUIRED:
                    raise HTTPException(status_code=401, detail="Invalid API key")

    try:
        body = await request.json()
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"Invalid JSON: {e}")

    device_id = body.get("device_id")
    if not device_id:
        raise HTTPException(status_code=400, detail="device_id missing")

    timestamps = body.get("timestamp", [])
    lats = body.get("lat", [])
    longs = body.get("long", body.get("lon", []))
    payloads = body.get("payload", [])
    batteries = body.get("battery", [])

    if not lats or not longs:
        raise HTTPException(status_code=400, detail="lat/long missing")
    if len(lats) != len(longs):
        raise HTTPException(status_code=400, detail="lat/long length mismatch")

    inserted = 0
    skipped = 0
    skip_reasons = {}
    for i in range(len(lats)):
        ts = timestamps[i] if i < len(timestamps) else parse_timestamp(None)
        lat = lats[i]
        lon = longs[i]
        pl = payloads[i] if i < len(payloads) else None
        speed, sats, batt, full_payload = extract_payload_data(pl) if pl else (None, None, None, None)

        if batteries and i < len(batteries):
            batt = batteries[i]
            if full_payload is None:
                full_payload = {}
            if isinstance(full_payload, dict):
                full_payload["battery"] = batt

        if not (-90 <= lat <= 90) or not (-180 <= lon <= 180):
            skipped += 1
            continue
        if lat == 0 and lon == 0:
            skipped += 1
            continue

        ok, reason = db.insert_point(
            device_id=device_id,
            timestamp_str=parse_timestamp(ts),
            lat=lat,
            lon=lon,
            speed=speed,
            sats=sats,
            payload=full_payload
        )
        if ok:
            inserted += 1
        else:
            skipped += 1
            skip_reasons[reason] = skip_reasons.get(reason, 0) + 1

    # Get current count after insert
    current_count = db.get_point_count(device_id)

    return JSONResponse(
        content={
            "status": "stored" if inserted>0 else "skipped",
            "device_id": device_id,
            "points_stored": inserted,
            "points_skipped": skipped,
            "skip_reasons": skip_reasons,
            "current_total": current_count,
            "max_points": MAX_POINTS,
            "timestamp": datetime.utcnow().isoformat()
        },
        status_code=201 if inserted > 0 else 200
    )

@app.post("/api/track")
def simple_track(payload: SimpleTrackPayload, authorization: Optional[str] = Header(None)):
    """Simpler endpoint for new firmware or testing"""
    if API_KEY_REQUIRED:
        if authorization and authorization != API_KEY_REQUIRED and authorization.replace("Bearer ", "") != API_KEY_REQUIRED:
            raise HTTPException(status_code=401, detail="Invalid API key")

    device_id = payload.device_id
    lat = payload.lat
    lon = payload.lon if payload.lon is not None else payload.long
    if lon is None:
        raise HTTPException(status_code=400, detail="lon/long missing")

    if not (-90 <= lat <= 90) or not (-180 <= lon <= 180):
        raise HTTPException(status_code=400, detail="Invalid coordinates")
    if lat == 0 and lon == 0:
        raise HTTPException(status_code=400, detail="Zero coordinates ignored")

    ts = parse_timestamp(payload.timestamp)

    extra = payload.payload or {}
    if payload.speed is not None:
        extra["speed"] = payload.speed
    if payload.sats is not None:
        extra["sats"] = payload.sats
    if payload.battery is not None:
        extra["battery"] = payload.battery

    ok, reason = db.insert_point(
        device_id=device_id,
        timestamp_str=ts,
        lat=lat,
        lon=lon,
        speed=payload.speed,
        sats=payload.sats,
        payload=extra if extra else None
    )

    current_count = db.get_point_count(device_id)

    if not ok:
        return JSONResponse(
            content={"status": "skipped", "reason": reason, "device_id": device_id, "current_total": current_count},
            status_code=200
        )

    return {"status": "stored", "device_id": device_id, "timestamp": ts, "current_total": current_count, "max_points": MAX_POINTS}

@app.get("/api/devices")
def list_devices():
    devices = db.get_devices()
    now = datetime.utcnow()
    for d in devices:
        try:
            last = datetime.fromisoformat(d["last_seen"].replace(" ", "T"))
            diff = (now - last).total_seconds()
            d["is_online"] = diff < 300
            d["seconds_ago"] = int(diff)
            # Add fill percentage
            d["fill_percent"] = round((d["total_points"] / MAX_POINTS) * 100, 1) if MAX_POINTS else 0
            d["points_remaining"] = MAX_POINTS - d["total_points"]
        except:
            d["is_online"] = False
            d["seconds_ago"] = None
            d["fill_percent"] = 0
            d["points_remaining"] = MAX_POINTS
    return devices

@app.get("/api/devices/{device_id}")
def get_device_info(device_id: str):
    dev = db.get_device(device_id)
    if not dev:
        raise HTTPException(status_code=404, detail="Device not found")
    latest = db.get_latest(device_id)
    count = db.get_point_count(device_id)
    dev["fill_percent"] = round((count / MAX_POINTS) * 100, 1)
    return {"device": dev, "latest": latest, "count": count, "max_points": MAX_POINTS}

@app.get("/api/devices/{device_id}/latest")
def get_latest_point(device_id: str):
    point = db.get_latest(device_id)
    if not point:
        raise HTTPException(status_code=404, detail="No points for device")
    return point

@app.get("/api/devices/{device_id}/history")
def get_history(
    device_id: str,
    limit: int = Query(1000, ge=1, le=10000),
    order: str = Query("asc", pattern="^(asc|desc)$"),
    from_ts: Optional[str] = None,
    to_ts: Optional[str] = None
):
    dev = db.get_device(device_id)
    if not dev:
        return []
    history = db.get_history(device_id, limit=limit, order=order, from_ts=from_ts, to_ts=to_ts)
    return history

@app.delete("/api/devices/{device_id}/history")
def clear_history(device_id: str):
    db.delete_history(device_id)
    return {"status": "cleared", "device_id": device_id}

@app.delete("/api/devices/{device_id}")
def delete_device(device_id: str):
    db.delete_device(device_id)
    return {"status": "deleted", "device_id": device_id}

# ===== NEW: Optimization Endpoints =====
@app.post("/api/devices/{device_id}/optimize")
def optimize_device_endpoint(device_id: str, aggressive: bool = Query(False)):
    """
    Smart optimization:
    - Removes jitter points (<5m)
    - Compresses stationary clusters (keep 1 per 5 min when idle)
    - Removes collinear points on straight roads
    - Keeps first/last, important turns, speed changes
    """
    dev = db.get_device(device_id)
    if not dev:
        raise HTTPException(status_code=404, detail="Device not found")
    result = db.optimize_device(device_id, aggressive=aggressive)
    return {
        "device_id": device_id,
        "optimization": result,
        "current_total": db.get_point_count(device_id),
        "max_points": MAX_POINTS
    }

@app.post("/api/optimize-all")
def optimize_all_endpoint(aggressive: bool = Query(False)):
    """Optimize all devices"""
    results = db.optimize_all_devices(aggressive=aggressive)
    total_deleted = sum(r["deleted"] for r in results.values())
    return {
        "results": results,
        "total_deleted": total_deleted,
        "devices_optimized": len(results)
    }

@app.get("/api/devices/{device_id}/optimization-preview")
def optimization_preview(device_id: str, aggressive: bool = Query(False)):
    """Preview what would be deleted without actually deleting"""
    points = db.get_all_history_asc(device_id)
    if len(points) < 10:
        return {"device_id": device_id, "original": len(points), "would_keep": len(points), "would_delete": 0}

    # Simulate optimization logic to count
    # For preview, we run same logic but don't delete - we use optimize_device with dry run? 
    # Simplest: call optimize and then we have already deleted, so we need a dry-run version
    # We'll implement quick estimation
    from database import haversine_m, parse_ts
    # Quick estimation: count points that would be kept using same rules
    kept = 0
    last_kept = None
    last_stationary = None
    min_dist = 5 if aggressive else db.MIN_DISTANCE_M
    
    for i, p in enumerate(points):
        if i == 0 or i == len(points)-1:
            kept += 1
            last_kept = p
            continue
        if last_kept is None:
            kept += 1
            last_kept = p
            continue
        try:
            dist = haversine_m(last_kept["lat"], last_kept["lon"], p["lat"], p["lon"])
            if dist < 5:
                continue
            cur_speed = p["speed"] or 0
            last_speed = last_kept["speed"] or 0
            if cur_speed < db.STATIONARY_SPEED_KMH and last_speed < db.STATIONARY_SPEED_KMH and dist < db.STATIONARY_DISTANCE_M:
                if last_stationary and (parse_ts(p["timestamp"]) - last_stationary).total_seconds() < db.STATIONARY_TIME_S:
                    continue
                last_stationary = parse_ts(p["timestamp"])
            if dist < min_dist:
                continue
            kept += 1
            last_kept = p
        except:
            kept += 1
            last_kept = p

    return {
        "device_id": device_id,
        "original": len(points),
        "would_keep": kept,
        "would_delete": len(points) - kept,
        "savings_percent": round((len(points)-kept)/len(points)*100, 1) if points else 0,
        "aggressive": aggressive
    }

# ---------- Frontend Static ----------
FRONTEND_DIR = Path(__file__).parent.parent / "frontend"
if FRONTEND_DIR.exists():
    @app.get("/", response_class=HTMLResponse)
    def serve_index():
        index_path = FRONTEND_DIR / "index.html"
        if index_path.exists():
            return FileResponse(index_path)
        return HTMLResponse("<h1>FLEET-TRACKER API</h1><p>Frontend not found. API at /api</p>")

    app.mount("/static", StaticFiles(directory=FRONTEND_DIR), name="static")

    @app.get("/app.js")
    def serve_js():
        return FileResponse(FRONTEND_DIR / "app.js", media_type="application/javascript")

    @app.get("/style.css")
    def serve_css():
        return FileResponse(FRONTEND_DIR / "style.css", media_type="text/css")
else:
    @app.get("/", response_class=HTMLResponse)
    def root_no_frontend():
        return HTMLResponse("""
        <h1>FLEET-TRACKER API Running</h1>
        <p>Frontend folder not found. API available at:</p>
        <ul>
          <li><a href="/docs">/docs - Swagger UI</a></li>
          <li><a href="/api/devices">/api/devices</a></li>
          <li><a href="/api/stats">/api/stats</a></li>
          <li><a href="/api/config">/api/config</a></li>
        </ul>
        """)

if __name__ == "__main__":
    import uvicorn
    print(f"""
    ========================================
     FLEET-TRACKER v2.0 - Optimized
     Backend: http://{HOST}:{PORT}
     API Docs: http://{HOST}:{PORT}/docs
     Frontend: http://{HOST}:{PORT}/
     DB: {db.DB_PATH}
     Max Points/Device: {MAX_POINTS} (FIFO)
     Optimization: jitter<5m, stationary 1 per {db.STATIONARY_TIME_S}s, collinear<{db.COLLINEAR_TOLERANCE_M}m
     API Key Required: {'Yes - ' + API_KEY_REQUIRED if API_KEY_REQUIRED else 'No (open)'}
    ========================================
    """)
    uvicorn.run("app:app", host=HOST, port=PORT, reload=True)
