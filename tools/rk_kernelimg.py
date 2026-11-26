#!/usr/bin/env python3
"""Inspect and repack a Rockchip kernel partition image (kernel.img).

Why this exists
---------------
The NCM fix needs a kernel with ``CONFIG_USB_CONFIGFS_NCM=y`` and a
``composite.c`` that stops forcing ``bcdUSB = 0x0210``.  Both are compiled
into the arm64 ``Image``, which lives in the *kernel* partition wrapped in
Rockchip's little ``KRNL`` container.  Rebuilding that container by hand is
error prone, so this tool does it deterministically and verifies the result.

Container layout (confirmed against the 32 MiB dump of this box)
---------------------------------------------------------------
    offset 0x00   "KRNL"
    offset 0x04   u32 little endian, payload length
    offset 0x08   payload

The payload of this device is an *uncompressed* arm64 ``Image``: it starts
with a ``b`` branch (opcode 0b000101) and carries the ``ARM\\x64`` magic
(0x644d5241) at 0x38.  It is deliberately not gzip -- matching the original
exactly is what keeps the second stage loader happy, so gzip is rejected
unless ``--allow-compressed`` is passed.

Design rules
------------
* The header length field is the single source of truth for how much the
  loader copies.  Anything after ``8 + payload`` is residue from whatever
  occupied the partition before, is not read, and is therefore dropped.
* The payload is never rewritten.  Only the 8 byte header is generated, so
  a repack of an untouched payload round-trips to the same bytes.
* The result is read back and re-parsed before the tool reports success.

Usage
-----
    python rk_kernelimg.py -i kernel.img --info
    python rk_kernelimg.py -i kernel.img --extract out/Image
    python rk_kernelimg.py -i kernel.img -o new-kernel.img --image Image
    python rk_kernelimg.py -i kernel.img -o new-kernel.img --image Image --dry-run
    python rk_kernelimg.py -i kernel.img --roundtrip
"""

from __future__ import annotations

import argparse
import struct
import sys
import pathlib

KRNL_MAGIC = b"KRNL"
HDR_LEN = 8

# arm64 Image header, as laid out in Documentation/arm64/booting.txt.
IMG_TEXT_OFFSET = 0x08
IMG_SIZE = 0x10
IMG_FLAGS = 0x18
IMG_MAGIC = 0x38
ARM64_MAGIC = 0x644D5241

# kernel partition on this box, from /proc/partitions and blockdev --getsize64.
DEFAULT_CAPACITY = 32 * 1024 * 1024
ALIGN = 4096


def read_container(path: pathlib.Path) -> tuple[int, bytes, bytes]:
    """Return (declared_len, payload, whole_file)."""
    raw = path.read_bytes()
    if len(raw) < HDR_LEN:
        raise SystemExit(f"{path}: only {len(raw)} bytes, not a KRNL container")
    if raw[:4] != KRNL_MAGIC:
        raise SystemExit(f"{path}: bad magic {raw[:4]!r}, expected {KRNL_MAGIC!r}")
    (declared,) = struct.unpack_from("<I", raw, 4)
    if declared == 0:
        raise SystemExit(f"{path}: header says the payload is 0 bytes")
    if declared > len(raw) - HDR_LEN:
        raise SystemExit(
            f"{path}: header claims {declared} payload bytes but the file only "
            f"carries {len(raw) - HDR_LEN}"
        )
    return declared, raw[HDR_LEN:HDR_LEN + declared], raw


def describe(payload: bytes) -> dict[str, object]:
    info: dict[str, object] = {
        "compressed": payload[:2] == b"\x1f\x8b",
        "is_arm64_image": False,
    }
    if info["compressed"]:
        return info
    if len(payload) < IMG_MAGIC + 4:
        return info
    (magic,) = struct.unpack_from("<I", payload, IMG_MAGIC)
    if magic != ARM64_MAGIC:
        return info
    (opcode,) = struct.unpack_from("<I", payload, 0)
    info.update(
        {
            "is_arm64_image": True,
            "entry_branch": opcode >> 26 == 0b000101,
            "text_offset": struct.unpack_from("<Q", payload, IMG_TEXT_OFFSET)[0],
            "image_size": struct.unpack_from("<Q", payload, IMG_SIZE)[0],
            "flags": struct.unpack_from("<Q", payload, IMG_FLAGS)[0],
        }
    )
    return info


def cmd_info(path: pathlib.Path, capacity: int) -> int:
    declared, payload, raw = read_container(path)
    info = describe(payload)
    print(f"container  : {path}  ({len(raw)} bytes)")
    print(f"magic      : {KRNL_MAGIC.decode()}  payload length {declared}")
    print(f"payload    : {len(payload)} bytes")
    if info["compressed"]:
        print("kind       : gzip (compressed kernel)")
    elif info["is_arm64_image"]:
        print("kind       : uncompressed arm64 Image")
        print(f"text_offset: {info['text_offset']:#x}")
        print(f"image_size : {info['image_size']} ({info['image_size'] / 1024 / 1024:.2f} MiB, includes .bss)")
        print(f"flags      : {info['flags']:#x}")
        if not info["entry_branch"]:
            print("warning    : first word is not a `b` branch, the entry stub looks wrong")
    else:
        print("kind       : unknown, no ARM64 magic at 0x38")
    if len(raw) > HDR_LEN + declared:
        print(f"residue    : {len(raw) - HDR_LEN - declared} bytes after the payload, not read by the loader")
    print(f"capacity   : {capacity} bytes ({capacity / 1024 / 1024:.0f} MiB), " f"headroom {capacity - HDR_LEN - len(payload)}")
    return 0


