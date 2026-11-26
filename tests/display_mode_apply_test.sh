#!/bin/sh
# Behavioral checks against mocked Android commands; never touches a device.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
script=$root/deploy/opi4/scripts/apply_display_mode.sh
scratch=$(mktemp -d "${TMPDIR:-/tmp}/carplay-mode.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
mkdir -p "$scratch/bin" "$scratch/state"
export CARPLAY_ROOT=$scratch/state
export CARPLAY_BOOT_ID_FILE=$scratch/boot-id
export CARPLAY_DISPLAY_TIMEOUT=$scratch/bin/bounded
export CARPLAY_DISPLAY_WAIT_SECONDS=2
export CARPLAY_DISPLAY_PROFILE_DIR=$scratch/profile
export CARPLAY_DISPLAY_SETTINGS_DIR=$scratch/settings
export CARPLAY_BUSYBOX=$scratch/bin/busybox
export MOCK_STATE=$scratch
printf 'boot-a\n' > "$CARPLAY_BOOT_ID_FILE"
cat > "$scratch/bin/android" <<'EOF'
#!/bin/sh
set -eu
name=${0##*/}
printf '%s %s\n' "$name" "$*" >> "$MOCK_STATE/commands"
case "$name" in
getprop)
 case "$1" in
  init.svc.*) cat "$MOCK_STATE/${1#init.svc.}" ;;
  persist.sys.framebuffer.main) cat "$MOCK_STATE/mode" ;;
  persist.sys.resolution.main) printf 'Auto\n' ;;
 esac ;;
setprop)
 case "$1" in
  persist.sys.framebuffer.main) printf '%s\n' "$2" > "$MOCK_STATE/mode" ;;
  ctl.stop) printf 'stopped\n' > "$MOCK_STATE/$2" ;;
  ctl.restart) printf '%s\n' "$(($(cat "$MOCK_STATE/server-pid") + 1))" > "$MOCK_STATE/server-pid" ;;
  ctl.start)
   printf 'running\n' > "$MOCK_STATE/$2"
   if [ "$2" = surfaceflinger ]; then
    mode=$(cat "$MOCK_STATE/mode")
    if [ "$mode" != "$(cat "$MOCK_STATE/reject")" ]; then
     printf '%s\n' "${mode%@*}" > "$MOCK_STATE/physical"
    fi
   fi ;;
 esac ;;
dumpsys)
 if [ "${MOCK_PREP_DURING_READBACK:-}" = 1 ]; then mkdir -p "$CARPLAY_ROOT/.display-mode-lock-boot-b"; fi
 # Match the actual RK3399 SurfaceFlinger line, including space after '('.
 if [ "$1" = SurfaceFlinger ] && [ "${MOCK_EMPTY_SF:-}" != 1 ]; then
  printf '   type=0, hwcId=0, layerStack=0, ( %s), ANativeWindow=0x76aa7d0010 (8:8:8:8), orient= 0\n' "$(cat "$MOCK_STATE/physical" | sed 's/x/x /')"
 fi ;;
wm)
 if [ "${MOCK_FAIL_WM:-}" = "$*" ]; then exit 124; fi
 case "$1:$#" in
  size:1) printf 'Physical size: %s\n' "$(cat "$MOCK_STATE/physical")"
          [ ! -s "$MOCK_STATE/override" ] || printf 'Override size: %s\n' "$(cat "$MOCK_STATE/override")" ;;
  density:1) printf 'Physical density: 160\n'; printf 'Override density: %s\n' "$(cat "$MOCK_STATE/density")" ;;
  size:2) if [ "$2" = reset ]; then : > "$MOCK_STATE/override"; else printf '%s\n' "$2" > "$MOCK_STATE/override"; fi ;;
  density:2) if [ "$2" = reset ]; then printf '160\n' > "$MOCK_STATE/density"; else printf '%s\n' "$2" > "$MOCK_STATE/density"; fi ;;
 esac ;;
