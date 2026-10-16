# Builds the APK with Android Studio's JDK and installs it on the connected phone.
#   -Release   signed release build (key.properties at the project root names the keystore)
#   -NoInstall build only
param([switch]$NoInstall, [switch]$Offline, [switch]$Release)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$env:JAVA_HOME = 'C:\Program Files\Android\Android Studio\jbr'
Push-Location $root
try {
    $type = if ($Release) { 'release' } else { 'debug' }
    $gargs = @(if ($Release) { 'assembleRelease' } else { 'assembleDebug' })
    if ($Offline) { $gargs += '--offline' }
    & .\gradlew.bat @gargs
    if ($LASTEXITCODE -ne 0) { throw "gradle failed ($LASTEXITCODE)" }
    $apk = Join-Path $root "app\build\outputs\apk\$type\app-$type.apk"
    Write-Host "APK: $apk"
    if (-not $NoInstall) {
        $adb = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
        & $adb install -r $apk
    }
} finally {
    Pop-Location
}
