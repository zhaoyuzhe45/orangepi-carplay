param([string]$Original='', [string]$OutputDirectory='')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'toolchain.ps1')
if (-not $Original) { $Original=Join-Path $root 'build/deps/bluetooth/bluetooth.default.arm64.so' }
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $root 'build/bluetooth-classic' }
$llvm=Join-Path (Get-BoxNdkRoot) 'toolchains/llvm/prebuilt/windows-x86_64/bin'
$python=Get-BoxPython
$out=$ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
New-Item -ItemType Directory -Force $out | Out-Null
& "$llvm/clang.exe" --target=aarch64-linux-android27 -c "$root/system-patch/bluetooth-classic/guard-arm64.S" -o "$out/guard.o"
if($LASTEXITCODE){throw 'Guard compilation failed'}
& "$llvm/ld.lld.exe" -Ttext=0x91318 --entry=guard --defsym=BTM_IsAclConnectionUp=0x10fbf4 --defsym=bonded_continuation=0x9136c "$out/guard.o" -o "$out/guard.elf"
if($LASTEXITCODE){throw 'Guard linking failed'}
& "$llvm/llvm-objcopy.exe" --only-section=.text -O binary "$out/guard.elf" "$out/guard.bin"
if($LASTEXITCODE){throw 'Guard extraction failed'}
& $python "$root/scripts/patch-bluetooth-classic.py" $Original "$out/guard.bin" "$out/bluetooth.default.so"
if($LASTEXITCODE){throw 'Firmware validation or patch failed'}