pidof) cat "$MOCK_STATE/server-pid" ;;
sleep) : ;;
stat)
 case "$2" in
  '%u:%a') if [ "${MOCK_UNTRUSTED_PROFILE:-}" = 1 ]; then printf '1000:777\n'; elif [ -d "$3" ]; then printf '0:700\n'; else printf '0:600\n'; fi ;;
  '%u:%g') printf '0:0\n' ;;
  '%a') printf '600\n' ;;
  '%s') printf '64\n' ;;
 esac ;;
chown|restorecon|sync) : ;;
esac
exit 0
EOF
for cmd in getprop setprop dumpsys wm pidof sleep stat chown restorecon sync; do cp "$scratch/bin/android" "$scratch/bin/$cmd"; chmod +x "$scratch/bin/$cmd"; done
cat > "$scratch/bin/bounded" <<'EOF'
#!/bin/sh
while [ "$#" -gt 0 ]; do
 case "$1" in -k|-s) shift 2 ;; *) shift; break ;; esac
done
exec "$@"
EOF
chmod +x "$scratch/bin/bounded"
printf '#!/bin/sh\nexec "$@"\n' > "$scratch/bin/busybox"
chmod +x "$scratch/bin/busybox"
export PATH="$scratch/bin:$PATH"
reset_mock() {
 printf '800x480@60\n' > "$scratch/mode"
 printf '800x480\n' > "$scratch/physical"
 printf '160\n' > "$scratch/density"
 printf 'none\n' > "$scratch/reject"
 printf '123\n' > "$scratch/server-pid"
 printf 'running\n' > "$scratch/surfaceflinger"
 printf 'running\n' > "$scratch/hwcomposer-2-1"
 : > "$scratch/override"
 : > "$scratch/commands"
}
fail() { echo "FAIL: $*" >&2; exit 1; }
assert_status() { grep -Fxq "$1" "$CARPLAY_ROOT/.display-mode-status" || fail "missing status $1"; }
assert_exit() {
 want=$1; shift
 set +e
 sh "$script" "$@" > "$scratch/stdout" 2> "$scratch/stderr"
 got=$?
 set -e
 [ "$got" = "$want" ] || { cat "$scratch/stderr" >&2; fail "expected exit $want, got $got: $*"; }
}
reset_mock
assert_exit 64 prepare 1234x567@60 160 bad --media-stopped
[ ! -s "$scratch/commands" ] || fail 'invalid mode reached Android commands'
assert_exit 64 prepare 800x480@30 160 bad --media-stopped
assert_exit 64 prepare 800x480@60 0 bad --media-stopped
assert_exit 64 prepare 800x480@60 119 bad --media-stopped
assert_exit 64 prepare 800x480@60 481 bad --media-stopped
assert_exit 64 prepare 800x480@60 160 bad
printf 'PASS: rejects unlisted modes, density, and missing media-stop assertion\n'
export MOCK_EMPTY_SF=1
assert_exit 70 prepare 1024x480@60 160 blank-sf --media-stopped
assert_status 'detail=preflight_failed'
grep -q '^setprop ctl\.' "$scratch/commands" && fail 'missing SF geometry restarted services'
unset MOCK_EMPTY_SF
: > "$scratch/commands"
printf 'PASS: empty SF geometry fails preflight without service changes\n'
assert_exit 0 prepare 800x480@60 160 noop --media-stopped
assert_status 'phase=complete'
assert_status 'detail=noop'
grep -q '^setprop ctl\.' "$scratch/commands" && fail 'no-op restarted services'
seq1=$(sed -n 's/^sequence=//p' "$CARPLAY_ROOT/.display-mode-status")
printf 'PASS: actual spaced SF geometry verifies native no-op without restart\n'
printf '1024x480\n' > "$scratch/override"
printf '200\n' > "$scratch/density"
: > "$scratch/commands"
assert_exit 0 prepare 800x480@60 160 logical --media-stopped
[ ! -s "$scratch/override" ] || fail 'logical override survived'
[ "$(cat "$scratch/density")" = 160 ] || fail 'density was not reset'
grep -q '^setprop ctl\.' "$scratch/commands" && fail 'logical cleanup restarted services'
printf 'PASS: stale WM override is cleared without physical restart\n'
assert_exit 0 prepare 1280x600@60 180 switch --media-stopped
assert_status 'mode=1280x600@60'
assert_status 'density=180'
assert_status 'phase=complete'
[ "$(cat "$scratch/physical")" = 1280x600 ] || fail 'physical mode did not switch'
seq2=$(sed -n 's/^sequence=//p' "$CARPLAY_ROOT/.display-mode-status")
[ "$seq2" -gt "$seq1" ] || fail 'transaction sequence did not increase'
stops=$(grep '^setprop ctl\.' "$scratch/commands" | tail -5)
[ "$stops" = "$(printf 'setprop ctl.stop surfaceflinger\nsetprop ctl.stop hwcomposer-2-1\nsetprop ctl.start hwcomposer-2-1\nsetprop ctl.start surfaceflinger\nsetprop ctl.restart zygote')" ] || fail 'unsafe service transition ordering'
printf 'PASS: switch verifies native geometry with ordered service transitions\n'
mkdir "$CARPLAY_ROOT/.display-mode-lock-boot-a"
printf 'boot-a %s\n' "$$" > "$CARPLAY_ROOT/.display-mode-lock-boot-a/owner"
: > "$scratch/commands"
assert_exit 75 prepare 1920x900@60 160 busy --media-stopped
[ ! -s "$scratch/commands" ] || fail 'busy worker mutated Android state'
rm -rf "$CARPLAY_ROOT/.display-mode-lock-boot-a"
printf 'PASS: exclusive lock rejects concurrent switches\n'
printf '1920x900@60\n' > "$scratch/reject"
assert_exit 70 prepare 1920x900@60 160 failing --media-stopped
assert_status 'phase=failed'
assert_status 'detail=switch_failed_rolled_back'
[ "$(cat "$scratch/mode")" = 1280x600@60 ] || fail 'mode property not rolled back'
[ "$(cat "$scratch/physical")" = 1280x600 ] || fail 'physical geometry not rolled back'
[ "$(cat "$scratch/density")" = 180 ] || fail 'density not rolled back'
: > "$scratch/commands"
assert_exit 76 prepare 1920x900@60 160 retry --media-stopped
[ ! -s "$scratch/commands" ] || fail 'failed target retried in same boot'
assert_status 'request_id=retry'
printf 'PASS: failure rolls back and blocks repeated target in one boot\n'
printf 'boot-b\n' > "$CARPLAY_BOOT_ID_FILE"
printf 'none\n' > "$scratch/reject"
assert_exit 0 prepare 1920x900@60 160 nextboot --media-stopped
assert_status 'boot_id=boot-b'
assert_status 'phase=complete'
printf 'PASS: failure suppression expires with kernel boot ID\n'
export MOCK_FAIL_WM='density 200'
assert_exit 70 prepare 1920x1080@60 200 wmfail --media-stopped
assert_status 'detail=wm_failed_rolled_back'
[ "$(cat "$scratch/physical")" = 1920x900 ] || fail 'WM timeout did not restore geometry'
unset MOCK_FAIL_WM
printf 'PASS: bounded WM failure restores prior physical and logical settings\n'
: > "$scratch/commands"
assert_exit 0 boot
[ ! -s "$scratch/commands" ] || fail 'boot path called Android or wait commands'
[ "$(cat "$CARPLAY_ROOT/.display-mode-boot-id")" = boot-b ] || fail 'boot marker missing'
printf 'PASS: compatibility boot path is immediate marker only\n'
assert_exit 0 can-prepare 800x480@60 160
assert_exit 76 can-prepare 1920x1080@60 200
mkdir "$CARPLAY_ROOT/.display-mode-lock-boot-b"
assert_exit 75 can-prepare 800x480@60 160
rmdir "$CARPLAY_ROOT/.display-mode-lock-boot-b"
printf 'PASS: can-prepare exposes failure and busy gates without Android work\n'

