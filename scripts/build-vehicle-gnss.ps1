param([string]$ReferenceDirectory='', [string]$Output='')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'toolchain.ps1')
if (-not $ReferenceDirectory) { $ReferenceDirectory=Join-Path $root 'build/deps/gnss' }
if (-not $Output) { $Output=Join-Path $root 'build/vehicle-gnss/android.hardware.gnss@1.0-impl.so' }
New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($Output))) | Out-Null
$refs=(Resolve-Path -LiteralPath $ReferenceDirectory).Path
$clang=Join-Path (Get-BoxNdkRoot) 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
$argsList=@('--target=aarch64-linux-android27','-std=c++17','-O2','-fPIC','-shared','-fno-rtti','-fno-exceptions',
    '-nostdinc++','-nostdlib++','-isystem',"$refs/include/libcxx",'-I',"$refs/include",
    '-Wl,-z,defs','-Wl,-soname,android.hardware.gnss@1.0-impl.so',
    "$root/system-patch/vehicle-gnss/VehicleGnss.cpp",'-L',"$refs/lib64",
    '-l:android.hardware.gnss@1.0.so','-lhidlbase','-lhidltransport','-lutils','-lc++','-llog','-lcutils',
    '-o',$Output)
& $clang @argsList
if($LASTEXITCODE){throw 'Vehicle GNSS HAL build failed'}
Get-FileHash -Algorithm SHA256 -LiteralPath $Output
