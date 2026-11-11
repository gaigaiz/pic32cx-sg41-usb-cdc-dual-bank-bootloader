# -*- coding: utf-8 -*-
"""
Generate APP / BL / merged Flash images for the PIC32CX1025SG41128 USB-CDC bootloader.

Single-bank (Bank0) physical layout; for merge.bin the file offset equals the
Flash address:
  bootloader   0x000000 .. 0x003FFF   (BL_SIZE = 16 KB)
  APP          0x004000 .. 0x07DFFF   (APP_REGION_SIZE = 0x7A000)
  failsafe     0x07E000 .. 0x07FFFF
  config bits  0x00804000+            (EXCLUDED; this flow does not program config bits)

Products (--target)
-------------------
app      compact app.bin: file offset 0 == Flash 0x4000. Used by:
         python btl_host.py -i COMx -d pic32cx_sg41 -a 0x4000 -f merged_out/app.bin
bl       fixed-length bl.bin (padded to BL_SIZE=0x4000); file offset 0 == Flash 0
merged   merge.bin: bank-relative combined image (BL_B @offset 0x0000 slot,
         APP_B @offset 0x4000), rest 0xFF; Bank A archive / whole-chip layout
bankb    bankb_merged.bin: same bank-relative layout as merge.bin, but this file
         is meant for the inactive Bank B download at physical base 0x80000:
         python btl_host.py -i COMx -d pic32cx_sg41 -a 0x80000 -f merged_out/bankb_merged.bin -s
all      bl.bin + app.bin + merge.bin + bankb_merged.bin

Vector safety checks
---------------------
app.bin must hold a Thumb Reset entry inside [0x4000, 0x7E000); bl.bin inside
[0, 0x4000). A wrong link base (e.g. legacy 0x2000) aborts instead of emitting a
run-away image.
"""

import argparse
import os
import struct

# Flash partition facts; must match BL bootloader_common.h / APP linker setup.
BL_SIZE = 0x00004000                         # bootloader slot 0x0000 .. 0x3FFF (16 KB)
APP_START = 0x00004000
APP_REGION_SIZE = 0x0007A000                 # 0x4000 .. 0x7DFFF
APP_REGION_END = APP_START + APP_REGION_SIZE  # 0x7E000 (failsafe sector starts here)
ERASE_BLOCK = 0x00002000                      # NVMCTRL 8 KB erase block
INACTIVE_BANK_START = 0x00080000              # inactive Bank B physical base
FLASH_END = 0x00100000                        # total 1 MB Flash
BANK_FAILSAFE_OFFSET = 0x0007E000             # per-bank failsafe sector offset

# Project root = parent of this Tools/ directory (portable, no hard-coded local path).
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BL_HEX = os.path.join(ROOT, "BL", "out", "BL", "default.hex")
DEFAULT_APP_HEX = os.path.join(ROOT, "APP", "out", "APP", "default.hex")
OUT_DIR = os.path.join(ROOT, "Tools", "merged_out")
DEFAULT_BL_OUT = os.path.join(OUT_DIR, "bl.bin")
DEFAULT_APP_OUT = os.path.join(OUT_DIR, "app.bin")
DEFAULT_MERGED_OUT = os.path.join(OUT_DIR, "merge.bin")
DEFAULT_BANKB_OUT = os.path.join(OUT_DIR, "bankb_merged.bin")


def parse_hex(hex_path, lo, hi):
    """Return {absolute_flash_address: byte} for type-00 records in [lo, hi)."""
    image = {}
    ext_hi = 0
    with open(hex_path, "r") as fh:
        for raw in fh:
            line = raw.strip()
            if not line.startswith(":"):
                continue
            n = int(line[1:3], 16)
            addr = int(line[3:7], 16)
            typ = int(line[7:9], 16)
            data = bytes.fromhex(line[9:9 + 2 * n])
            if typ == 0x04:                    # Extended Linear Address (upper 16b)
                ext_hi = int(line[9:13], 16) << 16
                continue
            if typ != 0x00:
                continue
            for i, b in enumerate(data):
                abs_addr = ext_hi + addr + i
                if lo <= abs_addr < hi:
                    image[abs_addr] = b
    return image


def compact(image, lo, hi, pad=0xFF):
    """Compact bytes for [lo, hi); missing addresses become pad bytes."""
    return bytes(image.get(a, pad) for a in range(lo, hi))


