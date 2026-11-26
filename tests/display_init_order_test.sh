#!/bin/sh
# Exercise installer preflight against the identity-verified init references.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
refs=$root/tests/fixtures/opi4-display-init
scratch=$(mktemp -d "${TMPDIR:-/tmp}/carplay-init-order.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
mkdir "$scratch/bin"
cp "$refs/init.rc" "$scratch/init.rc"
cp "$refs/init.rk30board.bootmode.emmc.rc" "$scratch/board.rc"
cp "$refs/fstab.rk30board" "$scratch/fstab"
cp "$refs/system_etc_init_surfaceflinger.rc" "$scratch/sf.rc"
cp "$refs/vendor_etc_init_android.hardware.graphics.composer@2.1-service.rc" "$scratch/hwc.rc"
printf '#!/bin/sh\nexec "$@"\n' > "$scratch/bin/busybox"
printf '#!/bin/sh\nexit 0\n' > "$scratch/bin/timeout"
cat > "$scratch/bin/getprop" <<'EOF'
#!/bin/sh
case "$1" in
ro.product.device) printf 'rk3399_mid\n' ;;
ro.build.version.release) printf '8.1.0\n' ;;
esac
EOF
chmod +x "$scratch/bin/"*
export PATH="$scratch/bin:$PATH"
PROFILE_DIR=$scratch/profile
sed -n '/^validate_display_boot_order()/,/^}/p' "$root/deploy/opi4/scripts/install_carplay.sh" |
 sed -e "s|/system/bin/busybox|$scratch/bin/busybox|g" \
     -e "s|/system/bin/timeout|$scratch/bin/timeout|g" \
     -e "s|/init.rc|$scratch/init.rc|g" \
     -e "s|/init.rk30board.bootmode.emmc.rc|$scratch/board.rc|g" \
     -e "s|/fstab.rk30board|$scratch/fstab|g" \
     -e "s|/vendor/etc/init/android.hardware.graphics.composer@2.1-service.rc|$scratch/hwc.rc|g" \
     -e "s|/system/etc/init/surfaceflinger.rc|$scratch/sf.rc|g" > "$scratch/preflight.sh"
. "$scratch/preflight.sh"
validate_display_boot_order || { echo 'FAIL: verified board graph was rejected' >&2; exit 1; }
sed 's/^on boot$/on early-boot/' "$scratch/init.rc" > "$scratch/changed"
mv "$scratch/changed" "$scratch/init.rc"
if validate_display_boot_order; then echo 'FAIL: early graphics class start was accepted' >&2; exit 1; fi
cp "$refs/init.rc" "$scratch/init.rc"
sed '/\/cache[[:space:]]/s/wait,check/wait,check,latemount/' "$scratch/fstab" > "$scratch/changed"
mv "$scratch/changed" "$scratch/fstab"
if validate_display_boot_order; then echo 'FAIL: late cache mount was accepted' >&2; exit 1; fi
printf 'PASS: installer accepts verified graph and rejects early graphics or late cache mounts\n'
# The board parses root init before system rc. Replay the actual FDE load action
# with ro.persistent_properties.ready already true, so it emits no new event.
# Only the decrypt event itself can restore the active fallback/cached target.
for selected in '800x480@60 160' '1280x720@60 200'; do
    set -- $selected
    awk -v selected_mode="$1" -v selected_density="$2" '
    BEGIN {
        active_mode=selected_mode; active_density=selected_density
        framebuffer=selected_mode; density=selected_density; qemu=selected_density
    }
    /^[[:space:]]*#/ { next }
    /^on / {
        root_load=($0=="on property:vold.decrypt=trigger_load_persist_props")
        replay=($0=="on property:vold.decrypt=trigger_load_persist_props && property:sys.carplay.display.active_mode=*")
        next
    }
    /^service / { root_load=0; replay=0 }
    root_load && $1=="load_persist_props" {
        loaded++
        framebuffer="1920x1080@60"; density=360
        # ro.ready was already true: Android cannot set this read-only property
        # again, so no ro.ready property action is triggered by this load.
    }
    replay && NF {
        if ($1!="setprop" || !loaded) bad=1
        if ($2=="persist.sys.framebuffer.main" && $3=="${sys.carplay.display.active_mode}") framebuffer=active_mode
        if ($2=="persist.sys.resolution.main" && $3=="Auto") resolution="Auto"
        if ($2=="persist.sys.sf.lcd_density" && $3=="${sys.carplay.display.active_density}") density=active_density
        if ($2=="qemu.sf.lcd_density" && $3=="${sys.carplay.display.active_density}") qemu=active_density
    }
    END {
        if (loaded!=1 || bad || framebuffer!=selected_mode || density!=selected_density || qemu!=selected_density || resolution!="Auto") {
            print "FAIL: second FDE persist load did not preserve active profile " selected_mode > "/dev/stderr"
            exit 1
        }
    }
    ' "$refs/init.rc" "$root/deploy/opi4/scripts/init.carplay.rc"
done
printf 'PASS: ordered decrypt replay preserves fallback and cached profiles without readiness retrigger\n'
