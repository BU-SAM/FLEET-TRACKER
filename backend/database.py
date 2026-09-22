import sqlite3
import os
import json
import math
from datetime import datetime, timedelta
from pathlib import Path

DB_PATH = Path(__file__).parent / "fleet.db"

# ===== Config - Tunable via env =====
MAX_POINTS_PER_DEVICE = int(os.getenv("MAX_POINTS_PER_DEVICE", "10000"))
MIN_DISTANCE_M = float(os.getenv("MIN_DISTANCE_M", "10"))  # min distance to store new point
STATIONARY_DISTANCE_M = float(os.getenv("STATIONARY_DISTANCE_M", "15"))
STATIONARY_SPEED_KMH = float(os.getenv("STATIONARY_SPEED_KMH", "2.0"))
STATIONARY_TIME_S = int(os.getenv("STATIONARY_TIME_S", "300"))  # 5 min - keep 1 point per 5 min when idle
MIN_TIME_BETWEEN_POINTS_S = int(os.getenv("MIN_TIME_BETWEEN_POINTS_S", "10"))
COLLINEAR_TOLERANCE_M = float(os.getenv("COLLINEAR_TOLERANCE_M", "8"))  # for straight line simplification

def get_conn():
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("""
        CREATE TABLE IF NOT EXISTS devices (
            device_id TEXT PRIMARY KEY,
            created_at TEXT,
            last_seen TEXT,
            last_lat REAL,
            last_lon REAL,
            total_points INTEGER DEFAULT 0,
            optimized_points INTEGER DEFAULT 0,
            last_optimized TEXT
        )
    """)
    # Migration: add columns if old DB
    try:
        cur.execute("ALTER TABLE devices ADD COLUMN optimized_points INTEGER DEFAULT 0")
    except: pass
    try:
        cur.execute("ALTER TABLE devices ADD COLUMN last_optimized TEXT")
    except: pass

    cur.execute("""
        CREATE TABLE IF NOT EXISTS points (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT,
            timestamp TEXT,
            lat REAL,
            lon REAL,
            speed REAL,
            sats INTEGER,
            payload TEXT,
            created_at TEXT,
            FOREIGN KEY(device_id) REFERENCES devices(device_id)
        )
    """)
    cur.execute("CREATE INDEX IF NOT EXISTS idx_points_device ON points(device_id)")
    cur.execute("CREATE INDEX IF NOT EXISTS idx_points_timestamp ON points(timestamp)")
    cur.execute("CREATE INDEX IF NOT EXISTS idx_points_device_time ON points(device_id, timestamp DESC)")
    cur.execute("CREATE INDEX IF NOT EXISTS idx_points_device_id_asc ON points(device_id, timestamp ASC)")
    conn.commit()
    conn.close()

# ===== Geo helpers =====
def haversine_m(lat1, lon1, lat2, lon2):
    R = 6371000  # meters
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlambda = math.radians(lon2 - lon1)
    a = math.sin(dphi/2)**2 + math.cos(phi1)*math.cos(phi2)*math.sin(dlambda/2)**2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1-a))
    return R * c

def parse_ts(ts_str):
    # Try multiple formats
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S", "%Y-%m-%d %H:%M:%S.%f"):
        try:
            return datetime.strptime(ts_str.split(".")[0] if "." in ts_str and fmt != "%Y-%m-%d %H:%M:%S.%f" else ts_str, fmt)
        except:
            continue
    try:
        # ISO with T
        return datetime.fromisoformat(ts_str.replace(" ", "T"))
    except:
        return datetime.utcnow()

def bearing_deg(lat1, lon1, lat2, lon2):
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dlambda = math.radians(lon2 - lon1)
    x = math.sin(dlambda) * math.cos(phi2)
    y = math.cos(phi1)*math.sin(phi2) - math.sin(phi1)*math.cos(phi2)*math.cos(dlambda)
    return (math.degrees(math.atan2(x, y)) + 360) % 360

def cross_track_distance_m(lat1, lon1, lat2, lon2, lat3, lon3):
    """Distance of point 3 from line 1-2 (for collinear check)"""
    # If line is very short, return distance to point 1
    d13 = haversine_m(lat1, lon1, lat3, lon3)
    if haversine_m(lat1, lon1, lat2, lon2) < 1:
        return d13
    # Use bearing method
    brng12 = math.radians(bearing_deg(lat1, lon1, lat2, lon2))
    brng13 = math.radians(bearing_deg(lat1, lon1, lat3, lon3))
    # cross-track
    dXt = math.asin(math.sin(d13/6371000) * math.sin(brng13 - brng12)) * 6371000
    return abs(dXt)