def head8(buf, off=0):
    return " ".join("%02X" % b for b in buf[off:off + 8])


def build_bl(hex_path, out_path):
    if not os.path.isfile(hex_path):
        raise SystemExit("ERROR: BL hex not found: %s" % hex_path)

    img = parse_hex(hex_path, 0, BL_SIZE)
    if not img:
        raise SystemExit("ERROR: no BL records in [0, 0x%X)." % BL_SIZE)

    lo, hi = min(img), max(img) + 1
    if lo != 0:
        raise SystemExit("ERROR: BL program starts at 0x%X, expected 0x0." % lo)
    if hi > BL_SIZE:
        raise SystemExit("ERROR: BL ends at 0x%X, exceeds BL_SIZE 0x%X." % (hi, BL_SIZE))

    prog = bytearray([0xFF]) * BL_SIZE
    prog[0:hi] = compact(img, 0, hi)

    _, reset_raw = struct.unpack("<II", prog[0:8])
    entry = reset_raw & ~1
    thumb_ok = (reset_raw & 1) == 1
    pc_ok = 0 <= entry < BL_SIZE

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(prog)

    print("== BL image (fixed %d bytes = 0x%X) ==" % (BL_SIZE, BL_SIZE))
    print("  source hex    : %s" % hex_path)
    print("  program range : 0x00000000 ~ 0x%08X (%d bytes)" % (hi, hi))
    print("  output bin    : %s (%d bytes)" % (out_path, len(prog)))
    print("  vector head   : %s" % head8(prog, 0))
    print("  Reset entry   : raw=0x%08X entry=0x%08X  thumb=%s in_BL=%s"
          % (reset_raw, entry, thumb_ok, pc_ok))
    if not (thumb_ok and pc_ok):
        raise SystemExit("ERROR: BL Reset vector invalid.")
    return bytes(prog)


def build_app(hex_path, out_path):
    if not os.path.isfile(hex_path):
        raise SystemExit("ERROR: APP hex not found: %s" % hex_path)

    img = parse_hex(hex_path, APP_START, APP_REGION_END)
    if not img:
        raise SystemExit(
            "ERROR: no APP records in [0x%X, 0x%X)." % (APP_START, APP_REGION_END))

    lo, hi = min(img), max(img) + 1
    if lo != APP_START:
        raise SystemExit(
            "ERROR: APP starts at 0x%X, expected 0x%X. Fix APP ROM_ORIGIN / MPLAB "
            "Application Start Address." % (lo, APP_START))
    if hi > APP_REGION_END:
        raise SystemExit("ERROR: APP ends at 0x%X, exceeds region end 0x%X."
                         % (hi, APP_REGION_END))

    prog = compact(img, lo, hi)
    init_sp, reset_raw = struct.unpack("<II", prog[0:8])
    entry = reset_raw & ~1
    thumb_ok = (reset_raw & 1) == 1
    pc_ok = APP_START <= entry < APP_REGION_END

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(prog)

    print("== APP compact image (file offset 0 == Flash 0x%X) ==" % APP_START)
    print("  source hex    : %s" % hex_path)
    print("  program range : 0x%08X ~ 0x%08X (%d bytes)" % (lo, hi, len(prog)))
    print("  output bin     : %s (%d bytes)" % (out_path, len(prog)))
    print("  vector head    : %s" % head8(prog, 0))
    print("  initial SP     : 0x%08X" % init_sp)
    print("  Reset entry   : raw=0x%08X entry=0x%08X  thumb=%s in_APP=%s"
          % (reset_raw, entry, thumb_ok, pc_ok))
    if not (thumb_ok and pc_ok):
        raise SystemExit(
            "ERROR: APP Reset entry 0x%08X outside APP region; do NOT download this bin."
            % entry)
    print("  check         : OK")
    return prog


def compose_combined(bl_prog, app_prog):
    """Bank-relative combined bytes: BL slot @offset 0, APP @offset APP_START,
    rest 0xFF, length aligned to the erase block. The bytes are identical for
    the Bank A archive and the inactive Bank B OTA; only the download base
    address differs (0x00000000 vs 0x00080000)."""
    total = (APP_START + len(app_prog) + ERASE_BLOCK - 1) & ~(ERASE_BLOCK - 1)
    merged = bytearray([0xFF]) * total
    merged[0:BL_SIZE] = bl_prog
    merged[APP_START:APP_START + len(app_prog)] = app_prog
    return merged


