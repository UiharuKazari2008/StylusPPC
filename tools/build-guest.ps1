# Builds the Windows Mobile side of WMDroid (guest\Helper -> app assets) with the host's .NET 3.5
# C# compiler against the Compact Framework 3.5 assemblies taken from the WM 6.5.3 ROM.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cf = Join-Path $root '..\WM 6.5.3 Professional Emulator\SDK\CF35'
$csc = "$env:WINDIR\Microsoft.NET\Framework\v3.5\csc.exe"
$out = Join-Path $root 'app\src\main\assets\guest\WMDroid.exe'
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$refs = 'mscorlib','System','System.Core','System.Drawing','System.Windows.Forms','System.Xml','System.Data','Microsoft.WindowsCE.Forms',
        'Microsoft.WindowsMobile','Microsoft.WindowsMobile.PocketOutlook','System.Net.IrDA' |
    ForEach-Object { "/r:$(Join-Path $cf "$_.dll")" }
# Each folder under guest\ is one program: Helper -> WMDroid.exe (the helper), others by folder name.
foreach ($dir in Get-ChildItem (Join-Path $root 'guest') -Directory) {
    # Discord is withheld: do not put Discord.exe back into the APK.
    if ($dir.Name -eq 'Discord') { continue }
    $name = if ($dir.Name -eq 'Helper') { 'WMDroid' } else { $dir.Name }
    $exe = Join-Path (Split-Path $out) "$name.exe"
    $src = @(Get-ChildItem $dir.FullName -Filter *.cs | ForEach-Object FullName)
    $extra = @()
    if ($dir.Name -eq 'Helper') {
        # MAPIdotnet (third_party/mapidotnet, no license, "feel free"): WM's messaging store for text
        # messages (Sms.cs). Its native half, MAPIlib.dll, comes from tools/make-mapilib.py; OpenNETCF.dll
        # (its stream types) ships next to the helper.
        $mapi = Join-Path $root 'third_party\mapidotnet\mapidotnet-code-r20\MAPIdotnet\MAPIdotnet'
        $src += @(Get-ChildItem $mapi -Filter *.cs | ForEach-Object FullName)
        $src += @(Get-ChildItem (Join-Path $mapi 'cemapi') -Filter *.cs | ForEach-Object FullName)
        $onetcf = Join-Path $root 'third_party\mapidotnet\mapidotnet-code-r20\OpenNETCF.dll'
        $extra = @("/r:$onetcf")
        Copy-Item $onetcf (Join-Path (Split-Path $out) 'OpenNETCF.dll') -Force
    }
    # Native programs (guest\Autorun, the card's bootstrap) have no C#: tools/make-autorun.py builds them.
    if ($src.Count -eq 0) { continue }
    # The program's icon (tools/make-icons.py draws them), if it has one.
    $icon = @()
    $ico = Join-Path $dir.FullName 'app.ico'
    if (Test-Path $ico) { $icon = @("/win32icon:$ico") }
    & $csc /nologo /noconfig /nostdlib+ /target:winexe /optimize+ /nowarn:618,1030,0168,0169,0219,0414,0649 "/out:$exe" @icon @refs @extra @src
    if ($LASTEXITCODE -ne 0) { throw "csc failed for $name ($LASTEXITCODE)" }
    Write-Host "built $exe"
}

# Windows Mobile 2003 is CE 4.2. CF 3.5 will not load there, so the same sources are built again
# against the CF 2.0 assemblies inside NETCFv2.wce4.ARMV4.cab (/define:CF2). CF 3.5 runs that
# binary too, but the WM 5 and later asset stays the CF 3.5 build above.
$cab2 = Join-Path (Split-Path $out) 'NETCFv2.wce4.ARMV4.cab'
$seven = @(
    "$env:ProgramFiles\7-Zip\7z.exe",
    "${env:ProgramFiles(x86)}\7-Zip\7z.exe",
    "$env:USERPROFILE\scoop\shims\7z.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not (Test-Path $cab2)) { Write-Host "no $cab2; skipping the CF 2.0 helper"; return }
if (-not $seven) { throw "7z is required to read $cab2" }
$ref2 = Join-Path $root 'build\netcf2-ref'
New-Item -ItemType Directory -Force $ref2 | Out-Null
$unpack = Join-Path $ref2 'cab'
if (Test-Path $unpack) { Remove-Item $unpack -Recurse -Force }
New-Item -ItemType Directory -Force $unpack | Out-Null
& $seven e "-o$unpack" -y $cab2 mscorlib.007 00System.008 SYSTEM~3.009 SYSTEM~4.012 SY40C7~1.014 SY4317~1.016 MICROS~1.018 SYSTEM~1.015 | Out-Null
$names = @{
    'mscorlib.007' = 'mscorlib.dll'; '00System.008' = 'System.dll'; 'SYSTEM~3.009' = 'System.Drawing.dll'
    'SYSTEM~4.012' = 'System.Windows.Forms.dll'; 'SY40C7~1.014' = 'System.Xml.dll'; 'SY4317~1.016' = 'System.Data.dll'
    'MICROS~1.018' = 'Microsoft.WindowsCE.Forms.dll'; 'SYSTEM~1.015' = 'System.Net.IrDA.dll'
}
foreach ($k in $names.Keys) { Copy-Item (Join-Path $unpack $k) (Join-Path $ref2 $names[$k]) -Force }
$refs2 = $names.Values | ForEach-Object { "/r:$(Join-Path $ref2 $_)" }
$helper = Get-ChildItem (Join-Path $root 'guest\Helper') -Filter *.cs | ForEach-Object FullName
$exe2 = Join-Path (Split-Path $out) 'WMDroid2003.exe'
& $csc /nologo /noconfig /nostdlib+ /target:winexe /optimize+ /define:CF2 /nowarn:618,1030,0168,0169,0219,0414,0649 "/out:$exe2" @refs2 @helper
if ($LASTEXITCODE -ne 0) { throw "csc failed for WMDroid2003 ($LASTEXITCODE)" }
Write-Host "built $exe2"
foreach ($dir in @('GpsTest','InfraLobby','SendBluetooth','IrTest')) {
    $src = @(Get-ChildItem (Join-Path $root "guest\$dir") -Filter *.cs | ForEach-Object FullName)
    if ($src.Count -eq 0) { continue }
    $exe = Join-Path (Split-Path $out) "$dir`2003.exe"
    & $csc /nologo /noconfig /nostdlib+ /target:winexe /optimize+ /define:CF2 /nowarn:618,1030,0168,0169,0219,0414,0649 "/out:$exe" @refs2 @src
    if ($LASTEXITCODE -ne 0) { throw "csc failed for $dir CF2 ($LASTEXITCODE)" }
    Write-Host "built $exe"
}