# ===== Core DB ops =====
def get_point_count(device_id):
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("SELECT COUNT(*) as c FROM points WHERE device_id=?", (device_id,))
    c = cur.fetchone()["c"]
    conn.close()
    return c

def upsert_device(device_id, lat, lon, ts, increment=True):
    conn = get_conn()
    cur = conn.cursor()
    now = datetime.utcnow().isoformat()
    cur.execute("SELECT device_id FROM devices WHERE device_id=?", (device_id,))
    row = cur.fetchone()
    if row:
        if increment:
            cur.execute("""
                UPDATE devices SET last_seen=?, last_lat=?, last_lon=?, total_points=(SELECT COUNT(*) FROM points WHERE device_id=?)
                WHERE device_id=?
            """, (ts, lat, lon, device_id, device_id))
        else:
            cur.execute("""
                UPDATE devices SET last_seen=?, last_lat=?, last_lon=?
                WHERE device_id=?
            """, (ts, lat, lon, device_id))
    else:
        cur.execute("""
            INSERT INTO devices (device_id, created_at, last_seen, last_lat, last_lon, total_points)
            VALUES (?, ?, ?, ?, ?, 1)
        """, (device_id, now, ts, lat, lon))
    conn.commit()
    conn.close()

def enforce_fifo_limit(device_id):
    """Delete oldest points if over MAX_POINTS_PER_DEVICE"""
    count = get_point_count(device_id)
    if count <= MAX_POINTS_PER_DEVICE:
        return 0
    excess = count - MAX_POINTS_PER_DEVICE
    conn = get_conn()
    cur = conn.cursor()
    # Delete oldest excess points
    cur.execute("""
        DELETE FROM points WHERE id IN (
            SELECT id FROM points WHERE device_id=? ORDER BY timestamp ASC LIMIT ?
        )
    """, (device_id, excess))
    deleted = cur.rowcount
    cur.execute("UPDATE devices SET total_points=(SELECT COUNT(*) FROM points WHERE device_id=?), last_optimized=? WHERE device_id=?",
                (device_id, datetime.utcnow().isoformat(), device_id))
    conn.commit()
    conn.close()
    return deleted

def insert_point(device_id, timestamp_str, lat, lon, speed=None, sats=None, payload=None, skip_optimization_check=False):
    """
    Smart insert:
    - Skip if too close & too soon & stationary
    - Insert
    - Enforce FIFO 10k limit
    - Auto-optimize if near limit
    Returns: (inserted:bool, reason:str)
    """
    # Validate
    if not (-90 <= lat <= 90) or not (-180 <= lon <= 180):
        return False, "invalid_coords"
    if lat == 0 and lon == 0:
        return False, "zero_coords"

    # Check last point for useless filtering
    if not skip_optimization_check:
        latest = get_latest(device_id)
        if latest:
            try:
                last_lat = latest["lat"]
                last_lon = latest["lon"]
                last_ts = parse_ts(latest["timestamp"])
                cur_ts = parse_ts(timestamp_str)
                dt = (cur_ts - last_ts).total_seconds()
                dist = haversine_m(last_lat, last_lon, lat, lon)

                # Rule 1: If very close (<5m) and <30s, skip (GPS jitter)
                if dist < 5 and abs(dt) < 30:
                    return False, f"too_close_jitter {dist:.1f}m {dt}s"

                # Rule 2: If stationary (<2 km/h) and <15m and <5min, skip - keep 1 per 5 min
                cur_speed = speed if speed is not None else 0
                last_speed = latest["speed"] if latest["speed"] is not None else 0
                if cur_speed < STATIONARY_SPEED_KMH and last_speed < STATIONARY_SPEED_KMH:
                    if dist < STATIONARY_DISTANCE_M and abs(dt) < STATIONARY_TIME_S:
                        return False, f"stationary_duplicate {dist:.1f}m"

                # Rule 3: If distance < MIN_DISTANCE and time < 60s and speed change small, skip
                if dist < MIN_DISTANCE_M and abs(dt) < 60:
                    speed_diff = abs((cur_speed or 0) - (last_speed or 0))
                    if speed_diff < 5:
                        return False, f"redundant_close {dist:.1f}m"

            except Exception as e:
                # If parsing fails, still insert
                pass

    # Insert
    conn = get_conn()
    cur = conn.cursor()
    created_at = datetime.utcnow().isoformat()
    payload_json = json.dumps(payload) if payload else None
    cur.execute("""
        INSERT INTO points (device_id, timestamp, lat, lon, speed, sats, payload, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    """, (device_id, timestamp_str, lat, lon, speed, sats, payload_json, created_at))
    conn.commit()
    conn.close()
    upsert_device(device_id, lat, lon, timestamp_str, increment=True)

    # Enforce FIFO limit
    deleted_fifo = enforce_fifo_limit(device_id)

    # Auto-optimize if near limit (90% full) to free space intelligently
    count = get_point_count(device_id)
    if count > MAX_POINTS_PER_DEVICE * 0.9:
        # Run light optimization
        try:
            optimize_device(device_id, aggressive=False)
        except Exception as e:
            print(f"Auto-optimize failed for {device_id}: {e}")

    return True, f"inserted fifo_deleted={deleted_fifo}"

