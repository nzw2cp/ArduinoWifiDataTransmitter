import asyncio
import socket
import csv
from pathlib import Path
from typing import List, Tuple


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
        # expect: time, x, y, z  -> require at least 4 parts
        if len(parts) < 4:
            continue
        try:
            # parse first four values: time, x, y, z
            rows.append([float(parts[0]), float(parts[1]), float(parts[2]), float(parts[3])])
        except ValueError:
            continue
    return device_name, rows

async def handler(reader, writer):
    peer = writer.get_extra_info("peername")
    print(f"[server] Connection from {peer}")
    try:
        # read multiple messages per connection; each message is terminated by a blank line
        while True:
            lines = []
            while True:
                line = await reader.readline()
                if not line:
                    # connection closed by peer
                    break
                # if line is just a newline -> message terminator
                if line in (b'\n', b'\r\n'):
                    break
                lines.append(line.decode("utf-8", errors="replace"))
            # if no data and connection closed, exit
            if not lines and reader.at_eof():
                print(f"[server] Connection closed by peer {peer} (no more data)")
                break
            # build message from collected lines
            message = "".join(lines)
            if not message:
                # nothing to process (could be consecutive blank lines); continue reading
                continue
            print(f"[server] Received {len(message)} bytes from {peer} (message truncated 200 chars): {message[:200]!r}")
            device_name, rows = parse_payload(message)
            if not device_name or not rows:
                print(f"[server] Invalid payload from {peer}: device='{device_name}', rows={len(rows)}")
                # continue to next message without sending ACK
                continue

            device_path = Path(f"./Data/{device_name}.csv")
            mode = "a" if device_path.exists() else "w"
            with device_path.open(mode, newline="") as csvfile:
                writer_csv = csv.writer(csvfile)
                writer_csv.writerows(rows)
            print(f"[server] Saved {len(rows)} rows to {device_path}")

            ack = f"ACK {device_name} {len(rows)}\n".encode("utf-8")
            try:
                writer.write(ack)
                await writer.drain()
                print(f"[server] Sent ACK to {peer}: {ack.decode().strip()}")
            except Exception as e:
                print(f"[server] Failed to send ACK to {peer}: {e}")
                # if send fails, close connection loop
                break
    finally:
        try:
            writer.close()
            await writer.wait_closed()
        except Exception:
            pass
        print(f"[server] Connection closed: {peer}")

def get_local_ip() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        try:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
        except OSError:
            return "127.0.0.1"

async def main():
    server_ip = get_local_ip()
    server = await asyncio.start_server(handler, "0.0.0.0", 8765)
    print(f"TCP server ready on tcp://{server_ip}:8765")
    async with server:
        await server.serve_forever()

if __name__ == "__main__":
    asyncio.run(main())