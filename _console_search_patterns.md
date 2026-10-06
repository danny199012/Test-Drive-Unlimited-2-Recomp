# Real-console capture plan (RGH + MemoryEngine360)

Goal: capture ground-truth data from the real Xbox 360 while the party scene is
on screen, so we can diff it against the emulator's version of the same draws.

## Console-side requirements

- RGH/JTAG with JRPC2 dash launch plugin (MemoryEngine360 connects over JRPC),
  or a debug kernel with XBDM.
- The game running, idle at the party scene with corrupted characters visible.
- IMPORTANT: read-only! Search and dump only — do not write/edit memory.

## Search 1 — bone palette ring (the data we believe is correct)

Search memory for this byte pattern (float32 -1.0, -1.0, 1.0 — little-endian):

    BF 80 00 00 BF 80 00 00 3F 80 00 00

When found, export/dump ~256 KB around each hit (a handful of hits expected —
these are the per-character bone palettes). Note the addresses of the hits.

## Search 2 — vertex shader microcode (the prime suspect)

Each character shader's microcode lives in RAM as these dword sequences. Search
for either byte order of the first two dwords (console RAM is big-endian; our
values are the guest-loaded view):

| shader (ucode hash) | dword 0   | dword 1   | AoB little-endian     | AoB big-endian        |
|---------------------|-----------|-----------|-----------------------|-----------------------|
| 5D85701B1D8337A1    | F2556004  | 600A1200  | 04 60 55 F2 00 12 0A 60 | F2 55 60 04 60 0A 12 00 |
| 74B008B1794E7CFC    | 10011003  | 00001200  | 03 10 01 10 00 12 00 00 | 10 01 10 03 00 00 12 00 |
| 9D50FF50FE808986    | 10096005  | 600B1200  | 05 60 09 10 00 12 0B 60 | 10 09 60 05 60 0B 12 00 |
| 67DBC61DCB8324F2    | 30052003  | 00001200  | 03 20 05 30 00 12 00 00 | 30 05 20 03 00 00 12 00 |
| 9BB940898382FF8D    | 10011003  | 00001200  | (same as 74B008B1)      | (same as 74B008B1)      |
| D084E6C4FBA48D12    | 30256004  | 600A1200  | 04 60 25 30 00 12 0A 60 | 30 25 60 04 60 0A 12 00 |
| 110FB972C476FFD6    | 30052003  | 00001200  | (same as 67DBC61D)      | (same as 67DBC61D)      |

When found, export ~16 KB starting at the hit (vertex shader ucode is a few KB).

## Also capture (if the tool allows quick region saves)

- 1 MB around the palette hits from Search 1.
- Any region around the fetch-constant area if visible: search for the dword
  pair `00 0B C1 20`-style... skip if not obvious; palettes + ucode are the
  priority.

## Deliverables

Drop the exported .bin files (with their addresses noted) into:
`E:\Xbox 360 Games\Torrent\Minerva_Myrient\Redump\Microsoft - Xbox 360\Rex Glue TDU 2 Project\real360_capture\`

Then I will:
1. Disassemble the real console's copy of each vertex shader with the SDK's own
   disassembler and compare it against our translated version — any translation
   divergence is the bug.
2. Diff the real palette layout (entry size, count, stride) against what the
   emulator fetches.
3. Compare the real shader's referenced fetch constant usage against ours.
