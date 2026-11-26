#!/usr/bin/env python3
"""Force the OTG port into device mode inside a Rockchip resource image.

What and why
------------
The RK3399 USB3 controller (dwc3@fe800000, i.e. /usb0) is wired to a FUSB302
Type-C controller.  The stock device tree describes it as ``dr_mode = "otg"``
and attaches an ``extcon`` phandle, so the role is decided at runtime by cable
detection.  That is exactly what "USB must never act as a host" cannot allow:
plug the wrong cable or hit a detection glitch and the port flips to host.

Two edits, both inside rk-kernel.dtb:

  1. ``dr_mode`` on /usb0/dwc3@fe800000: "otg" -> "peripheral"
  2. drop ``extcon`` from /usb0 and from /syscon@ff770000/usb2-phy@e450, so
     dwc3 no longer waits for an external role notification

The second point has field evidence behind it: /usb1 (dwc3@fe900000) has no
extcon property at all and works, which shows the property is optional for
this glue.

Size is preserved on purpose.  The dtb lives at a fixed offset inside a flat
RSCE container whose index table records its length; keeping totalsize
identical means the index table, the mem_rsvmap and every following resource
(logo.bmp, ...) stay byte-for-byte valid.  The slack is reclaimed by emitting
FDT_NOP tokens, which the kernel ignores.

Usage
-----
    python patch_resource.py -i resource.img -o resource-box.img
    python patch_resource.py -i resource.img --dump      # inspect only
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

FDT_MAGIC = 0xD00DFEED
RSCE_MAGIC = b"RSCE"
DTB_OFFSET = 0x800  # constant in this image: header + three 512-byte index entries

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9

TARGET_NODE = "/usb0/dwc3@fe800000"
TARGET_PROP = "dr_mode"
NEW_DR_MODE = b"peripheral\0"
DROP = {("/usb0", "extcon"), ("/syscon@ff770000/usb2-phy@e450", "extcon")}


def be32(b: bytes, o: int) -> int:
    return struct.unpack(">I", b[o : o + 4])[0]


def u32(v: int) -> bytes:
    return struct.pack(">I", v)


class Dtb:
    def __init__(self, blob: bytes) -> None:
        self.blob = blob
        (magic, self.totalsize, self.off_struct, self.off_strings, self.off_rsvmap,
         self.version, self.last_comp, self.boot_cpuid, self.size_strings,
         self.size_struct) = struct.unpack(">10I", blob[:40])
        if magic != FDT_MAGIC:
            raise SystemExit(f"not a dtb: magic {magic:#x}")
        self.strings = blob[self.off_strings : self.off_strings + self.size_strings]

    def prop_name(self, nameoff: int) -> str:
        end = self.strings.index(b"\0", nameoff)
        return self.strings[nameoff:end].decode(errors="replace")

    def walk(self) -> list[tuple[str, str, bytes]]:
        """Return (path, prop, value) for every property, for inspection."""
        src = self.blob
        pos = self.off_struct
        stack: list[str] = []
        out: list[tuple[str, str, bytes]] = []
        while True:
            tok = be32(src, pos)
            if tok == FDT_BEGIN_NODE:
                end = src.index(b"\0", pos + 4)
                stack.append(src[pos + 4 : end].decode(errors="replace"))
                pos = (end + 4) & ~3
            elif tok == FDT_END_NODE:
                stack.pop()
                pos += 4
            elif tok == FDT_PROP:
                length = be32(src, pos + 4)
                name = self.prop_name(be32(src, pos + 8))
                out.append(("/" + "/".join(p for p in stack if p), name,
                            src[pos + 12 : pos + 12 + length]))
                pos = (pos + 12 + length + 3) & ~3
            elif tok == FDT_NOP:
                pos += 4
            elif tok == FDT_END:
                break
            else:
                raise SystemExit(f"bad FDT token {tok} at {pos}")
        return out

    def patched_struct(self) -> tuple[bytes, list[str]]:
        src = self.blob
        pos = self.off_struct
        out = bytearray()
        stack: list[str] = []
        delta = 0
        log: list[str] = []
        while True:
            tok = be32(src, pos)
            if tok == FDT_BEGIN_NODE:
                end = src.index(b"\0", pos + 4)
                stack.append(src[pos + 4 : end].decode(errors="replace"))
                nxt = (end + 4) & ~3
                out += src[pos:nxt]
                pos = nxt
            elif tok == FDT_END_NODE:
                stack.pop()
                out += src[pos : pos + 4]
                pos += 4
            elif tok == FDT_PROP:
                length = be32(src, pos + 4)
                nameoff = be32(src, pos + 8)
                name = self.prop_name(nameoff)
                vstart = pos + 12
                nxt = (vstart + length + 3) & ~3
                path = "/" + "/".join(p for p in stack if p)

                if (path, name) in DROP:
                    delta -= nxt - pos
                    log.append(f"drop    {path}/{name}  ({nxt - pos} bytes)")
                elif path == TARGET_NODE and name == TARGET_PROP:
                    padded = NEW_DR_MODE + b"\0" * (-len(NEW_DR_MODE) % 4)
                    blk = (u32(FDT_PROP) + u32(len(NEW_DR_MODE))
                           + u32(nameoff) + padded)
                    delta += len(blk) - (nxt - pos)
                    old = src[vstart : vstart + length]
                    log.append(f"replace {path}/{name}: {old!r} -> {NEW_DR_MODE!r}")
                    out += blk
                else:
                    out += src[pos:nxt]
                pos = nxt
            elif tok == FDT_NOP:
                out += src[pos : pos + 4]
                pos += 4
            elif tok == FDT_END:
                if delta % 4:
                    raise SystemExit(f"size delta {delta} is not a multiple of 4")
                if delta < 0:
                    nops = -delta // 4
                    out += u32(FDT_NOP) * nops
                    log.append(f"pad     {nops} FDT_NOP (+{-delta} bytes)")
                elif delta > 0:
                    raise SystemExit(
                        f"patch grew the struct by {delta} bytes; this image has no "
                        "slack to absorb it and the RSCE index would need rewriting"
                    )
                out += src[pos : pos + 4]
                break
            else:
                raise SystemExit(f"bad FDT token {tok} at {pos}")
        return bytes(out), log


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--input", required=True, type=pathlib.Path)
    ap.add_argument("-o", "--output", type=pathlib.Path)
    ap.add_argument("--dump", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    res = args.input.read_bytes()
    if res[:4] != RSCE_MAGIC:
        raise SystemExit(f"{args.input}: not an RSCE image (magic {res[:4]!r})")
    dtb_size = be32(res, DTB_OFFSET + 4)
    dtb = Dtb(res[DTB_OFFSET : DTB_OFFSET + dtb_size])
    print(f"image   : {args.input} ({len(res)} bytes)")
    print(f"dtb     : offset {DTB_OFFSET:#x}, {dtb_size} bytes "
          f"(struct {dtb.size_struct}, strings {dtb.size_strings})")

    if args.dump:
        for path, name, val in dtb.walk():
            if name in ("dr_mode", "extcon") or path == "/usb0":
                print(f"  {path:48s} {name:12s} {val!r}")
        return 0

    if not args.output:
        ap.error("--output is required unless --dump")

    already = all(
        not (p == "/usb0" and n == "extcon")
        and not (p == "/usb0/dwc3@fe800000" and n == "dr_mode" and v != NEW_DR_MODE)
        for p, n, v in dtb.walk()
    )
    if already:
        print("unchanged: dtb already forces device mode "
              "(dr_mode=peripheral, no extcon on /usb0)")
        if args.dry_run:
            print(f"dry-run : nothing to do for {args.output}")
            return 0
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(res)
        print(f"written : {args.output} (verbatim copy, {len(res)} bytes)")
        return 0

    new_struct, log = dtb.patched_struct()
    for line in log:
        print(f"patch   : {line}")
    if len(new_struct) != dtb.size_struct:
        raise SystemExit(
            f"struct length changed ({dtb.size_struct} -> {len(new_struct)}); "
            "refusing, the RSCE index would go stale"
        )

    new_dtb = bytearray(dtb.blob)
    new_dtb[dtb.off_struct : dtb.off_struct + len(new_struct)] = new_struct
    if len(new_dtb) != dtb_size:
        raise SystemExit(f"dtb length changed ({dtb_size} -> {len(new_dtb)})")

    # --- verify before writing: parse the result and check every invariant ---
    check = Dtb(bytes(new_dtb))
    props = check.walk()
    dr = [v for p, n, v in props if p == "/usb0/dwc3@fe800000" and n == "dr_mode"]
    if dr != [NEW_DR_MODE]:
        raise SystemExit(f"verify failed: dr_mode reads back as {dr!r}")
    leftovers = [p for p, n, v in props if n == "extcon" and p in
                 ("/usb0", "/syscon@ff770000/usb2-phy@e450")]
    if leftovers:
        raise SystemExit(f"verify failed: extcon still present on {leftovers}")
    other_roles = [(p, v) for p, n, v in props if n == "dr_mode" and p != "/usb0/dwc3@fe800000"]
    for p, v in other_roles:
        print(f"note    : {p} dr_mode left as {v!r} (not the OTG port)")

    out_img = bytearray(res)
    out_img[DTB_OFFSET : DTB_OFFSET + dtb_size] = new_dtb
    if len(out_img) != len(res):
        raise SystemExit("image length changed")

    if args.dry_run:
        print(f"dry-run : would write {len(out_img)} bytes to {args.output}")
        return 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(bytes(out_img))
    print(f"written : {args.output} ({len(out_img)} bytes, index table untouched)")
    print("verify  : dr_mode=peripheral, extcon removed from /usb0 and usb2-phy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
