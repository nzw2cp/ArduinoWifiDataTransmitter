import asyncio
import websockets
import socket
import csv
from pathlib import Path
from typing import List, Tuple

CSV_PATH = "test.csv"

def parse_payload(message: str) -> Tuple[str, List[List[float]]]:
    lines = message.splitlines()
    if not lines:
        return "", []
    device_name = lines[0].strip()
    if not device_name:
        return "", []
    rows: List[List[float]] = []
    for raw_line in lines[1:]:
        line = raw_line.strip()
        if not line:
            continue
        parts = [part.strip() for part in line.split(",")]
        if len(parts) < 3:
            continue
        try:
            rows.append([float(parts[0]), float(parts[1]), float(parts[2])])
        except ValueError:
            continue
    return device_name, rows

async def handler(websocket):
    try:
        async for message in websocket:
            device_name, rows = parse_payload(message)
            if not device_name or not rows:
                continue

            device_path = Path(f"{device_name}.csv")
            mode = "a" if device_path.exists() else "w"
            with device_path.open(mode, newline="") as csvfile:
                writer = csv.writer(csvfile)
                writer.writerows(rows)
            await websocket.send(f"ACK {device_name} {len(rows)}")
    finally:
        await websocket.close()

def get_local_ip() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        try:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
        except OSError:
            return "127.0.0.1"

async def main():
    server_ip = get_local_ip()
    async with websockets.serve(handler, "0.0.0.0", 8765):
        print(f"WebSocket server ready on ws://{server_ip}:8765")
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())