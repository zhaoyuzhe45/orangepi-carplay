#!/system/bin/sh
# Device-side installer for the frp client.  Run as root:
#   sh install_frpc.sh <package-root>
# where <package-root> holds bin/frpc, scripts/frpc_launcher.sh and
# config/frpc.toml.template (i.e. the staged deploy/opi4 tree).
#
# Safe to re-run: an existing /data/carplay/frpc.toml is never overwritten, so
# a filled-in config survives reinstalls.
set -e

PKG=${1:-}
ROOT=/data/carplay
INIT_RC=/system/etc/init/carplay_frpc.rc

if [ -z "$PKG" ]; then
    echo "usage: install_frpc.sh <package-root>" >&2
    exit 2
fi

[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }
[ -f "$PKG/bin/frpc" ] || { echo "missing $PKG/bin/frpc" >&2; exit 1; }
[ -f "$PKG/scripts/frpc_launcher.sh" ] || { echo "missing $PKG/scripts/frpc_launcher.sh" >&2; exit 1; }

mkdir -p "$ROOT/bin" "$ROOT/logs"
chmod 700 "$ROOT" "$ROOT/logs" 2>/dev/null || true

# Stop the old instance before replacing the binary; MVFS keeps an executing
# image mapped, so overwriting in place leaves a running process on old code.
stop carplay_frpc 2>/dev/null || true

cp "$PKG/bin/frpc" "$ROOT/bin/frpc.new"
chmod 755 "$ROOT/bin/frpc.new"
mv -f "$ROOT/bin/frpc.new" "$ROOT/bin/frpc"

cp "$PKG/scripts/frpc_launcher.sh" "$ROOT/bin/frpc_launcher.sh.new"
chmod 755 "$ROOT/bin/frpc_launcher.sh.new"
mv -f "$ROOT/bin/frpc_launcher.sh.new" "$ROOT/bin/frpc_launcher.sh"

if [ -f "$ROOT/frpc.toml" ]; then
    echo "keeping existing $ROOT/frpc.toml"
else
    cp "$PKG/config/frpc.toml.template" "$ROOT/frpc.toml"
    chmod 600 "$ROOT/frpc.toml"
    echo "wrote template $ROOT/frpc.toml -- fill in __FRP_* before it connects"
fi

# /system is mounted read-only and verity-free on this image; remount for the
# rc file only, then put it back so nothing else can write there by accident.
mount -o rw,remount /system
cp "$PKG/scripts/init.frpc.rc" "$INIT_RC.new"
chmod 644 "$INIT_RC.new"
mv -f "$INIT_RC.new" "$INIT_RC"
mount -o ro,remount /system

# SELinux labels: /data/carplay must stay a data file, the rc file must look
# like every other init script or init will refuse to parse it.
restorecon -R "$ROOT" 2>/dev/null || true
restorecon "$INIT_RC" 2>/dev/null || true

echo "installed:"
ls -l "$ROOT/bin/frpc" "$ROOT/bin/frpc_launcher.sh" "$ROOT/frpc.toml" "$INIT_RC"

# init needs a restart of the service to pick up the new rc; on a fresh install
# the boot trigger has already fired, so start it here.
if ! start carplay_frpc 2>/dev/null; then
    echo "note: start carplay_frpc failed; it will come up on next boot"
fi
sleep 2
echo "service state: $(getprop init.svc.carplay_frpc)"
