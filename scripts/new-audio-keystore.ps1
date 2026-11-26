param(
    [string]$Keystore = '',
    [string]$PasswordFile = '',
    [switch]$Force,
    [switch]$SkipCredentialStore
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Keystore) { $Keystore = Join-Path $projectRoot 'config\signing\carplay-audio.keystore' }
if (-not $PasswordFile) { $PasswordFile = Join-Path $projectRoot 'config\signing\carplay-audio.password' }
if ((Test-Path -LiteralPath $Keystore) -and -not $Force) { throw "Keystore already exists: $Keystore (use -Force to rotate)" }

function Find-Keytool {
    $candidates = @(
        $(if ($env:JAVA_HOME) { Join-Path $env:JAVA_HOME 'bin/keytool.exe' })
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    $command = Get-Command keytool.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw 'keytool.exe not found.'
}

$alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789'
$randomBytes = New-Object byte[] 48
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
try { $rng.GetBytes($randomBytes) } finally { $rng.Dispose() }
$builder = New-Object System.Text.StringBuilder
foreach ($value in $randomBytes) { [void]$builder.Append($alphabet[$value % $alphabet.Length]) }
$password = $builder.ToString()
$temporary = "$Keystore.new"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent ([IO.Path]::GetFullPath($Keystore))) | Out-Null
try {
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    $newEnvironment = $password
    $previous = $env:CARPLAY_NEW_PASSWORD
    $env:CARPLAY_NEW_PASSWORD = $newEnvironment
    try {
        & (Find-Keytool) -genkeypair -noprompt -storetype PKCS12 -keystore $temporary `
            -storepass:env CARPLAY_NEW_PASSWORD -keypass:env CARPLAY_NEW_PASSWORD -alias CARPLAY `
            -keyalg RSA -keysize 3072 -sigalg SHA256withRSA -validity 36500 `
            -dname 'CN=CarPlay Audio, O=CarPlay Box, C=CN'
        if ($LASTEXITCODE -ne 0) { throw 'keytool key generation failed.' }
        & (Find-Keytool) -list -keystore $temporary -storepass:env CARPLAY_NEW_PASSWORD -alias CARPLAY | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'keytool password verification failed.' }
    } finally {
        if ($null -eq $previous) { Remove-Item Env:CARPLAY_NEW_PASSWORD -ErrorAction SilentlyContinue }
        else { $env:CARPLAY_NEW_PASSWORD = $previous }
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $PasswordFile) | Out-Null
    [IO.File]::WriteAllText($PasswordFile, $password, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $Keystore -Force

    $account = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    foreach ($secretPath in @($Keystore, $PasswordFile)) {
        & icacls.exe $secretPath /inheritance:r /grant:r "${account}:(F)" 'SYSTEM:(F)' 'Administrators:(F)' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not restrict file ACL: $secretPath" }
    }

    if (-not $SkipCredentialStore) {
        $credentialSource = Join-Path $projectRoot 'tools\CarPlayCredentialStore.cs'
        if (-not ('CarPlayBox.CredentialStore' -as [type])) { Add-Type -Path $credentialSource }
        $credentialTarget = 'CarPlayBox/CarPlayAudio/Keystore'
        [CarPlayBox.CredentialStore]::Write($credentialTarget, 'CARPLAY', $password)
        if ([CarPlayBox.CredentialStore]::Read($credentialTarget) -cne $password) {
            throw 'Windows Credential Manager read-back did not match the new keystore password.'
        }
        Write-Host "Windows credential updated: $credentialTarget"
    }

    Write-Host "Keystore generated: $Keystore"
    Write-Host "Restricted plaintext password stored: $PasswordFile"
} finally {
    $password = $null
    $builder = $null
    [Array]::Clear($randomBytes, 0, $randomBytes.Length)
    [GC]::Collect()
}
