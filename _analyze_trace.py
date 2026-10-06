#!/usr/bin/env python3
"""Correlate palette vfetch requests with shared-memory uploads/invalidations.

Reads a _trace_*.log snapshot produced by _run_memexport_trace.ps1.

For each "VFetch request ... buffer N 0xADDR (SIZE bytes)" line, tracks the
page coverage state (uploaded / invalidated) of the 4KB pages covering the
range, and classifies the request:
  fresh    - every page in the range had an upload after its last invalidation
             (or was never uploaded but invalidated = will upload now)
  stale    - some page's last event was an upload with NO invalidation after
             it -> the GPU reads data uploaded at that earlier time; if the
             CPU rewrote the data since (which for a watched page is
             impossible without a fault), the draw sees the old bytes.
Uploads and invalidations update the state as the log interleaves.
"""
import re
import sys
from datetime import datetime

PAGE = 4096

log = sys.argv[1]
scene_start = None
if len(sys.argv) > 1:
    pass

req_re = re.compile(
    r"\[(?P<ts>[0-9: .-]+)\].*VFetch request: VS ucode (?P<ucode>[0-9A-F]+) buffer "
    r"(?P<buf>\d+) 0x(?P<addr>[0-9A-F]+) \((?P<size>\d+) bytes\)")
up_re = re.compile(
    r"\[(?P<ts>[0-9: .-]+)\].*SharedMemory upload: pages (?P<first>\d+)\.\.(?P<last>\d+) "
    r"\(0x(?P<addr>[0-9A-F]+), (?P<size>\d+) bytes\)")
inv_re = re.compile(
    r"\[(?P<ts>[0-9: .-]+)\].*SharedMemory invalidate: CPU wrote 0x(?P<addr>[0-9A-F]+) "
    r"\((?P<size>\d+) bytes.*-> pages (?P<first>\d+)\.\.(?P<last>\d+)")

def ts(s):
    return datetime.strptime(s.strip(), "%Y-%m-%d %H:%M:%S.%f")

# page index -> ("upload"|"invalidate", timestamp)
state = {}
events = []  # (timestamp, kind, payload)

for line in open(log, encoding="utf-8", errors="replace"):
    m = req_re.search(line)
    if m:
        events.append((ts(m["ts"]), "req", (int(m["buf"]), int(m["addr"], 16), int(m["size"]),
                                            m["ucode"])))
        continue
    m = up_re.search(line)
    if m:
        events.append((ts(m["ts"]), "up",
                       (int(m["first"]), int(m["last"]), int(m["addr"], 16), int(m["size"]))))
        continue
    m = inv_re.search(line)
    if m:
        events.append((ts(m["ts"]), "inv", (int(m["first"]), int(m["last"]),
                                            int(m["addr"], 16), int(m["size"]))))

events.sort(key=lambda e: e[0])
print(f"{len(events)} events")

stale_by_buf = {}
fresh_by_buf = {}
stale_samples = []
window_start = None
window_end = None
# Focus on the last 90 seconds of the run (party scene is near the end).
if events:
    window_start = events[0][0]
    window_end = events[-1][0]

for t, kind, payload in events:
    if kind == "up":
        for p in range(payload[0], payload[1] + 1):
            state[p] = ("up", t)
    elif kind == "inv":
        for p in range(payload[0], payload[1] + 1):
            state[p] = ("inv", t)
    else:
        buf, addr, size, ucode = payload
        p_first = addr // PAGE
        p_last = (addr + size - 1) // PAGE
        is_stale = False
        worst = None
        for p in range(p_first, p_last + 1):
            st = state.get(p)
            if st and st[0] == "up":
                is_stale = True
                if worst is None or st[1] > worst[1]:
                    worst = st
        if is_stale:
            stale_by_buf[buf] = stale_by_buf.get(buf, 0) + 1
            if len(stale_samples) < 25:
                stale_samples.append((t, buf, addr, size, ucode, worst[1] if worst else None))
        else:
            fresh_by_buf[buf] = fresh_by_buf.get(buf, 0) + 1

print("\nrequests by buffer: stale / fresh")
for buf in sorted(set(stale_by_buf) | set(fresh_by_buf)):
    print(f"  buffer {buf}: stale={stale_by_buf.get(buf, 0)} fresh={fresh_by_buf.get(buf, 0)}")

print("\nfirst stale samples (time, buffer, addr, size, ucode, upload-time):")
for s in stale_samples:
    print(" ", s)

# frame pacing: inter-arrival of requests as a rough GPU-thread pulse
print("\nrequest burst gaps > 0.5s (potential stalls):")
prev = None
for t, kind, payload in events:
    if kind != "req":
        continue
    if prev is not None:
        dt = (t - prev).total_seconds()
        if dt > 0.5:
            print(f"  gap {dt:6.2f}s ending at {t.time()}")
    prev = t