assert_exit 0 remember 1920x900@60 160
[ "$(cat "$CARPLAY_DISPLAY_PROFILE_DIR/display-profile.conf")" = "$(printf 'version=1\nmode=1920x900@60\ndensity=160')" ] || fail 'remember wrote wrong profile'
first_profile=$(sha256sum "$CARPLAY_DISPLAY_PROFILE_DIR/display-profile.conf")
: > "$scratch/commands"
assert_exit 0 remember 1920x900@60 160
[ "$first_profile" = "$(sha256sum "$CARPLAY_DISPLAY_PROFILE_DIR/display-profile.conf")" ] || fail 'repeated remember changed profile'
grep -Eq '^setprop |^wm (size|density) .|^(chown|restorecon|sync) ' "$scratch/commands" && fail 'matching remember made a mutation'
assert_exit 70 remember 800x480@60 160
[ "$first_profile" = "$(sha256sum "$CARPLAY_DISPLAY_PROFILE_DIR/display-profile.conf")" ] || fail 'stale session changed remembered profile'
printf 'PASS: remember verifies current mode, rejects stale session, repeats as noop\n'
mkdir "$CARPLAY_DISPLAY_PROFILE_DIR/.display-profile-lock-boot-b"
assert_exit 0 can-prepare 800x480@60 160
printf '180\n' > "$scratch/density"
assert_exit 75 remember 1920x900@60 180
printf '160\n' > "$scratch/density"
rmdir "$CARPLAY_DISPLAY_PROFILE_DIR/.display-profile-lock-boot-b"
export MOCK_PREP_DURING_READBACK=1
assert_exit 70 remember 1920x900@60 160
unset MOCK_PREP_DURING_READBACK
rmdir "$CARPLAY_ROOT/.display-mode-lock-boot-b"
[ "$first_profile" = "$(sha256sum "$CARPLAY_DISPLAY_PROFILE_DIR/display-profile.conf")" ] || fail 'racing preparation changed remembered profile'
printf 'PASS: cache lock leaves handshake gate open; racing preparation preserves prior cache\n'

