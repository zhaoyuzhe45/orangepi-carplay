#!/bin/sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/carplay-inventory.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
mkdir -p "$scratch/bin" "$scratch/state"
export CARPLAY_ROOT=$scratch/state
export CARPLAY_DISPLAY_RESOURCE=$scratch/resource.xml
export CARPLAY_BUSYBOX=$scratch/bin/busybox
cat > "$scratch/bin/busybox" <<'EOF'
#!/bin/sh
exec "$@"
EOF
cat > "$scratch/bin/id" <<'EOF'
#!/bin/sh
printf '0\n'
EOF
for cmd in mount restorecon sync; do printf '#!/bin/sh\nexit 0\n' > "$scratch/bin/$cmd"; done
chmod +x "$scratch/bin/"*
export PATH="$scratch/bin:$PATH"
cat > "$CARPLAY_DISPLAY_RESOURCE" <<'EOF'
<resolutions>
<resolution><hdisplay>1280</hdisplay><vdisplay>720</vdisplay><vrefresh>60</vrefresh></resolution>
<resolution><hdisplay>1920</hdisplay><vdisplay>1080</vdisplay><vrefresh>60</vrefresh></resolution>
</resolutions>
EOF
sh "$root/deploy/opi4/scripts/provision_display_modes.sh" "$root/deploy/opi4/config/resolution-white-modes.xml"
count=$(grep -c '<resolution>' "$CARPLAY_DISPLAY_RESOURCE")
[ "$count" = 6 ] || { echo "FAIL: expected six entries without stock duplicates, got $count" >&2; exit 1; }
first=$(sha256sum "$CARPLAY_DISPLAY_RESOURCE" | cut -d ' ' -f1)
sh "$root/deploy/opi4/scripts/provision_display_modes.sh" "$root/deploy/opi4/config/resolution-white-modes.xml"
[ "$first" = "$(sha256sum "$CARPLAY_DISPLAY_RESOURCE" | cut -d ' ' -f1)" ] || { echo 'FAIL: provisioning is not idempotent' >&2; exit 1; }
[ "$(grep -c '<resolution>' "$CARPLAY_ROOT/backups/resolution_white.original.xml")" = 2 ] || { echo 'FAIL: original backup changed' >&2; exit 1; }
printf 'PASS: six-preset provisioning preserves stock modes and original backup; repeat is byte-identical\n'
