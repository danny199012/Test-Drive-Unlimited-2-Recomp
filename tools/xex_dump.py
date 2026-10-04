#!/usr/bin/env python3
"""Minimal XEX -> PE image extractor (XEX1/XEX2/XEX3), then PE -> flat .text bytes.

Used to disassemble guest PPC regions straight out of the redump XEX while
bringing up a ReXGlue recomp, without needing a full XISO/PIRS unpack.

Usage:
    python tools/xex_dump.py Default.xex
    python tools/xex_dump.py Default.xex --range 0x82022B80:0x82022C60
"""
import argparse
import struct
import sys

XEX_MAGIC = {
    b"XEX1": (1, 32),
    b"XEX2": (2, 32),
    b"XEX3": (3, 32),
    b"XEX1.5": (1, 32),
}


def parse_xex_header(data):
    magic = data[:4]
    if magic not in XEX_MAGIC:
        raise SystemExit(f"not a XEX: magic={magic!r}")
    version = XEX_MAGIC[magic][0]
    module_count, _ = struct.unpack_from("<II", data, 4)
    base = version == 1 and 4 or 8
    return version, module_count, base


def parse_xex_headers(data):
    """Yield (key, offset, size) for each encryption/header entry."""
    version, module_count, _ = parse_xex_header(data)
    if version == 1:
        return []

    magic_offset = 8
    # XEX2/3: module_count at 4, then the header table of key/size/offset triples.
    entries = []
    off = magic_offset
    # The region table starts right after the magic + module count for XEX2.
    for _ in range(module_count):
        key, size, offset = struct.unpack_from("<III", data, off)
        entries.append((key, size, offset))
        off += 12
        if size == 0:
            break
    return entries


def parse_pe_sections(pe):
    if pe[:2] != b"MZ":
        raise SystemExit("not an MZ image")
    e_lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    if pe[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise SystemExit("bad PE signature")
    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", pe, coff + 2)[0]
    opt_size = struct.unpack_from("<H", pe, coff + 16)[0]
    sec = coff + 20 + opt_size

    image_base = struct.unpack_from("<I", pe, coff + 20 + 28)[0]
    sections = []
    for i in range(num_sections):
        o = sec + i * 40
        name = pe[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", pe, o + 8)
        sections.append(
            {"name": name, "vaddr": vaddr, "vsize": vsize,
             "rawptr": rawptr, "rawsize": rawsize})
    return image_base, sections


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xex")
    ap.add_argument("--range", dest="rng",
                    help="ADDR:ADDR guest virtual range to dump (hex, 0x82xxxxxx)")
    ap.add_argument("--out", help="write the flat PE image here")
    args = ap.parse_args()

    data = open(args.xex, "rb").read()

    pe = None
    for key, size, offset in parse_xex_headers(data):
        if key == 0x00000001 and offset + size <= len(data):
            pe = data[offset:offset + size]
            print(f"# XEX header key=0x{key:08X} size={size} "
                  f"(0x{size:#x}) at 0x{offset:X}", file=sys.stderr)
            break

    if pe is None:
        # Some XEXs embed the PE directly without the XDATA encryption header.
        mz = data.find(b"MZ")
        if mz < 0:
            raise SystemExit("no XEX2 XDATA header and no MZ found")
        pe = data[mz:]
        print(f"# no XDATA header; raw MZ at 0x{mz:X}", file=sys.stderr)

    if args.out:
        open(args.out, "wb").write(pe)
        print(f"# wrote {args.out} ({len(pe)} bytes)", file=sys.stderr)

    image_base, sections = parse_pe_sections(pe)
    print(f"# image_base=0x{image_base:08X}", file=sys.stderr)
    for s in sections:
        print(f"# {s['name']:<8} vaddr=0x{s['vaddr']:08X} vsize=0x{s['vsize']:X} "
              f"raw=0x{s['rawptr']:X} rawsize=0x{s['rawsize']:X}", file=sys.stderr)

    if args.rng:
        start, end = (int(x, 16) for x in args.rng.split(":"))
        rva_start = start - image_base
        rva_end = end - image_base
        for s in sections:
            lo = s["vaddr"]
            hi = lo + max(s["vsize"], s["rawsize"])
            if rva_start >= hi or rva_end <= lo:
                continue
            a = max(rva_start, lo) - lo + s["rawptr"]
            b = min(rva_end, hi) - lo + s["rawptr"]
            sys.stdout.buffer.write(pe[a:b])


if __name__ == "__main__":
    main()