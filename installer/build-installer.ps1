# Builds dist\BatteryHub-Setup-<version>.exe and dist\BatteryHub-<version>-portable.zip.
# Needs: BatteryHub.exe already built (make / build.bat). Inno Setup is used for the installer.
param([string]$Version = "1.0.0", [switch]$PortableOnly)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
& (Join-Path $root 'scripts\verify-windows-version.ps1') -ExpectedVersion $Version

# Bundled HeadsetControl: pinned release, verified against the SHA-256 GitHub publishes for it.
$hscVersion = "4.3.0"
$hscSha256  = "e371da043710ddb8fc70af77f2ccad1cd03f8c6f49ba423e08764ad7a94a3c40"
$hscDir = Join-Path $root "third_party\headsetcontrol"
$hscExe = Join-Path $hscDir "headsetcontrol.exe"
New-Item -ItemType Directory -Force $hscDir | Out-Null
if (-not (Test-Path $hscExe) -or (Get-FileHash $hscExe -Algorithm SHA256).Hash -ne $hscSha256) {
    Write-Host "Downloading HeadsetControl $hscVersion..."
    Invoke-WebRequest "https://github.com/Sapd/HeadsetControl/releases/download/$hscVersion/headsetcontrol-windows-x86_64.exe" -OutFile $hscExe
}
if ((Get-FileHash $hscExe -Algorithm SHA256).Hash -ne $hscSha256) {
    Remove-Item $hscExe
    throw "HeadsetControl checksum mismatch - refusing to bundle it."
}

# Valve OpenVR SDK v2.15.6 (the revision that supplied vendor/openvr/openvr_capi.h).
$openvrCommit = '0924064316de3effbcd1acf1e309182a2deb1c05'
$openvrSha256 = 'bab8ac6ef64e68a9ca53315b0014d131088584b2efdfa6db511d67ec03cfcb4a'
$openvrDir = Join-Path $root 'third_party\openvr'
$openvrDll = Join-Path $openvrDir 'openvr_api.dll'
New-Item -ItemType Directory -Force $openvrDir | Out-Null
if (!(Test-Path -LiteralPath $openvrDll) -or (Get-FileHash -LiteralPath $openvrDll -Algorithm SHA256).Hash -ne $openvrSha256) {
    Write-Host 'Downloading OpenVR SDK DLL...'
    Invoke-WebRequest "https://raw.githubusercontent.com/ValveSoftware/openvr/$openvrCommit/bin/win64/openvr_api.dll" -OutFile $openvrDll
}
if ((Get-FileHash -LiteralPath $openvrDll -Algorithm SHA256).Hash -ne $openvrSha256) {
    Remove-Item -LiteralPath $openvrDll
    throw 'OpenVR checksum mismatch - refusing to bundle it.'
}

if (-not (Test-Path (Join-Path $root "BatteryHub.exe"))) { throw "Build BatteryHub.exe first (make or build.bat)." }

$names = @()
if (!$PortableOnly) {
    $iscc = foreach ($v in 7, 6) {
        foreach ($base in ${env:ProgramFiles(x86)}, $env:ProgramFiles, "$env:LOCALAPPDATA\Programs") { "$base\Inno Setup $v\ISCC.exe" }
    }
    $iscc = $iscc | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { $iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source }
    if (-not $iscc) { throw "Inno Setup not found (https://jrsoftware.org/isdl.php)." }
    & $iscc "/DAppVersion=$Version" (Join-Path $PSScriptRoot "BatteryHub.iss")
    if ($LASTEXITCODE -ne 0) { throw "ISCC failed." }
    $names += "BatteryHub-Setup-$Version.exe"
}

# Portable zip: same files, no installer.
$stage = Join-Path $root "build\portable"
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item BatteryHub.exe, $hscExe, $openvrDll, installer\THIRD_PARTY_NOTICES.txt $stage
Copy-Item LICENSE (Join-Path $stage "LICENSE.txt")
Copy-Item vendor\openvr\LICENSE (Join-Path $stage 'OpenVR-LICENSE.txt')
$portableFiles = foreach ($name in @('BatteryHub.exe', 'headsetcontrol.exe', 'openvr_api.dll',
    'THIRD_PARTY_NOTICES.txt', 'LICENSE.txt', 'OpenVR-LICENSE.txt')) { Join-Path $stage $name }
New-Item -ItemType Directory -Force (Join-Path $root 'dist') | Out-Null
$portableName = "BatteryHub-$Version-portable.zip"
Compress-Archive -LiteralPath $portableFiles -DestinationPath (Join-Path 'dist' $portableName) -Force
$names += $portableName
# Checksums of what the app can install or users download. The maintainer signs this file on their own
# PC (scripts\sign-release.ps1) and uploads SHA256SUMS.txt.sig; the app only installs a signed setup.
# sha256sum format with LF line ends, so `sha256sum -c SHA256SUMS.txt` also works.
$sums = foreach ($name in $names) {
    "$((Get-FileHash (Join-Path 'dist' $name) -Algorithm SHA256).Hash.ToLower())  $name"
}
[IO.File]::WriteAllText((Join-Path $root 'dist\SHA256SUMS.txt'), (($sums -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
Write-Host "Done: $($names -join ', ') and SHA256SUMS.txt in dist\"
