#!/usr/bin/env python3
"""Set ``dr_mode`` on one dwc3 node inside a Rockchip resource image.

Why this exists
---------------
``/sys/devices/platform/usb0/dwc3_mode`` is the only switch that moves the
Type-C port between peripheral and host on this board, and
``dwc3_mode_store()`` refuses to act unless the port was described as
``dr_mode = "otg"`` at boot:

    if (rockchip->original_dr_mode != USB_DR_MODE_OTG) {
            dev_err(rockchip->dev, "Not support set mode!\\n");
            return -EINVAL;
    }

The shipped image was patched the other way round (``otg`` -> ``peripheral``,
extcon dropped) to make the port permanently a device.  That is exactly the
state that makes role switching impossible, so this tool writes the value back
without touching anything else.

How the value is replaced
-------------------------
An FDT property carries its own length, so a shorter string cannot simply be
written in place without rebuilding the structure.  Instead the new value is
padded with NUL bytes to the old length: the kernel reads a C string, stops at
the first NUL, and never looks at the padding.  ``totalsize``, the string table
and every following byte in the container therefore stay identical, which is
what keeps the RSCE index table and the rest of the partition valid.

Usage
-----
    python patch_dr_mode.py -i resource.img --show
    python patch_dr_mode.py -i resource.img -o resource-otg.img --set otg
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys

FDT_MAGIC = 0xD00DFEED
RSCE_MAGIC = b"RSCE"
DTB_OFFSET = 0x800  # header + three 512-byte index entries, constant here

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


class Dtb:
    def __init__(self, blob: bytes):
        self.raw = bytearray(blob)
        (magic, self.totalsize, self.off_struct, self.off_strings,
         _rsv, _ver, _lcv, _cpu, self.size_strings, _size_struct) = struct.unpack(
            ">10I", self.raw[:40])
        if magic != FDT_MAGIC:
            raise SystemExit(f"not a device tree: magic {magic:#x}")
        if self.totalsize != len(self.raw):
            raise SystemExit(
                f"totalsize {self.totalsize} != blob {len(self.raw)}")
        self.strings = bytes(
            self.raw[self.off_strings:self.off_strings + self.size_strings])

    def name_at(self, nameoff: int) -> str:
        end = self.strings.index(b"\0", nameoff)
        return self.strings[nameoff:end].decode(errors="replace")

    def walk(self):
        """Yield (path, kind, struct_offset, length, name) for every property."""
        pos = self.off_struct
        stack: list[str] = []
        while True:
            tok = struct.unpack(">I", self.raw[pos:pos + 4])[0]
            pos += 4
            if tok == FDT_BEGIN_NODE:
                end = self.raw.index(b"\0", pos)
                node = self.raw[pos:end].decode(errors="replace")
                stack.append(node or "/")
                pos = (end + 4) & ~3
            elif tok == FDT_END_NODE:
                stack.pop()
            elif tok == FDT_PROP:
                length, nameoff = struct.unpack(">II", self.raw[pos:pos + 8])
                pos += 8
                yield "/".join(stack), pos, length, self.name_at(nameoff)
                pos = (pos + length + 3) & ~3
            elif tok == FDT_NOP:
                continue
            elif tok == FDT_END:
                return
            else:
                raise SystemExit(f"bad fdt token {tok:#x} at {pos - 4:#x}")

    def find(self, node_suffix: str, prop: str):
        for path, off, length, name in self.walk():
            if name == prop and path.endswith(node_suffix):
                return path, off, length
        return None

    def get_string(self, node_suffix: str, prop: str):
        hit = self.find(node_suffix, prop)
        if not hit:
            return None
        _path, off, length = hit
        return bytes(self.raw[off:off + length]).split(b"\0")[0].decode()

    def set_padded_string(self, node_suffix: str, prop: str, value: str):
        hit = self.find(node_suffix, prop)
        if not hit:
            raise SystemExit(f"{prop} not found under {node_suffix}")
        path, off, length = hit
        payload = value.encode() + b"\0"
        if len(payload) > length:
            raise SystemExit(
                f'"{value}" does not fit in {length} bytes of {path}/{prop}')
        new = payload + b"\0" * (length - len(payload))
        self.raw[off:off + length] = new
        return path, off, length


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-i", "--input", required=True)
    ap.add_argument("-o", "--output")
    ap.add_argument("--node", default="dwc3@fe800000")
    ap.add_argument("--set", choices=["otg", "peripheral", "host"])
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args(argv)

    img = pathlib.Path(args.input).read_bytes()
    if img[:4] != RSCE_MAGIC:
        raise SystemExit("input is not an RSCE resource image")
    dtb_len = struct.unpack(">I", img[DTB_OFFSET + 4:DTB_OFFSET + 8])[0]
    dtb = Dtb(img[DTB_OFFSET:DTB_OFFSET + dtb_len])

    if args.show or not args.set:
        print(f"image      : {args.input} ({len(img)} bytes)")
        print(f"dtb        : {dtb_len} bytes at 0x{DTB_OFFSET:x}")
        print(f"md5        : {hashlib.md5(bytes(dtb.raw)).hexdigest()}")
        for node in (args.node,):
            print(f"node       : {node}")
            print(f"  dr_mode      = {dtb.get_string(node, 'dr_mode')!r}")
            print(f"  extcon       = "
                  f"{'present' if dtb.find(node, 'extcon') else 'absent'}")
        print(f"usb2-phy   : extcon "
              f"{'present' if dtb.find('usb2-phy@e450', 'extcon') else 'absent'}")
        if not args.set:
            return 0

    if not args.output:
        raise SystemExit("--output is required with --set")

    before = bytes(dtb.raw)
    path, off, length = dtb.set_padded_string(args.node, "dr_mode", args.set)
    after = bytes(dtb.raw)
    changed = [i for i in range(len(before)) if before[i] != after[i]]
    if len(before) != len(after) or not changed:
        raise SystemExit("internal error: dtb size changed or nothing written")
    expected = {off + i for i in range(length)}
    if not set(changed) <= expected:
        raise SystemExit("refusing to write: bytes outside dr_mode changed")
    # Re-parse to prove the structure is still walkable.
    check = Dtb(after)
    got = check.get_string(args.node, "dr_mode")
    if got != args.set:
        raise SystemExit(f"re-read gave {got!r}, expected {args.set!r}")

    out = bytearray(img)
    out[DTB_OFFSET:DTB_OFFSET + dtb_len] = after
    pathlib.Path(args.output).write_bytes(out)
    print(f"node       : {path}")
    print(f"dr_mode    : -> {args.set!r} ({len(changed)} bytes changed at "
          f"0x{off:x}, property stays {length} bytes)")
    print(f"dtb md5    : {hashlib.md5(after).hexdigest()}")
    print(f"output     : {args.output} ({len(out)} bytes, container unchanged)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
