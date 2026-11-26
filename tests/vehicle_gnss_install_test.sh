#!/bin/sh
# Execute the one-time installer in a disposable filesystem with Android commands mocked.
set -eu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/bin"
cat > "$tmp/bin/getprop" <<'EOF'
#!/bin/sh
case "$1" in ro.product.device) echo rk3399_mid;; ro.build.version.sdk) echo 27;; esac
EOF
printf '#!/bin/sh\necho 0\n' > "$tmp/bin/id"
printf '#!/bin/sh\necho Permissive\n' > "$tmp/bin/getenforce"
for tool in mount restorecon chown; do printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/$tool"; done
cat > "$tmp/bin/cp" <<'EOF'
#!/bin/sh
if [ "${FAIL_COPY:-0}" = 1 ]; then
    case "$2" in */vendor/manifest.xml.new) exit 69;; esac
fi
exec /bin/cp "$@"
EOF
chmod +x "$tmp/bin/"*
export PATH="$tmp/bin:$PATH"
setup() {
    root="$tmp/$1"
    mkdir -p "$root/vendor/lib64/hw" "$root/system/lib64" "$root/system/priv-app/CarPlayAudio" "$root/data/local/tmp/carplay-vehicle-gnss-stage"
    stage="$root/data/local/tmp/carplay-vehicle-gnss-stage"
    echo '<manifest type="device">original</manifest>' > "$root/vendor/manifest.xml"
    echo old-apk > "$root/system/priv-app/CarPlayAudio/CarPlayAudio.apk"
    echo interface > "$root/system/lib64/android.hardware.gnss@1.0.so"
    echo module > "$stage/module.so"
    echo interface > "$stage/interface.so"
    echo '<manifest type="device">new</manifest>' > "$stage/manifest.xml"
    (cd "$stage" && sha256sum module.so interface.so manifest.xml > SHA256SUMS)
    hash=$(sha256sum "$root/vendor/manifest.xml" | cut -d ' ' -f 1)
    sed -e "s|/vendor|$root/vendor|g" -e "s|/system/lib64|$root/system/lib64|g" \
        -e "s|/system/priv-app|$root/system/priv-app|g" -e "s|/data/|$root/data/|g" \
        deploy/opi4/scripts/install_vehicle_gnss.sh > "$root/install.sh"
}
setup success
sh "$root/install.sh" "$hash" > "$root/output"
cmp "$stage/module.so" "$root/vendor/lib64/hw/android.hardware.gnss@1.0-impl.so"
cmp "$stage/interface.so" "$root/vendor/lib64/android.hardware.gnss@1.0.so"
cmp "$stage/manifest.xml" "$root/vendor/manifest.xml"
backup=$(find "$root/data/carplay/backups" -name manifest.xml)
grep -q original "$backup"
[ "$(cat "$(dirname "$backup")/CarPlayAudio.apk")" = old-apk ]
echo 'PASS install and original manifest/APK backup'

setup rollback
echo interface > "$root/vendor/lib64/android.hardware.gnss@1.0.so"
if FAIL_COPY=1 sh "$root/install.sh" "$hash" > "$root/output" 2>&1; then exit 1; fi
grep -q original "$root/vendor/manifest.xml"
[ ! -e "$root/vendor/lib64/hw/android.hardware.gnss@1.0-impl.so" ]
[ "$(cat "$root/vendor/lib64/android.hardware.gnss@1.0.so")" = interface ]
[ ! -e "$root/data/carplay/vehicle-gnss.sha256" ]
[ ! -e "$root/vendor/lib64/hw/android.hardware.gnss@1.0-impl.so.new" ]
[ ! -e "$root/vendor/lib64/android.hardware.gnss@1.0.so.new" ]
grep -q 'restored files' "$root/output"
echo 'PASS failed copy rolls back all installed files'

setup foreign
echo foreign > "$root/vendor/lib64/hw/android.hardware.gnss@1.0-impl.so"
if sh "$root/install.sh" "$hash" > "$root/output" 2>&1; then exit 1; fi
[ "$(cat "$root/vendor/lib64/hw/android.hardware.gnss@1.0-impl.so")" = foreign ]
grep -q original "$root/vendor/manifest.xml"
[ ! -e "$root/data/carplay/backups" ]
echo 'PASS unknown HAL rejected before mutation'

setup race
echo changed > "$root/vendor/manifest.xml"
if sh "$root/install.sh" "$hash" > "$root/output" 2>&1; then exit 1; fi
[ "$(cat "$root/vendor/manifest.xml")" = changed ]
[ ! -e "$root/data/carplay/backups" ]
echo 'PASS manifest race rejected before mutation'

setup corruption
echo corrupt >> "$stage/module.so"
if sh "$root/install.sh" "$hash" > "$root/output" 2>&1; then exit 1; fi
[ ! -e "$root/data/carplay/backups" ]
echo 'PASS corrupted staging rejected before mutation'

setup abi
echo different-abi > "$root/system/lib64/android.hardware.gnss@1.0.so"
if sh "$root/install.sh" "$hash" > "$root/output" 2>&1; then exit 1; fi
[ ! -e "$root/data/carplay/backups" ]
echo 'PASS mismatching system ABI rejected before mutation'
