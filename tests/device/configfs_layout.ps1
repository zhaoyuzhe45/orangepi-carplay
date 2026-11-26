param([string]$Supervisor = '')
$ErrorActionPreference = 'Stop'
if (-not $Supervisor) {
    $Supervisor = Join-Path $PSScriptRoot '..\..\deploy\opi4\scripts\carplay_supervisor.sh'
}
$text = Get-Content -LiteralPath $Supervisor -Raw
$requirements = @(
    'STOCK_CONFIG=$G/configs/b.1',
    'CARPLAY_CONFIG=$G/configs/b.6',
    'OS_DESC=$G/os_desc',
    'STOCK_OS_DESC_CONFIG_LINK=$OS_DESC/b.1',
    'OS_DESC_CONFIG_LINK=$OS_DESC/b.6',
    'LINK=$CARPLAY_CONFIG/f4',
    'CARPLAY_OS_VENDOR_CODE=0x52',
    'STOCK_F2=$(readlink "$STOCK_CONFIG/f2"',
    'STOCK_F3=$(readlink "$STOCK_CONFIG/f3"',
    'STOCK_F4=$(readlink "$STOCK_CONFIG/f4"',
    'STOCK_F5=$(readlink "$STOCK_CONFIG/f5"',
    'STOCK_F6=$(readlink "$STOCK_CONFIG/f6"',
    'mkdir -p "$CARPLAY_CONFIG"',
    'echo 500 > "$CARPLAY_CONFIG/MaxPower"',
    'echo 0x80 > "$CARPLAY_CONFIG/bmAttributes"',
    'rm -f "$LINK"',
    'rmdir "$CARPLAY_CONFIG"',
    'echo 1 > "$OS_DESC/use"',
    'echo "$CARPLAY_OS_VENDOR_CODE" > "$OS_DESC/b_vendor_code"',
    'rm -f "$STOCK_OS_DESC_CONFIG_LINK"',
    'ln -s "$CARPLAY_CONFIG" "$OS_DESC_CONFIG_LINK"'
)
foreach ($required in $requirements) {
    if (-not $text.Contains($required)) { throw "supervisor is missing configfs invariant: $required" }
}

$restoreMatch = [regex]::Match(
    $text,
    '(?ms)^restore_stock_gadget\(\) \{.*?^\}'
)
if (-not $restoreMatch.Success) { throw 'restore_stock_gadget() was not found' }
$restoreBody = $restoreMatch.Value
if (-not $restoreBody.Contains('for slot in f2 f3 f4 f5 f6; do')) {
    throw 'restore_stock_gadget() must clear temporary stock slots f2 through f6'
}
if (-not $restoreBody.Contains('rm -f "$STOCK_CONFIG/$slot"')) {
    throw 'restore_stock_gadget() does not remove the temporary stock slot links'
}
if ($restoreBody.Contains('rm -f "$STOCK_CONFIG/f1"')) {
    throw 'restore_stock_gadget() must preserve the stock adb function b.1/f1'
}

if (-not $restoreBody.Contains('restore_file "$OS_DESC/use" "$ORIGINAL_OS_DESC_USE"')) {
    throw 'restore_stock_gadget() must restore OS descriptor use'
}
if (-not $restoreBody.Contains('restore_file "$OS_DESC/b_vendor_code" "$ORIGINAL_OS_DESC_VENDOR_CODE"')) {
    throw 'restore_stock_gadget() must restore OS descriptor vendor code'
}
if (-not $restoreBody.Contains('rm -f "$STOCK_OS_DESC_CONFIG_LINK"')) {
    throw 'restore_stock_gadget() must unlink the temporary OS descriptor configuration'
}
if (-not $restoreBody.Contains('restore_symlink "$STOCK_OS_DESC_CONFIG_LINK" "$ORIGINAL_OS_DESC_CONFIG"')) {
    throw 'restore_stock_gadget() must restore the captured OS descriptor config link'
}
$modifiedMatch = [regex]::Match($text, '(?ms)^stock_gadget_is_modified\(\) \{.*?^\}')
if (-not $modifiedMatch.Success) { throw 'stock_gadget_is_modified() was not found' }
foreach ($required in @('"$OS_DESC/use"', '"$OS_DESC/b_vendor_code"', '"$STOCK_OS_DESC_CONFIG_LINK"', '"$OS_DESC_CONFIG_LINK"')) {
    if (-not $modifiedMatch.Value.Contains($required)) {
        throw "stock_gadget_is_modified() does not detect $required"
    }
}
$attachMatch = [regex]::Match($text, '(?ms)^attach_gadget\(\) \{.*?^\}')
if (-not $attachMatch.Success) { throw 'attach_gadget() was not found' }
$attachBody = $attachMatch.Value
$ensureIndex = $attachBody.IndexOf('ensure_carplay_config || return 1')
$functionLinkIndex = $attachBody.IndexOf('ln -s "$G/functions/ffs.iap2" "$LINK"')
$stockOsUnlinkIndex = $attachBody.IndexOf('rm -f "$STOCK_OS_DESC_CONFIG_LINK"')
$osLinkIndex = $attachBody.IndexOf('ln -s "$CARPLAY_CONFIG" "$OS_DESC_CONFIG_LINK"')
$bindIndex = $attachBody.IndexOf('echo "$UDC" > "$G/UDC"')
if ($ensureIndex -lt 0 -or $functionLinkIndex -lt 0 -or $stockOsUnlinkIndex -lt 0 -or $osLinkIndex -lt 0 -or $bindIndex -lt 0 -or
    -not ($ensureIndex -lt $functionLinkIndex -and $functionLinkIndex -lt $stockOsUnlinkIndex -and $stockOsUnlinkIndex -lt $osLinkIndex -and $osLinkIndex -lt $bindIndex)) {
    throw 'attach_gadget() must recreate b.6, link its function and OS descriptor, then bind UDC'
}
Write-Host 'supervisor configfs layout PASS'
