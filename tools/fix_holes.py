#!/usr/bin/env python3
"""Drive the pin -> codegen -> build -> run loop until the game stops crashing.

The ReXGlue recompiler misses a scattering of thin thunk functions, and the
only reliable way to find each one is to run the game and read the address out
of the [FATAL] line. That is slow by hand: each round is a ~4 min codegen of
the 16 MB DLL plus a ~1 min link, and the crash you are chasing may not even be
the next one.

This automates that loop:

    1. launch the game headless for --seconds
    2. read the newest [FATAL] guest address from its log
    3. if already registered, stop and say why (pinning will not help)
    4. otherwise append a pinned entry to tdu2_manifest.toml
    5. run codegen, then build, then go back to 1

Each round is one codegen + one link, the minimum possible. It stops on
--rounds, on a clean run, or when a pinned address still faults.

Usage:
    python tools/fix_holes.py --rounds 5
    python tools/fix_holes.py --dry-run     # report only, change nothing
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import time

FATAL_RE = re.compile(
    r"Call to invalid or unregistered function at guest address 0x([0-9A-Fa-f]+)")
REGISTER_RE = re.compile(r"SetFunction\(0x([0-9A-Fa-f]+),")
PIN_RE = re.compile(r'^\[modules\.functions\."0x([0-9A-Fa-f]+)"\]')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = os.path.join(ROOT, "tdu2_manifest.toml")
REXGLOUE = os.path.join(ROOT, "rexglue", "win-amd64", "bin", "rexglue.exe")


def pinned_addresses():
    """Addresses already pinned in the manifest, so we never duplicate one."""
    out = set()
    if not os.path.exists(MANIFEST):
        return out
    with open(MANIFEST, "r", encoding="utf-8") as fh:
        for line in fh:
            m = PIN_RE.match(line.strip())
            if m:
                out.add(int(m.group(1), 16))
    return out


def registered_addresses():
    addrs = set()
    for path in glob.glob(os.path.join(ROOT, "generated", "**", "*register*.cpp"),
                          recursive=True):
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = REGISTER_RE.search(line)
                if m:
                    addrs.add(int(m.group(1), 16))
    return addrs


def newest_log(config):
    logs = glob.glob(os.path.join(ROOT, "out", "build", config, "logs", "*.log"))
    return max(logs, key=os.path.getmtime) if logs else None


def run_and_get_fatal(config, game_root, seconds):
    """Launch the game headless; return the FATAL address, or None if clean."""
    build_dir = os.path.join(ROOT, "out", "build", config)
    exe = os.path.join(build_dir, "tdu2.exe")
    if not os.path.exists(exe):
        sys.exit(f"{exe} not found - build first (build.cmd -perf tdu2)")

    log_dir = os.path.join(build_dir, "logs")
    os.makedirs(log_dir, exist_ok=True)
    for stale in glob.glob(os.path.join(log_dir, "*.log")):
        os.remove(stale)

    proc = subprocess.Popen(
        [exe, "--game_data_root", game_root, "--headless"],
        cwd=build_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    deadline = time.time() + seconds
    addr = None
    while time.time() < deadline:
        if proc.poll() is not None:
            break
        # Crash out as soon as a FATAL lands rather than idling the full
        # window - otherwise every round costs the full --seconds.
        path = newest_log(config)
        if path:
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as fh:
                    hits = FATAL_RE.findall(fh.read())
                if hits:
                    addr = int(hits[-1], 16)
                    break
            except OSError:
                pass
        time.sleep(1)

    if proc.poll() is None:
        proc.kill()
        proc.wait()

    # Always re-read the log once the process is gone. When the game crashes
    # it exits almost immediately, so the polling loop above usually breaks on
    # proc.poll() before it ever sees the FATAL line - without this final read
    # every crash would be misreported as "the run stayed clean".
    path = newest_log(config)
    if path:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                hits = FATAL_RE.findall(fh.read())
            if hits:
                addr = int(hits[-1], 16)
        except OSError:
            pass
    return addr


def pin(address, dry_run):
    entry = ("\n# auto-pinned by tools/fix_holes.py"
             "\n[modules.functions.\"0x%08X\"]\nname = \"sub_%08X\"\n"
             % (address, address))
    if dry_run:
        print("[dry-run] would append:\n" + entry)
        return True
    with open(MANIFEST, "a", encoding="utf-8") as fh:
        fh.write(entry)
    print(f"[pinned] 0x{address:08X}")
    return True


def sh(args, label):
    print(f"--- {label} ---", flush=True)
    res = subprocess.run(args, cwd=ROOT, shell=False,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for line in res.stdout.decode("utf-8", "replace").strip().splitlines()[-5:]:
        print("   " + line)
    return res.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=5,
                    help="max pin/build/run cycles (default 5)")
    ap.add_argument("--seconds", type=int, default=300,
                    help="max seconds to let each run live (default 300)")
    ap.add_argument("--config", default="win-amd64-perf",
                    help="build dir to run (default win-amd64-perf)")
    ap.add_argument("--game-root", default=os.path.join(ROOT, "TDU2"),
                    help="game content dir (default ./TDU2)")
    ap.add_argument("--dry-run", action="store_true",
                    help="report the next address but change nothing")
    args = ap.parse_args()

    if not os.path.exists(REXGLOUE):
        sys.exit(f"rexglue not found at {REXGLOUE}")

    for rnd in range(1, args.rounds + 1):
        print(f"\n=========== round {rnd}/{args.rounds} ===========", flush=True)
        addr = run_and_get_fatal(args.config, args.game_root, args.seconds)

        if addr is None:
            print("No FATAL - the run stayed clean. Done!")
            return 0

        print(f"[FATAL] guest address 0x{addr:08X}")

        if addr in pinned_addresses():
            reg = registered_addresses()
            if addr not in reg:
                # Pinned in the manifest but not in the generated table: the
                # edit landed after the last codegen. Build it in and retry
                # instead of treating it as an unfixable crash.
                print(f"0x{addr:08X} is pinned but not generated yet - "
                      "running codegen + build to catch up.")
                if args.dry_run:
                    return 0
                if sh([REXGLOUE, "codegen", MANIFEST], "codegen") != 0:
                    print("codegen FAILED")
                    return 1
                if sh(["cmd", "/c", os.path.join(ROOT, "build.cmd"), "-perf",
                       "tdu2"], "build") != 0:
                    print("build FAILED")
                    return 1
                continue
            print(f"0x{addr:08X} is pinned AND generated but still faulting.")
            print("Pinning is not the fix here - inspect it by hand:")
            print(f"    python tools/check_func.py 0x{addr:08X}")
            return 1

        reg = registered_addresses()
        if addr in reg:
            print(f"0x{addr:08X} IS in the generated table.")
            print("So the table has it but dispatch missed it - not a scanner")
            print("gap; look at how the guest reached the address.")
            return 1

        below = max((a for a in reg if a < addr), default=None)
        above = min((a for a in reg if a > addr), default=None)
        print(f"  hole: prev={below and hex(below)} next={above and hex(above)}")

        if not pin(addr, args.dry_run):
            return 1
        if args.dry_run:
            return 0

        if sh([REXGLOUE, "codegen", MANIFEST], "codegen") != 0:
            print("codegen FAILED")
            return 1
        if sh(["cmd", "/c", os.path.join(ROOT, "build.cmd"), "-perf", "tdu2"],
              "build") != 0:
            print("build FAILED")
            return 1

    print(f"\nReached --rounds limit ({args.rounds}). Run again to continue.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
