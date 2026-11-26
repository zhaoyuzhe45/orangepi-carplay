#!/usr/bin/env python3
"""Repack a Rockchip boot image's ramdisk without touching the kernel.

Why this exists
---------------
The USB/ADB lockdown lives in three init rc files that ship inside the boot
partition's ramdisk (KRNL header + gzip + newc cpio).  Those are plain text,
so changing them needs no compiler and no toolchain -- but it does need a
byte-exact cpio rewrite, because the ramdisk also contains device nodes,
symlinks and tens of binaries that must survive untouched.

Design rules
------------
* Only entries named on the command line are replaced.  Every other entry
  keeps its original header fields, order and payload byte for byte.
* Nothing is inferred from the source tree: the input image is the single
  source of truth, so the result depends only on (input image, patch files).
  That makes the output reproducible and the operation idempotent.
* After writing, the result is read back and every entry is compared against
  what was intended.  A silent mismatch aborts with a non-zero exit code.

Usage
-----
    python rk_bootimg.py --list  -i boot.img
    python rk_bootimg.py --extract DIR -i boot.img
    python rk_bootimg.py -i boot.img -o new-boot.img --patch-dir patches/
    python rk_bootimg.py -i boot.img -o new-boot.img --patch-dir patches/ --dry-run

With --patch-dir, every regular file in the directory replaces the ramdisk
entry of the same name.  ``adb_keys`` is allowed to be new; everything else
must already exist, because inventing an init file would mean guessing what
init should have done.
"""

from __future__ import annotations

import argparse
import gzip
import pathlib
import struct
import sys
import zlib

KRNL_MAGIC = b"KRNL"
CPIO_MAGIC = b"070701"
CPIO_TRAILER = b"TRAILER!!!"
HDR_LEN = 110
NFIELDS = 13

# newc header field order, as offsets into the 8-hex-digit table.
F_INO, F_MODE, F_UID, F_GID, F_NLINK, F_MTIME, F_SIZE = 0, 1, 2, 3, 4, 5, 6
F_NAMESIZE = 11

# Entries that may be introduced by --patch-dir when absent from the ramdisk.
ALLOWED_NEW = {"adb_keys"}

# Ramdisk files are read by init, which does not tolerate a stray CR.
TEXT_ENTRIES = {
    "adb_keys",
    "default.prop",
    "fstab.rk30board",
    "init.rc",
    "init.usb.rc",
    "init.usb.configfs.rc",
    "init.rk30board.usb.rc",
    "init.rk30board.rc",
    "init.rockchip.rc",
    "init.rk3399.rc",
    "init.connectivity.rc",
    "init.environ.rc",
    "init.zygote32.rc",
    "init.zygote64_32.rc",
}


def align4(n: int) -> int:
    return (n + 3) // 4 * 4


class Entry:
    """One cpio member, keeping its original header fields verbatim."""

    __slots__ = ("fields", "name", "data")

    def __init__(self, fields: list[int], name: str, data: bytes) -> None:
        self.fields = fields
        self.name = name
        self.data = data

    @property
    def mode(self) -> int:
        return self.fields[F_MODE]

    def __repr__(self) -> str:  # pragma: no cover - debugging aid
        return f"<{self.name} mode={oct(self.mode)} size={len(self.data)}>"


def parse_cpio(buf: bytes) -> list[Entry]:
    entries: list[Entry] = []
    off = 0
    while True:
        if off + HDR_LEN > len(buf):
            raise SystemExit(f"cpio: truncated header at offset {off}")
        if buf[off : off + 6] != CPIO_MAGIC:
            raise SystemExit(
                f"cpio: bad magic at offset {off}: {buf[off:off + 6]!r}"
            )
        fields = [
            int(buf[off + 6 + i * 8 : off + 6 + (i + 1) * 8], 16)
            for i in range(NFIELDS)
        ]
        namesize = fields[F_NAMESIZE]
        size = fields[F_SIZE]
        name_bytes = buf[off + HDR_LEN : off + HDR_LEN + namesize - 1]
        name = name_bytes.decode("utf-8", "surrogateescape")
        data_off = off + align4(HDR_LEN + namesize)
        data = buf[data_off : data_off + size]
        if len(data) != size:
            raise SystemExit(f"cpio: truncated payload for {name!r}")
        entries.append(Entry(fields, name, data))
        off = data_off + align4(size)
        if name == CPIO_TRAILER.decode():
            break
    return entries


