#!/usr/bin/env python3
"""Check whether a guest address is registered in codegen's function table.

Codegen's scanner classifies some small PPC routines as non-code, leaving
holes in the registered table. When the runtime calls such an address it
fatals with:

    [FATAL] Call to invalid or unregistered function at guest address 0x...

and abort()s. Each hole is fixed by pinning the address in the manifest:

    [modules.functions."0xADDR"]
    name = "sub_ADDR"

This tool answers "is this address already registered?" and, if not, prints
the manifest stanza to paste in plus the surrounding registered neighbours
(which identify the hole).

Usage:
    python tools/check_func.py 0x88F12600
    python tools/check_func.py 0x88F12600 --module TestDrive2_DLL
    python tools/check_func.py --latest          # newest FATAL from the log
"""
import argparse
import glob
import os
import re
import sys

REGISTER_RE = re.compile(r"SetFunction\(0x([0-9A-Fa-f]+),")
FATAL_RE = re.compile(
    r"Call to invalid or unregistered function at guest address 0x([0-9A-Fa-f]+)")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def register_files(module):
    """All codegen'd files that list registered function addresses."""
    gen = os.path.join(repo_root(), "generated")
    if not os.path.isdir(gen):
        return []
    if module:
        paths = [os.path.join(gen, module, "tdu2_register.cpp")]
        return [p for p in paths if os.path.exists(p)]
    found = []
    for path in glob.glob(os.path.join(gen, "**", "*register*.cpp"), recursive=True):
        found.append(path)
    return sorted(found)


def load_addresses(paths):
    addrs = set()
    for path in paths:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = REGISTER_RE.search(line)
                if m:
                    addrs.add(int(m.group(1), 16))
    return addrs


def latest_fatal_address():
    logs = glob.glob(os.path.join(repo_root(), "out", "build", "*", "logs", "*.log"))
    if not logs:
        return None
    newest = max(logs, key=os.path.getmtime)
    with open(newest, "r", encoding="utf-8", errors="replace") as fh:
        hits = FATAL_RE.findall(fh.read())
    if not hits:
        return None
    return int(hits[-1], 16), newest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("address", nargs="?", help="guest address, e.g. 0x88F12600")
    ap.add_argument("--module", help="restrict to one module's register file")
    ap.add_argument("--latest", action="store_true",
                    help="use the newest FATAL address from the newest log")
    args = ap.parse_args()

    paths = register_files(args.module)
    if not paths:
        sys.exit("no generated register files found - run codegen first")
    addrs = load_addresses(paths)

    if args.latest or not args.address:
        latest = latest_fatal_address()
        if not latest:
            sys.exit("no 'unregistered function' FATAL found in any log")
        addr, log_path = latest
        print(f"# from {os.path.relpath(log_path, repo_root())}")
    else:
        addr = int(args.address, 16)

    print(f"# scanned {len(addrs)} registered functions across "
          f"{len(paths)} file(s)")

    if addr in addrs:
        print(f"0x{addr:08X}: ALREADY REGISTERED - nothing to pin")
        return

    print(f"0x{addr:08X}: NOT REGISTERED (hole in the function table)")
    below = sorted(a for a in addrs if a < addr)
    above = sorted(a for a in addrs if a > addr)
    if below:
        print(f"  previous: 0x{below[-1]:08X}")
    if above:
        print(f"  next:     0x{above[0]:08X}")
    print()
    print("Pin this in tdu2_manifest.toml:")
    print(f'[modules.functions."0x{addr:08X}"]')
    print(f'name = "sub_{addr:08X}"')


if __name__ == "__main__":
    main()