def get_devices():
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("SELECT * FROM devices ORDER BY last_seen DESC")
    rows = cur.fetchall()
    conn.close()
    return [dict(r) for r in rows]

def get_device(device_id):
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("SELECT * FROM devices WHERE device_id=?", (device_id,))
    row = cur.fetchone()
    conn.close()
    return dict(row) if row else None

def get_latest(device_id):
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("SELECT * FROM points WHERE device_id=? ORDER BY timestamp DESC LIMIT 1", (device_id,))
    row = cur.fetchone()
    conn.close()
    return dict(row) if row else None

def get_history(device_id, limit=1000, order="desc", from_ts=None, to_ts=None):
    conn = get_conn()
    cur = conn.cursor()
    query = "SELECT * FROM points WHERE device_id=?"
    params = [device_id]
    if from_ts:
        query += " AND timestamp >= ?"
        params.append(from_ts)
    if to_ts:
        query += " AND timestamp <= ?"
        params.append(to_ts)
    order_sql = "DESC" if order.lower() == "desc" else "ASC"
    query += f" ORDER BY timestamp {order_sql} LIMIT ?"
    params.append(limit)
    cur.execute(query, params)
    rows = cur.fetchall()
    conn.close()
    result = []
    for r in rows:
        d = dict(r)
        if d.get("payload"):
            try:
                d["payload"] = json.loads(d["payload"])
            except:
                pass
        result.append(d)
    return result

def get_all_history_asc(device_id):
    """Get all points ASC for optimization"""
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("SELECT * FROM points WHERE device_id=? ORDER BY timestamp ASC", (device_id,))
    rows = cur.fetchall()
    conn.close()
    return [dict(r) for r in rows]

def delete_history(device_id):
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("DELETE FROM points WHERE device_id=?", (device_id,))
    cur.execute("UPDATE devices SET total_points=0, optimized_points=0 WHERE device_id=?", (device_id,))
    conn.commit()
    conn.close()

def delete_device(device_id):
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("DELETE FROM points WHERE device_id=?", (device_id,))
    cur.execute("DELETE FROM devices WHERE device_id=?", (device_id,))
    conn.commit()
    conn.close()

def get_stats():
    conn = get_conn()
    cur = conn.cursor()
    cur.execute("SELECT COUNT(*) as total_devices FROM devices")
    total_devices = cur.fetchone()["total_devices"]
    cur.execute("SELECT COUNT(*) as total_points FROM points")
    total_points = cur.fetchone()["total_points"]
    cur.execute("SELECT device_id, last_seen, total_points FROM devices ORDER BY last_seen DESC LIMIT 5")
    recent = [dict(r) for r in cur.fetchall()]
    # Calculate storage saved
    cur.execute("SELECT SUM(optimized_points) as total_optimized FROM devices")
    total_optimized = cur.fetchone()["total_optimized"] or 0
    conn.close()
    return {
        "total_devices": total_devices,
        "total_points": total_points,
        "total_optimized_deleted": total_optimized,
        "max_points_per_device": MAX_POINTS_PER_DEVICE,
        "recent_devices": recent
    }

