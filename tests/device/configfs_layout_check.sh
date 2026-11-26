#!/system/bin/sh
set -eu
s=${1:-/data/carplay/bin/carplay_supervisor.sh}
grep -Fq 'STOCK_CONFIG=$G/configs/b.1' "$s"
grep -Fq 'CARPLAY_CONFIG=$G/configs/b.6' "$s"
grep -Fq 'OS_DESC=$G/os_desc' "$s"
grep -Fq 'STOCK_OS_DESC_CONFIG_LINK=$OS_DESC/b.1' "$s"
grep -Fq 'OS_DESC_CONFIG_LINK=$OS_DESC/b.6' "$s"
grep -Fq 'LINK=$CARPLAY_CONFIG/f4' "$s"
grep -Fq 'STOCK_F2=$(readlink "$STOCK_CONFIG/f2"' "$s"
grep -Fq 'mkdir -p "$CARPLAY_CONFIG"' "$s"
grep -Fq 'echo 500 > "$CARPLAY_CONFIG/MaxPower"' "$s"
grep -Fq 'echo 0x80 > "$CARPLAY_CONFIG/bmAttributes"' "$s"
grep -Fq 'rm -f "$STOCK_OS_DESC_CONFIG_LINK"' "$s"
grep -Fq 'ln -s "$CARPLAY_CONFIG" "$OS_DESC_CONFIG_LINK"' "$s"
grep -Fq 'rm -f "$LINK"' "$s"
grep -Fq 'rmdir "$CARPLAY_CONFIG"' "$s"
echo "supervisor configfs layout PASS"
