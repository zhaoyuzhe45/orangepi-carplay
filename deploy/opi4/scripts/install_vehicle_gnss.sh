#!/system/bin/sh
# One-time installation only; never called by init or the supervisor.
set -eu
stage=/data/local/tmp/carplay-vehicle-gnss-stage
module=/vendor/lib64/hw/android.hardware.gnss@1.0-impl.so
interface=/vendor/lib64/android.hardware.gnss@1.0.so
manifest=/vendor/manifest.xml
owner=/data/carplay/vehicle-gnss.sha256
[ "$(id -u)" = 0 ]
[ "$(getprop ro.product.device)" = rk3399_mid ]
[ "$(getprop ro.build.version.sdk)" = 27 ]
[ "$(getenforce)" = Permissive ]
cd "$stage"
sha256sum -c SHA256SUMS
before_hash=$(sha256sum "$manifest" | cut -d ' ' -f 1)
[ "$before_hash" = "$(echo "${1:?expected manifest hash}" | tr A-F a-f)" ] || {
    echo 'Vendor manifest changed after preflight; retry installation' >&2; exit 1;
}
new_hash=$(sha256sum module.so | cut -d ' ' -f 1)
if [ -e "$module" ]; then
    current_hash=$(sha256sum "$module" | cut -d ' ' -f 1)
    owned_hash=$(cat "$owner" 2>/dev/null || true)
    [ "$current_hash" = "$new_hash" ] || [ "$current_hash" = "$owned_hash" ] || {
        echo 'Refusing to replace unknown GNSS HAL' >&2; exit 1;
    }
fi
cmp -s /system/lib64/android.hardware.gnss@1.0.so interface.so
if [ -e "$interface" ]; then cmp -s "$interface" interface.so; fi
backup=/data/carplay/backups/vehicle-gnss-$(date +%Y%m%d-%H%M%S)-$$
mkdir -p "$backup"
cp "$manifest" "$backup/manifest.xml"
if [ -e "$module" ]; then cp "$module" "$backup/module.so"; fi
if [ -e "$interface" ]; then cp "$interface" "$backup/interface.so"; fi
if [ -e "$owner" ]; then cp "$owner" "$backup/owner.sha256"; fi
if [ -e /system/priv-app/CarPlayAudio/CarPlayAudio.apk ]; then
    cp /system/priv-app/CarPlayAudio/CarPlayAudio.apk "$backup/CarPlayAudio.apk"
fi
mount -o remount,rw /vendor
committed=0
rollback() {
    if [ "$committed" = 0 ]; then
        cp "$backup/manifest.xml" "$manifest"
        if [ -e "$backup/module.so" ]; then cp "$backup/module.so" "$module"; else rm -f "$module"; fi
        if [ -e "$backup/interface.so" ]; then cp "$backup/interface.so" "$interface"; else rm -f "$interface"; fi
        if [ -e "$backup/owner.sha256" ]; then cp "$backup/owner.sha256" "$owner"; else rm -f "$owner"; fi
        rm -f "$module.new" "$interface.new" "$manifest.new"
        restorecon "$manifest" "$module" "$interface" 2>/dev/null || true
        sync
        echo "Installation failed; restored files from $backup" >&2
    fi
}
trap rollback EXIT
trap 'exit 1' HUP INT TERM
mkdir -p /vendor/lib64/hw
cp module.so "$module.new"
cp interface.so "$interface.new"
cp manifest.xml "$manifest.new"
chmod 644 "$module.new" "$interface.new" "$manifest.new"
chown 0:0 "$module.new" "$interface.new" "$manifest.new"
mv "$module.new" "$module"
mv "$interface.new" "$interface"
mv "$manifest.new" "$manifest"
restorecon "$module" "$interface" "$manifest" 2>/dev/null || true
cmp -s module.so "$module"
cmp -s interface.so "$interface"
cmp -s manifest.xml "$manifest"
echo "$new_hash" > "$owner"
sync
committed=1
echo "Installed vehicle GNSS. Backup: $backup. Reboot once to register gps."
