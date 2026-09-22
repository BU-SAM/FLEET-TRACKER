"""
Simulate a car tracker device for testing without hardware
Usage:
  python simulate_device.py --device car_tracker_01
  python simulate_device.py --device car_tracker_02 --loop --interval 5
  python simulate_device.py --device car_tracker_01 --lat 6.5244 --lon 3.3792 --server http://localhost:8000
"""
import argparse
import time
import random
import requests
from datetime import datetime, timedelta

def send_geolinker_format(server, device_id, lat, lon, speed, sats):
    url = f"{server}/api/v1/geolinker"
    ts = datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S")
    payload = {
        "device_id": device_id,
        "timestamp": [ts],
        "lat": [lat],
        "long": [lon],
        "payload": [{"speed": speed, "sats": sats}]
    }
    try:
        r = requests.post(url, json=payload, timeout=10)
        print(f"[{ts}] {device_id} -> {lat:.6f},{lon:.6f} speed={speed} sats={sats} | {r.status_code} {r.text[:100]}")
        return r.status_code in (200, 201)
    except Exception as e:
        print(f"Error: {e}")
        return False

def send_simple_format(server, device_id, lat, lon, speed, sats):
    url = f"{server}/api/track"
    ts = datetime.utcnow().strftime("%Y-%m-%d %H:%M:%S")
    payload = {
        "device_id": device_id,
        "lat": lat,
        "lon": lon,
        "timestamp": ts,
        "speed": speed,
        "sats": sats
    }
    try:
        r = requests.post(url, json=payload, timeout=10)
        print(f"[{ts}] {device_id} -> {lat:.6f},{lon:.6f} | {r.status_code}")
        return True
    except Exception as e:
        print(f"Error: {e}")
        return False

def main():
    parser = argparse.ArgumentParser(description="Simulate fleet tracker")
    parser.add_argument("--device", default="car_tracker_01", help="Device ID")
    parser.add_argument("--server", default="http://localhost:8000", help="Server URL")
    parser.add_argument("--lat", type=float, default=6.5244, help="Start lat (Lagos default)")
    parser.add_argument("--lon", type=float, default=3.3792, help="Start lon")
    parser.add_argument("--loop", action="store_true", help="Continuous loop")
    parser.add_argument("--interval", type=int, default=10, help="Interval seconds")
    parser.add_argument("--format", choices=["geolinker", "simple"], default="geolinker", help="Payload format")
    args = parser.parse_args()

    lat = args.lat
    lon = args.lon

    print(f"Simulating {args.device} at {args.server} | {args.format} format")
    print(f"Start: {lat}, {lon}")
    if args.loop:
        print(f"Looping every {args.interval}s - Ctrl+C to stop")

    def one_send():
        nonlocal lat, lon
        # Move slightly to simulate driving
        lat += random.uniform(-0.0005, 0.0005)
        lon += random.uniform(-0.0005, 0.0005)
        speed = random.uniform(0, 80)
        sats = random.randint(6, 12)
        if args.format == "geolinker":
            return send_geolinker_format(args.server, args.device, lat, lon, speed, sats)
        else:
            return send_simple_format(args.server, args.device, lat, lon, speed, sats)

    if not args.loop:
        one_send()
    else:
        try:
            while True:
                one_send()
                time.sleep(args.interval)
        except KeyboardInterrupt:
            print("\nStopped")

if __name__ == "__main__":
    main()