def _write_bin(out_path, blob):
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(blob)


def build_merged(bl_prog, app_prog, out_path):
    merged = compose_combined(bl_prog, app_prog)
    _write_bin(out_path, merged)

    print("== Merged bank-relative image (Bank A archive / whole-chip layout) ==")
    print("  output bin    : %s (%d bytes = 0x%X)" % (out_path, len(merged), len(merged)))
    print("  BL head @0x0000 : %s" % head8(merged, 0))
    print("  APP head @0x%04X : %s" % (APP_START, head8(merged, APP_START)))
    print("  layout        : BL 0x0000..0x3FFF; APP 0x4000..; erased area 0xFF")


def build_bankb_merged(bl_prog, app_prog, out_path):
    """Inactive Bank B combined image; download base = INACTIVE_BANK_START 0x80000."""
    merged = compose_combined(bl_prog, app_prog)

    bl_phys = INACTIVE_BANK_START
    app_phys = INACTIVE_BANK_START + APP_START
    app_end = app_phys + len(app_prog)
    failsafe_b_phys = INACTIVE_BANK_START + BANK_FAILSAFE_OFFSET

    if bl_phys + BL_SIZE > app_phys:
        raise SystemExit("ERROR: BL_B slot overlaps APP_B region.")
    if app_end > failsafe_b_phys:
        raise SystemExit("ERROR: APP_B ends at 0x%X, exceeds failsafe_B 0x%X."
                         % (app_end, failsafe_b_phys))
    if INACTIVE_BANK_START + len(merged) > FLASH_END:
        raise SystemExit("ERROR: Bank B image crosses Flash end 0x%X." % FLASH_END)

    _write_bin(out_path, merged)

    print("== Inactive Bank B combined image (download base 0x%08X) =="
          % INACTIVE_BANK_START)
    print("  output bin    : %s (%d bytes = 0x%X)" % (out_path, len(merged), len(merged)))
    print("  BL_B head @bank-off 0x0000 (phys 0x%08X): %s"
          % (bl_phys, head8(merged, 0)))
    print("  APP_B head @bank-off 0x%04X (phys 0x%08X): %s"
          % (APP_START, app_phys, head8(merged, APP_START)))
    print("  physical BL_B : 0x%08X .. 0x%08X" % (bl_phys, bl_phys + BL_SIZE - 1))
    print("  physical APP_B: 0x%08X .. 0x%08X" % (app_phys, app_end - 1))
    print("  download cmd  : python btl_host.py -i COMx -d pic32cx_sg41 "
          "-a 0x80000 -f merged_out/bankb_merged.bin -s")


def main():
    ap = argparse.ArgumentParser(description="Build BL / APP / merged Flash images.")
    ap.add_argument("--target",
                    choices=["app", "bl", "merged", "bankb", "all"],
                    default="app",
                    help="app: only app.bin (default); bl: only bl.bin; "
                         "merged: merge.bin; bankb: bankb_merged.bin (download to "
                         "0x80000 with -s); all: all four outputs")
    ap.add_argument("--bl-hex", default=DEFAULT_BL_HEX)
    ap.add_argument("--app-hex", default=DEFAULT_APP_HEX)
    ap.add_argument("--bl-out", default=DEFAULT_BL_OUT)
    ap.add_argument("--app-out", default=DEFAULT_APP_OUT)
    ap.add_argument("--merged-out", default=DEFAULT_MERGED_OUT)
    ap.add_argument("--bankb-out", default=DEFAULT_BANKB_OUT)
    a = ap.parse_args()

    need_bl = a.target in ("bl", "merged", "bankb", "all")
    need_app = a.target in ("app", "merged", "bankb", "all")

    bl_prog = build_bl(a.bl_hex, a.bl_out) if need_bl else None
    if bl_prog is not None:
        print()
    app_prog = build_app(a.app_hex, a.app_out) if need_app else None
    if app_prog is not None:
        print()

    if a.target in ("merged", "all"):
        build_merged(bl_prog, app_prog, a.merged_out)
        print()

    if a.target in ("bankb", "all"):
        build_bankb_merged(bl_prog, app_prog, a.bankb_out)


if __name__ == "__main__":
    main()
