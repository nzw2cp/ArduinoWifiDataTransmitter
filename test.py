import asyncio
import socket
import csv
import os
import threading
import time
import math
import queue           # NEW: notification queue
from pathlib import Path
from typing import List, Tuple
from collections import deque

# GUI imports
import tkinter as tk
from tkinter import ttk
import matplotlib
matplotlib.use("TkAgg")
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg

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

# Thread-safe device state
devices = {}  # device_name -> {'last_checkin': int_ms, 'times': deque, 'norms': deque}
devices_lock = threading.Lock()
MAX_SERIES_LEN = 5000

# notification queue for GUI updates (device names)
notify_queue = queue.Queue()

# ensure Data directory exists
os.makedirs("./Data", exist_ok=True)

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

            # update in-memory device time series
            now_ms = int(time.time() * 1000)
            with devices_lock:
                dev = devices.get(device_name)
                if dev is None:
                    dev = {
                        'last_checkin': now_ms,
                        'times': deque(maxlen=MAX_SERIES_LEN),
                        'norms': deque(maxlen=MAX_SERIES_LEN),
                        'rows_received': 0
                    }
                    devices[device_name] = dev
                else:
                    dev['last_checkin'] = now_ms
                for r in rows:
                    # r = [time_s, x, y, z]
                    t = float(r[0])
                    x = float(r[1])
                    y = float(r[2])
                    z = float(r[3])
                    norm = math.sqrt(x * x + y * y + z * z)
                    dev['times'].append(t)
                    dev['norms'].append(norm)
                    dev['rows_received'] += 1

            # notify GUI that this device received new data
            try:
                notify_queue.put_nowait(device_name)
            except Exception:
                pass

            # persist CSV rows to ./Data/{device_name}.csv
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

async def main_server():
    server_ip = get_local_ip()
    server = await asyncio.start_server(handler, "0.0.0.0", 8765)
    print(f"TCP server ready on tcp://{server_ip}:8765")
    async with server:
        await server.serve_forever()

# GUI code (runs in main thread)
class DeviceMonitorGUI:
    def __init__(self, root):
        self.root = root
        root.title("Device Monitor")

        # Top frame: device list
        frm = ttk.Frame(root)
        frm.pack(side=tk.LEFT, fill=tk.Y, padx=6, pady=6)

        # Add device name column
        self.tree = ttk.Treeview(frm, columns=("device", "last_ms", "rows"), show="headings", height=20)
        self.tree.heading("device", text="Device")
        self.tree.heading("last_ms", text="ms since last check-in")
        self.tree.heading("rows", text="rows received")
        self.tree.column("device", width=140, anchor=tk.CENTER)
        self.tree.column("last_ms", width=140, anchor=tk.CENTER)
        self.tree.column("rows", width=100, anchor=tk.CENTER)
        self.tree.pack(fill=tk.Y, expand=True)
        self.tree.bind("<<TreeviewSelect>>", self.on_select)

        # Right frame: plot
        plot_frm = ttk.Frame(root)
        plot_frm.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True, padx=6, pady=6)

        self.fig = Figure(figsize=(6,4))
        self.ax = self.fig.add_subplot(111)
        self.ax.set_xlabel("time (s)")
        self.ax.set_ylabel("norm(x,y,z)")
        self.line, = self.ax.plot([], [], "-b")
        self.canvas = FigureCanvasTkAgg(self.fig, master=plot_frm)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        # selected device
        self.selected = None

        # schedule updates
        self.update_interval_ms = 200
        self.notification_poll_ms = 100   # poll notify_queue this often
        self._schedule_update()
        self._schedule_notification_poll()

    def _schedule_update(self):
        self.root.after(self.update_interval_ms, self._update)

    def _schedule_notification_poll(self):
        self.root.after(self.notification_poll_ms, self._poll_notifications)

    def _poll_notifications(self):
        """
        Process any notifications from the server thread. If the selected device
        was updated, redraw the plot immediately.
        """
        updated_selected = False
        while True:
            try:
                name = notify_queue.get_nowait()
            except queue.Empty:
                break
            # if the notification mentions our selected device, mark to refresh
            if name == self.selected:
                updated_selected = True
            # also ensure the device is present in the tree (tree refresher will repopulate soon)
        if updated_selected:
            # redraw plot for selected device immediately
            self._update_plot_for_selected()
        # reschedule poll
        self._schedule_notification_poll()

    def _update(self):
        # refresh tree and plot periodically
        now_ms = int(time.time() * 1000)
        # remember current selection (device name iid)
        prev_selected = self.selected

        with devices_lock:
            names = sorted(devices.keys())
            # update tree items
            existing = set(self.tree.get_children())
            # clear and reinsert (preserve selection below)
            for iid in existing:
                self.tree.delete(iid)
            for name in names:
                dev = devices[name]
                last = now_ms - int(dev.get('last_checkin', now_ms))
                rows = dev.get('rows_received', 0)
                # include device name as a visible column value
                self.tree.insert("", "end", iid=name, values=(name, last, rows))

            # if selected device was removed, clear selection state
            if prev_selected and prev_selected not in devices:
                self.selected = None

        # restore Treeview selection if possible
        if prev_selected and prev_selected in names:
            try:
                self.tree.selection_set(prev_selected)
            except Exception:
                # ignore if selection_set fails for any reason
                pass
            # ensure internal selected matches restored selection
            self.selected = prev_selected

        # redraw plot (for selected) as part of periodic update
        self._update_plot_for_selected()

        self._schedule_update()

    def _update_plot_for_selected(self):
        # Grab data for the selected device and redraw plot
        with devices_lock:
            if self.selected and self.selected in devices:
                dev = devices[self.selected]
                times = list(dev['times'])
                norms = list(dev['norms'])
            else:
                times = []
                norms = []

        # redraw plot (show only last 5 seconds)
        self.ax.clear()
        self.ax.set_xlabel("time (s)")
        self.ax.set_ylabel("norm(x,y,z)")
        if times and norms:
            latest = times[-1]
            cutoff = latest - 5.0
            # find first index where time >= cutoff
            start_idx = 0
            while start_idx < len(times) and times[start_idx] < cutoff:
                start_idx += 1
            plot_times = times[start_idx:]
            plot_norms = norms[start_idx:]
            if plot_times and plot_norms:
                self.ax.plot(plot_times, plot_norms, "-b")
                # set x limits to the 5s window (or a small margin if fewer samples)
                left = cutoff if plot_times[0] <= cutoff else plot_times[0]
                right = latest
                self.ax.set_xlim(left, right)
                self.ax.set_ylim(0)
                self.ax.relim()
                self.ax.autoscale_view()
                self.ax.grid(True)
            else:
                self.ax.text(0.5, 0.5, "No data", ha="center", va="center", transform=self.ax.transAxes)
        else:
            self.ax.text(0.5, 0.5, "No data", ha="center", va="center", transform=self.ax.transAxes)
        self.canvas.draw()

    def on_select(self, event):
        sel = self.tree.selection()
        if sel:
            self.selected = sel[0]
            # immediate plot refresh when user selects a device
            self._update_plot_for_selected()
        else:
            self.selected = None

def start_server_in_thread():
    t = threading.Thread(target=lambda: asyncio.run(main_server()), daemon=True)
    t.start()
    return t

if __name__ == "__main__":
    # start asyncio server in background
    start_server_in_thread()

    # start GUI in main thread
    root = tk.Tk()
    gui = DeviceMonitorGUI(root)
    root.mainloop()