def build_header(fields: list[int], namesize: int, filesize: int) -> bytes:
    f = list(fields)
    f[F_SIZE] = filesize
    f[F_NAMESIZE] = namesize
    return CPIO_MAGIC + b"".join(b"%08x" % v for v in f)


def serialize_cpio(entries: list[Entry]) -> bytes:
    out = bytearray()
    for e in entries:
        name = e.name.encode("utf-8", "surrogateescape") + b"\0"
        out += build_header(e.fields, len(name), len(e.data))
        out += name
        out += b"\0" * (align4(HDR_LEN + len(name)) - (HDR_LEN + len(name)))
        out += e.data
        out += b"\0" * (align4(len(e.data)) - len(e.data))
    # The stock image pads the archive to a 512-byte boundary after the
    # TRAILER member.  Matching that keeps an unpatched repack byte-identical,
    # which is what makes --check-roundtrip a real guarantee.
    if len(out) % 512:
        out += b"\0" * (512 - len(out) % 512)
    return bytes(out)


def gunzip(payload: bytes) -> bytes:
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    out = d.decompress(payload)
    out += d.flush()
    return out


def gzip_like_stock(data: bytes) -> bytes:
    """gzip with a header identical in shape to the stock image (mtime 0)."""
    co = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
    body = co.compress(data) + co.flush()
    header = b"\x1f\x8b\x08\x00" + struct.pack("<I", 0) + b"\x00\x03"
    trailer = struct.pack("<II", zlib.crc32(data) & 0xFFFFFFFF, len(data) & 0xFFFFFFFF)
    return header + body + trailer


def read_image(path: pathlib.Path) -> tuple[int, bytes, bytes]:
    raw = path.read_bytes()
    if raw[:4] != KRNL_MAGIC:
        raise SystemExit(f"{path}: not a KRNL image (magic {raw[:4]!r})")
    plen = struct.unpack("<I", raw[4:8])[0]
    payload = raw[8 : 8 + plen]
    if len(payload) != plen:
        raise SystemExit(f"{path}: payload truncated ({len(payload)} of {plen})")
    return plen, payload, raw


def load_patches(patch_dir: pathlib.Path) -> dict[str, bytes]:
    patches: dict[str, bytes] = {}
    for p in sorted(patch_dir.iterdir()):
        if not p.is_file():
            continue
        data = p.read_bytes()
        if p.name in TEXT_ENTRIES or p.suffix == ".rc" or p.name == "adb_keys":
            if b"\r" in data:
                data = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        patches[p.name] = data
    return patches


def do_list(entries: list[Entry]) -> None:
    print(f"{'name':44s} {'mode':>7s} {'size':>8s}")
    for e in entries:
        print(f"{e.name:44s} {oct(e.mode):>7s} {len(e.data):>8d}")


