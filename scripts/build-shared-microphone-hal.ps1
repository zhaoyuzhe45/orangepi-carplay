param(
    [string] $Ndk = '',
    [string] $BuildDir = '',
    [switch] $Diagnostic
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'toolchain.ps1')
if (-not $Ndk) { $Ndk=Get-BoxNdkRoot }
if (!$BuildDir) { $BuildDir = Join-Path $repo 'build/shared-microphone' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$include = Join-Path $BuildDir 'include'
$lock = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'dependencies.lock.json') | ConvertFrom-Json
foreach ($entry in $lock.headers.'shared-microphone'.PSObject.Properties) {
    $file = $entry.Name
    $target = Join-Path $include $file
    if (-not (Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ine $entry.Value) {
        throw "Missing or changed AOSP header $file. Run python scripts/prepare-dependencies.py --headers."
    }
}
$clang = Join-Path $Ndk 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
$common = @('--target=armv7a-linux-androideabi27', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
    '-fstack-protector-strong', '-D_FORTIFY_SOURCE=2', "-I$include", '-Wl,-z,relro,-z,now', '-Wl,--no-undefined')
$src = Join-Path $repo 'system-patch/shared-microphone/audio_r_submix_fanout.c'
$hashFiles = @()
$logFlags = @()
$suffix = ''
if ($Diagnostic) {
    $logFlags = @('-DSHARED_MIC_OPEN_DIAGNOSTICS=1', '-llog')
    $suffix = '-diagnostic'
}
foreach ($variant in @('probe', 'production')) {
    $factory = if ($variant -eq 'probe') { '/vendor/lib/hw/audio.r_submix.default.so' }
        else { '/vendor/lib/hw/audio.r_submix.carplay-original.so' }
    $name = "audio.r_submix.fanout.$variant$suffix.so"
    # Define via a response file so Windows PowerShell/native argument quoting cannot strip the C string quotes.
    $defines = Join-Path $BuildDir "$variant.rsp"
    [IO.File]::WriteAllText($defines, ('-DORIGINAL_SUBMIX_PATH=\"' + $factory + '\"' + "`n"))
    & $clang @common "@$defines" '-shared' '-fPIC' '-fvisibility=hidden' "-Wl,-soname,$name" -o (Join-Path $BuildDir $name) $src '-ldl' @logFlags
    if ($LASTEXITCODE -ne 0) { throw "Failed to link $name" }
    $hashFiles += (Join-Path $BuildDir $name)
}
& $clang @common '-o' (Join-Path $BuildDir 'shared_microphone_hal_device_probe') (Join-Path $repo 'tests/shared_microphone_hal_device_probe.c') '-ldl'
if ($LASTEXITCODE -ne 0) { throw 'Failed to link the standalone real HAL probe' }
$hashFiles += (Join-Path $BuildDir 'shared_microphone_hal_device_probe')
Get-FileHash -Algorithm SHA256 -LiteralPath $hashFiles | Format-Table -AutoSize
