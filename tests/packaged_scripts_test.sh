#!/bin/sh
# Guards the seam between "a script exists in the deploy package" and "the
# installer actually puts it on the device".
#
# This is not hypothetical.  usb_role_switch.sh was written, hashed into
# SHA256SUMS and referenced by the supervisor, but install_carplay.sh never
# copied it -- so on a real box the CarPlay role handoff called a file that was
# not there, failed, and fell back to device three times before giving up.  The
# package verified fine because it was internally consistent; only the link to
# the device was missing.  That is exactly the class of bug a manifest check
# cannot see, so it gets its own test.
#
# Run with sh from the project root.
set -eu
project=$(cd "$(dirname "$0")/.." && pwd)
installer=$project/deploy/opi4/scripts/install_carplay.sh
supervisor=$project/deploy/opi4/scripts/carplay_supervisor.sh
sums=$project/deploy/opi4/SHA256SUMS

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

[ -f "$installer" ] || fail "missing $installer"
[ -f "$supervisor" ] || fail "missing $supervisor"
[ -f "$sums" ] || fail "missing $sums"

# ---- what the installer copies onto the device ---------------------------
installed=$(sed -n 's/.*cp "\$STAGE\/\([A-Za-z0-9._-]*\)".*/\1/p' "$installer" | sort -u)
[ -n "$installed" ] || fail "could not parse any cp \$STAGE lines out of install_carplay.sh"

# ---- what the installer insists must be staged ---------------------------
required=$(awk '/for required in/{f=1} f{gsub(/\\/," "); print} /; do/{if(f) exit}' "$installer" |
	sed 's/for required in//; s/; do//' | tr -s ' \t\n' '\n' | sed '/^$/d' | sort -u)
[ -n "$required" ] || fail "could not parse the required-file list out of install_carplay.sh"

# ---- what the supervisor expects to find at runtime ----------------------
referenced=$(sed -n 's/.*\$ROOT\/bin\/\([A-Za-z0-9._-]*\).*/\1/p' "$supervisor" | sort -u)
[ -n "$referenced" ] || fail "could not parse any \$ROOT/bin references out of the supervisor"

# aa_boot_attach.sh belongs to the optional Android Auto package and is only
# reached through the mode.aa escape hatch, so it is deliberately not part of
# the CarPlay installer.  Everything else must be installed.
optional='aa_boot_attach.sh'

# ---- 1. everything the supervisor says it is required --------------------
for name in $required; do
	case " $(echo $installed) " in
	*" $name "*) ;;
	*) fail "$name is in the required list but the installer never copies it" ;;
	esac
done
pass "every required file is also installed"

# ---- 2. everything the installer copies is a real package file -----------
for name in $installed; do
	if ! grep -q "  \(scripts\|bin\|config\)/$name\$" "$sums"; then
		fail "$name is installed but missing from SHA256SUMS (it would ship unhashed)"
	fi
done
pass "every installed file is covered by SHA256SUMS"

# ---- 3. everything the supervisor runs at runtime ------------------------
missing=
for name in $referenced; do
	[ "$name" = "$optional" ] && continue
	case " $(echo $installed) " in
	*" $name "*) ;;
	*) missing="$missing $name" ;;
	esac
done
[ -z "$missing" ] || fail "supervisor calls files the installer never installs:$missing"
pass "every supervisor \$ROOT/bin dependency is installed"

# ---- 4. the specific regression that started this test -------------------
case " $(echo $installed) " in
*" usb_role_switch.sh "*) ;;
*) fail "usb_role_switch.sh is not installed; the role handoff would fail on device" ;;
esac
grep -q 'usb_role_switch.sh' "$sums" ||
	fail "usb_role_switch.sh is not hashed in SHA256SUMS"
pass "the role switch helper is both installed and hashed"

echo "packaged scripts test: all checks passed"