# ===== Optimization Engine =====
def optimize_device(device_id, aggressive=False):
    """
    Smart optimization:
    - Removes stationary duplicates (keep 1 per 5 min when idle)
    - Removes collinear points on straight roads (Douglas-Peucker light)
    - Removes jitter points <5m
    Returns stats dict
    """
    points = get_all_history_asc(device_id)
    if len(points) < 10:
        return {"original": len(points), "optimized": len(points), "deleted": 0, "reason": "too_few"}

    kept = []
    deleted_ids = []

    # Config for aggressive mode
    min_dist = 5 if aggressive else MIN_DISTANCE_M
    stationary_dist = 10 if aggressive else STATIONARY_DISTANCE_M
    collinear_tol = 5 if aggressive else COLLINEAR_TOLERANCE_M

    last_kept = None
    last_stationary_time = None

    for i, p in enumerate(points):
        if i == 0 or i == len(points)-1:
            # Always keep first and last
            kept.append(p)
            last_kept = p
            continue

        if last_kept is None:
            kept.append(p)
            last_kept = p
            continue

        try:
            dist = haversine_m(last_kept["lat"], last_kept["lon"], p["lat"], p["lon"])
            dt = (parse_ts(p["timestamp"]) - parse_ts(last_kept["timestamp"])).total_seconds()
            cur_speed = p["speed"] if p["speed"] is not None else 0
            last_speed = last_kept["speed"] if last_kept["speed"] is not None else 0

            # Rule 1: Jitter - <5m
            if dist < 5:
                deleted_ids.append(p["id"])
                continue

            # Rule 2: Stationary cluster - keep 1 per STATIONARY_TIME_S
            if cur_speed < STATIONARY_SPEED_KMH and last_speed < STATIONARY_SPEED_KMH:
                if dist < stationary_dist:
                    # If we kept a stationary point recently, skip
                    if last_stationary_time and (parse_ts(p["timestamp"]) - last_stationary_time).total_seconds() < STATIONARY_TIME_S:
                        deleted_ids.append(p["id"])
                        continue
                    else:
                        last_stationary_time = parse_ts(p["timestamp"])
                        kept.append(p)
                        last_kept = p
                        continue

            # Rule 3: Too close and too soon
            if dist < min_dist and abs(dt) < 60:
                # Check speed change
                if abs(cur_speed - last_speed) < 5:
                    deleted_ids.append(p["id"])
                    continue

            # Rule 4: Collinear - check if this point is on straight line between last_kept and next
            # Look ahead 1 point
            if i+1 < len(points):
                next_p = points[i+1]
                # Only if all 3 are moving
                if cur_speed > 5 and last_speed > 5 and (next_p["speed"] or 0) > 5:
                    cross_dist = cross_track_distance_m(last_kept["lat"], last_kept["lon"], next_p["lat"], next_p["lon"], p["lat"], p["lon"])
                    if cross_dist < collinear_tol:
                        # Check distance: if last->next is not much longer than last->current + current->next, it's redundant
                        d_last_next = haversine_m(last_kept["lat"], last_kept["lon"], next_p["lat"], next_p["lon"])
                        d_last_cur = dist
                        d_cur_next = haversine_m(p["lat"], p["lon"], next_p["lat"], next_p["lon"])
                        if d_last_next > 0 and abs((d_last_cur + d_cur_next) - d_last_next) < 10:
                            deleted_ids.append(p["id"])
                            continue

            # Keep point
            kept.append(p)
            last_kept = p
            if cur_speed < STATIONARY_SPEED_KMH:
                last_stationary_time = parse_ts(p["timestamp"])
            else:
                last_stationary_time = None

        except Exception as e:
            # On error, keep point
            kept.append(p)
            last_kept = p

    # Delete useless points
    if deleted_ids:
        conn = get_conn()
        cur = conn.cursor()
        # Delete in batches
        for j in range(0, len(deleted_ids), 500):
            batch = deleted_ids[j:j+500]
            placeholders = ",".join("?" for _ in batch)
            cur.execute(f"DELETE FROM points WHERE id IN ({placeholders})", batch)
        cur.execute("UPDATE devices SET total_points=(SELECT COUNT(*) FROM points WHERE device_id=?), optimized_points=COALESCE(optimized_points,0)+?, last_optimized=? WHERE device_id=?",
                    (device_id, len(deleted_ids), datetime.utcnow().isoformat(), device_id))
        conn.commit()
        conn.close()

    return {
        "original": len(points),
        "optimized": len(kept),
        "deleted": len(deleted_ids),
        "kept_ids": len(kept),
        "aggressive": aggressive
    }

def optimize_all_devices(aggressive=False):
    devices = get_devices()
    results = {}
    for d in devices:
        res = optimize_device(d["device_id"], aggressive=aggressive)
        results[d["device_id"]] = res
    return results