def cmd_extract(path: pathlib.Path, dest: pathlib.Path) -> int:
    declared, payload, _ = read_container(path)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(payload)
    info = describe(payload)
    kind = "gzip" if info["compressed"] else ("arm64 Image" if info["is_arm64_image"] else "unknown")
    print(f"extracted  : {declared} bytes -> {dest}  ({kind})")
    return 0


def cmd_roundtrip(path: pathlib.Path) -> int:
    declared, payload, raw = read_container(path)
    rebuilt = KRNL_MAGIC + struct.pack("<I", len(payload)) + payload
    if rebuilt != raw[: len(rebuilt)]:
        print("roundtrip  : MISMATCH")
        for i, (a, b) in enumerate(zip(raw, rebuilt)):
            if a != b:
                print(f"  first difference at offset {i}: input {a:#04x}, rebuilt {b:#04x}")
                break
        return 1
    print(f"roundtrip  : OK, {len(rebuilt)} bytes identical to the input header + payload")
    return 0


def build(orig: pathlib.Path, image: pathlib.Path, out: pathlib.Path,
          capacity: int, allow_compressed: bool, dry_run: bool,
          pad_to_partition: bool) -> int:
    declared, orig_payload, _ = read_container(orig)
    payload = image.read_bytes()
    if not payload:
        raise SystemExit(f"{image}: empty file")
    info = describe(payload)
    compressed = payload[:2] == b"\x1f\x8b"
    if compressed and not allow_compressed:
        raise SystemExit(
            f"{image}: starts with a gzip header but the original payload is a raw "
            "arm64 Image; pass --allow-compressed only if you really mean to compress"
        )
    if not compressed and not info["is_arm64_image"]:
        raise SystemExit(f"{image}: no ARM\\x64 magic at 0x38, this is not an arm64 Image")

    total = HDR_LEN + len(payload)
    padded = (total + ALIGN - 1) // ALIGN * ALIGN
    if padded > capacity:
        raise SystemExit(
            f"new payload is {len(payload)} bytes, which does not fit the {capacity} byte "
            f"partition ({padded} with padding); the kernel must shrink by "
            f"{total - capacity} bytes"
        )
    if pad_to_partition:
        # Writing a full partition image leaves no stale bytes behind from
        # whatever was flashed before, so the on-device write is fully defined.
        padded = capacity

    print(f"source     : {orig}  payload {declared} bytes")
    print(f"new image  : {image}  payload {len(payload)} bytes ({len(payload) - declared:+d})")
    if info["is_arm64_image"]:
        print(f"             text_offset {info['text_offset']:#x}, image_size {info['image_size']}")
    print(f"output     : {out}  {padded} bytes (payload + {padded - total} bytes of zero padding)")
    if dry_run:
        print("dry run    : nothing written")
        return 0

    blob = KRNL_MAGIC + struct.pack("<I", len(payload)) + payload
    blob += b"\x00" * (padded - len(blob))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)

    # Read back through the same parser the flasher will rely on.
    r_declared, r_payload, r_raw = read_container(out)
    if r_declared != len(payload) or r_payload != payload:
        raise SystemExit(f"{out}: read-back does not match what was written")
    print(f"verified   : {out} re-parses as KRNL/{r_declared} bytes, payload identical")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("-i", "--input", required=True, type=pathlib.Path)
    ap.add_argument("-o", "--output", type=pathlib.Path)
    ap.add_argument("--image", type=pathlib.Path, help="raw arm64 Image to wrap")
    ap.add_argument("--info", action="store_true")
    ap.add_argument("--extract", type=pathlib.Path)
    ap.add_argument("--roundtrip", action="store_true")
    ap.add_argument("--allow-compressed", action="store_true")
    ap.add_argument("--capacity", type=int, default=DEFAULT_CAPACITY)
    ap.add_argument(
        "--pad-to-partition",
        action="store_true",
        help="pad the output to the full partition size instead of the payload",
    )
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    if args.info:
        return cmd_info(args.input, args.capacity)
    if args.extract:
        return cmd_extract(args.input, args.extract)
    if args.roundtrip:
        return cmd_roundtrip(args.input)
    if args.image and args.output:
        return build(args.input, args.image, args.output,
                     args.capacity, args.allow_compressed, args.dry_run,
                     args.pad_to_partition)
    ap.error("pick one of --info / --extract / --roundtrip, or give --image and --output")


if __name__ == "__main__":
    sys.exit(main())