: > "$scratch/commands"
assert_exit 0 early
[ "$(cat "$scratch/mode")" = 1920x900@60 ] || fail 'early ignored trusted cache'
grep -Eq '^sleep |^dumpsys |^wm |^pidof |^setprop ctl\.' "$scratch/commands" && fail 'early path waited, read framework, or restarted graphics'
printf 'version=1\nmode=1234x567@60\ndensity=160\n' > "$CARPLAY_DISPLAY_PROFILE_DIR/display-profile.conf"
assert_exit 0 early
[ "$(cat "$scratch/mode")" = 800x480@60 ] || fail 'corrupt profile failed to use fallback'
printf 'version=1\nmode=1280x720@60\ndensity=200\n' > "$CARPLAY_DISPLAY_PROFILE_DIR/display-profile.conf"
export MOCK_UNTRUSTED_PROFILE=1
assert_exit 0 early
[ "$(cat "$scratch/mode")" = 800x480@60 ] || fail 'untrusted ownership failed to use fallback'
unset MOCK_UNTRUSTED_PROFILE
printf 'PASS: early trusted cache is applied without waits; corrupt/untrusted cache falls back\n'

mkdir "$CARPLAY_DISPLAY_SETTINGS_DIR"
for kind in global secure; do
 cat > "$CARPLAY_DISPLAY_SETTINGS_DIR/settings_$kind.xml" <<'EOF'
<settings>
  <setting id="1" name="unrelated" value="kept" />
  <setting id="2" name="display_size_forced" value="1800,1080" />
  <setting id="3" name="display_density_forced" value="360" />
</settings>
EOF
done
: > "$scratch/commands"
assert_exit 0 early-settings
for kind in global secure; do
 grep -q 'name="unrelated" value="kept"' "$CARPLAY_DISPLAY_SETTINGS_DIR/settings_$kind.xml" || fail 'unrelated setting was lost'
 grep -q 'display_.*_forced' "$CARPLAY_DISPLAY_SETTINGS_DIR/settings_$kind.xml" && fail 'forced display setting survived'
done
grep -Eq '^sleep |^dumpsys |^wm |^pidof |^setprop ctl\.' "$scratch/commands" && fail 'early settings path called framework or waited'
printf 'PASS: offline early settings cleanup preserves unrelated settings without binder work\n'
printf 'all display mode behavioral tests passed\n'
