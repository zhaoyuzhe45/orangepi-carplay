#!/usr/bin/env python3
"""Route unclaimed Apple vendor requests to any configuration's functions.

2026-10-29, first real head unit (A4L): the car sends 0x40/0x51 while the
device is still *addressed* -- and re-probes it every minute from standby.
composite_setup() only offered vendor requests to the *selected*
configuration's functions, so with no config selected (or parked on the
PTP-only b.1) the ffs function living in b.2 never saw the request, the
control pipe stalled, and the car reported the accessory as unsupported.

The fix: after the static identity probes (0x45/0x53) decline, search every
configuration for a req_match claimant before stalling.  A function that
cannot answer right now (ffs without its daemon: -ENODEV from ->setup)
stalls the request exactly as before, so the daemon-dead regression case is
unchanged.

Idempotent: re-running on a patched file is a no-op.
"""
import sys

MARKER = "Search the remaining configurations for a claimant"

OLD = (
    "\t\tif (cdev->config) {\n"
    "\t\t\tlist_for_each_entry(f, &cdev->config->functions, list)\n"
    "\t\t\t\tif (f->req_match && f->req_match(f, ctrl))\n"
    "\t\t\t\t\tgoto try_fun_setup;\n"
    "\t\t\tf = NULL;\n"
    "\t\t}\n"
    "\n"
    "\t\tvalue = box_vendor_probe(ctrl, req->buf, w_length);\n"
    "\t\tif (value < 0)\n"
    "\t\t\tgoto unknown;\n"
    "\n"
    "\t\treq->length = value;\n"
    "\t\treq->zero = value < w_length;\n"
    "\t\tvalue = composite_ep0_queue(cdev, req, GFP_ATOMIC);\n"
    "\t\tif (value < 0) {\n"
    "\t\t\tDBG(cdev, \"ep_queue --> %d\\n\", value);\n"
    "\t\t\treq->status = 0;\n"
    "\t\t\tcomposite_setup_complete(gadget->ep0, req);\n"
    "\t\t}\n"
    "\t\treturn value;\n"
    "\t}"
)

NEW = (
    "\t\tif (cdev->config) {\n"
    "\t\t\tlist_for_each_entry(f, &cdev->config->functions, list)\n"
    "\t\t\t\tif (f->req_match && f->req_match(f, ctrl))\n"
    "\t\t\t\t\tgoto try_fun_setup;\n"
    "\t\t\tf = NULL;\n"
    "\t\t}\n"
    "\n"
    "\t\tvalue = box_vendor_probe(ctrl, req->buf, w_length);\n"
    "\t\tif (value >= 0) {\n"
    "\t\t\treq->length = value;\n"
    "\t\t\treq->zero = value < w_length;\n"
    "\t\t\tvalue = composite_ep0_queue(cdev, req, GFP_ATOMIC);\n"
    "\t\t\tif (value < 0) {\n"
    "\t\t\t\tDBG(cdev, \"ep_queue --> %d\\n\", value);\n"
    "\t\t\t\treq->status = 0;\n"
    "\t\t\t\tcomposite_setup_complete(gadget->ep0, req);\n"
    "\t\t\t}\n"
    "\t\t\treturn value;\n"
    "\t\t}\n"
    "\n"
    "\t\t/*\n"
    "\t\t * box: a real head unit does not wait for SET_CONFIGURATION\n"
    "\t\t * either.  0x51/0x52 arrive while the device is still addressed,\n"
    "\t\t * or while it is parked on the PTP-only b.1; the ffs function\n"
    "\t\t * that owns the decision lives in b.2 and never saw them, so\n"
    "\t\t * the control pipe stalled and the car reported the accessory\n"
    "\t\t * as unsupported (A4L, 2026-10-29).  Search the remaining\n"
    "\t\t * configurations for a claimant.  A function that cannot answer\n"
    "\t\t * right now (ffs without its daemon: -ENODEV from ->setup)\n"
    "\t\t * stalls the request exactly as before.\n"
    "\t\t */\n"
    "\t\t{\n"
    "\t\t\tstruct usb_configuration *c;\n"
    "\n"
    "\t\t\tlist_for_each_entry(c, &cdev->configs, list) {\n"
    "\t\t\t\tif (c == cdev->config)\n"
    "\t\t\t\t\tcontinue;\n"
    "\t\t\t\tlist_for_each_entry(f, &c->functions, list)\n"
    "\t\t\t\t\tif (f->req_match &&\n"
    "\t\t\t\t\t    f->req_match(f, ctrl))\n"
    "\t\t\t\t\t\tgoto try_fun_setup;\n"
    "\t\t\t}\n"
    "\t\t\tf = NULL;\n"
    "\t\t}\n"
    "\n"
    "\t\tgoto unknown;\n"
    "\t}"
)


def main():
    if len(sys.argv) != 2:
        print("usage: patch_composite_unconfigured.py <composite.c>",
              file=sys.stderr)
        return 2
    path = sys.argv[1]
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if MARKER in text:
        print("already applied:", path)
        return 0
    count = text.count(OLD)
    if count == 0:
        print("FAIL: expected block not found (tree layout changed?)",
              file=sys.stderr)
        return 1
    if count > 1:
        print("FAIL: expected block is not unique (%d hits)" % count,
              file=sys.stderr)
        return 1
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text.replace(OLD, NEW))
    print("patched:", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
