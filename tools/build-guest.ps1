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
