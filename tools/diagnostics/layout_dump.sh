#!/system/bin/sh
echo "== b.1 links =="; ls -l /config/usb_gadget/g1/configs/b.1/ | grep -v '^total'
echo "== b.2 links =="; ls -l /config/usb_gadget/g1/configs/b.2/ | grep -v '^total'
echo "== functions dir =="; ls /config/usb_gadget/g1/functions/
echo "== ncm linked? =="; for c in b.1 b.2; do for f in /config/usb_gadget/g1/configs/$c/f*; do printf '%s -> %s\n' "$f" "$(readlink $f)"; done; done