def do_extract(entries: list[Entry], dest: pathlib.Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for e in entries:
        if e.name == CPIO_TRAILER.decode():
            continue
        target = dest / e.name
        if not (e.mode & 0o170000) == 0o100000:  # not a regular file
            print(f"skip (not regular): {e.name}")
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(e.data)
    print(f"extracted {len(entries) - 1} entries to {dest}")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--input", required=True, type=pathlib.Path)
    ap.add_argument("-o", "--output", type=pathlib.Path)
    ap.add_argument("--patch-dir", type=pathlib.Path)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", type=pathlib.Path)
    ap.add_argument(
        "--check-roundtrip",
        action="store_true",
        help="serialize the untouched cpio and assert it matches the input byte for byte",
    )
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    plen, payload, raw = read_image(args.input)
    pad_to = len(raw)
    ramdisk = gunzip(payload)
    entries = parse_cpio(ramdisk)
    print(f"image      : {args.input}  ({len(raw)} bytes, payload {plen})")
    print(f"ramdisk    : {len(ramdisk)} bytes, {len(entries)} entries")

    if args.list:
        do_list(entries)
        return 0
    if args.extract:
        do_extract(entries, args.extract)
        return 0
    if args.check_roundtrip:
        rebuilt = serialize_cpio(entries)
        if rebuilt == ramdisk:
            print(f"roundtrip : OK, {len(rebuilt)} bytes identical to the input ramdisk")
            return 0
        for i, (a, b) in enumerate(zip(ramdisk, rebuilt)):
            if a != b:
                print(f"roundtrip : MISMATCH at offset {i} (input {a:#x}, rebuilt {b:#x})")
                lo = max(0, i - 32)
                print(f"  input  : {ramdisk[lo:i + 32]!r}")
                print(f"  rebuilt: {rebuilt[lo:i + 32]!r}")
                break
        else:
            print(f"roundtrip : MISMATCH in length ({len(ramdisk)} vs {len(rebuilt)})")
        return 1

    if not args.patch_dir or not args.output:
        ap.error("--patch-dir and --output are required unless --list/--extract")

    patches = load_patches(args.patch_dir)
    by_name = {e.name: e for e in entries}
    next_ino = max(e.fields[F_INO] for e in entries) + 1
    changed: list[str] = []

    for name, data in patches.items():
        existing = by_name.get(name)
        if existing is None:
            if name not in ALLOWED_NEW:
                raise SystemExit(
                    f"{name}: not present in the ramdisk and not in the allow-list "
                    f"{sorted(ALLOWED_NEW)}; refusing to invent an init file"
                )
            fields = [0] * NFIELDS
            fields[F_INO] = next_ino
            next_ino += 1
            fields[F_MODE] = 0o100644
            fields[F_UID] = 0
            fields[F_GID] = 0
            fields[F_NLINK] = 1
            fields[F_MTIME] = 0
            # Insert before TRAILER so the trailer stays last.
            idx = next(i for i, e in enumerate(entries) if e.name == CPIO_TRAILER.decode())
            entries.insert(idx, Entry(fields, name, data))
            changed.append(f"+ {name} ({len(data)} bytes, new)")
            continue
        if existing.data == data:
            print(f"unchanged : {name}")
            continue
        changed.append(f"~ {name} ({len(existing.data)} -> {len(data)} bytes)")
        existing.data = data

    if not changed:
        print("nothing to do: ramdisk already matches the patch set")
    else:
        for line in changed:
            print(f"patch     : {line}")

    new_ramdisk = serialize_cpio(entries)
    new_payload = gzip_like_stock(new_ramdisk)
    out = KRNL_MAGIC + struct.pack("<I", len(new_payload)) + new_payload
    if len(out) < pad_to:
        out += b"\0" * (pad_to - len(out))
    elif len(out) > pad_to:
        raise SystemExit(
            f"result {len(out)} bytes exceeds the original {pad_to}; "
            "a bigger ramdisk does not fit the partition as-is"
        )

    # --- self check: read the result back the same way the kernel will ------
    plen2 = struct.unpack("<I", out[4:8])[0]
    back = parse_cpio(gunzip(out[8 : 8 + plen2]))
    if [e.name for e in back] != [e.name for e in entries]:
        raise SystemExit("self-check failed: entry order changed")
    for want, got in zip(entries, back):
        if want.data != got.data or want.fields[F_MODE] != got.fields[F_MODE]:
            raise SystemExit(f"self-check failed on {want.name!r}")

    if args.dry_run:
        print(f"dry-run   : would write {len(out)} bytes to {args.output}")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out)
    print(
        f"written   : {args.output}  {len(out)} bytes "
        f"(payload {len(new_payload)}, ramdisk {len(new_ramdisk)})"
    )
    print(f"self-check: {len(back)} entries verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
