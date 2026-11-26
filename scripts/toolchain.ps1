# Shared host tool lookup for source builds. No workstation-specific paths.
function Get-BoxSdkRoot {
    foreach ($path in @($env:ANDROID_SDK_ROOT, $env:ANDROID_HOME)) {
        if ($path -and (Test-Path -LiteralPath $path -PathType Container)) { return [IO.Path]::GetFullPath($path) }
    }
    throw 'Set ANDROID_SDK_ROOT (or ANDROID_HOME) to the Android SDK directory.'
}
function Get-BoxNdkRoot {
    $path = $env:ANDROID_NDK_ROOT
    if (-not $path) { $path = Join-Path (Get-BoxSdkRoot) 'ndk/22.1.7171670' }
    if (-not (Test-Path -LiteralPath (Join-Path $path 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'))) {
        throw 'Android NDK 22.1.7171670 is required; install it in the SDK or set ANDROID_NDK_ROOT.'
    }
    return [IO.Path]::GetFullPath($path)
}
function Get-BoxJavaHome {
    if ($env:JAVA_HOME -and (Test-Path -LiteralPath (Join-Path $env:JAVA_HOME 'bin/javac.exe'))) { return $env:JAVA_HOME }
    $command = Get-Command javac.exe -ErrorAction SilentlyContinue
    if ($command) { return Split-Path -Parent (Split-Path -Parent $command.Source) }
    throw 'Set JAVA_HOME to a JDK 8 installation.'
}
function Get-BoxPython {
    $command = Get-Command python.exe -ErrorAction SilentlyContinue
    if (-not $command) { throw 'Python 3.12 must be on PATH.' }
    return $command.Source
}
