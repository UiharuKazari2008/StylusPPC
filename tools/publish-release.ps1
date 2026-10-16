# Publishes a tagged release with its APK to yozora (Gitea/Forgejo) and GitHub.
#   .\tools\publish-release.ps1 -Version 1.0.2
# yozora uses Git's saved sign-in (or YOZORA_TOKEN); GitHub uses gh, with a browser login if needed.
param([Parameter(Mandatory)][string]$Version, [string]$Repo = 'UiharuKazari2008/StylusPPC')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tag = "v$Version"
$apk = Join-Path $root "dist\PocketStylus-$Version.apk"
$notesFile = Join-Path $root "dist\RELEASE-$tag.md"
if (-not (Test-Path $apk)) { throw "missing $apk" }
$notes = if (Test-Path $notesFile) { Get-Content $notesFile -Raw } else { "PocketStylus $Version" }

# yozora (Gitea): YOZORA_TOKEN if set, else the sign-in Git already uses for pushes (Git
# Credential Manager, which opens its browser approval if it has none). Each form Gitea accepts is
# tried against /user; nothing is printed. A failure here doesn't stop the GitHub step.
$yozoraHost = 'yozora.bluesteel.737.jp.net'
try {
    $candidates = @()
    if ($env:YOZORA_TOKEN) { $candidates += @{ Authorization = "token $env:YOZORA_TOKEN" } }
    $cred = "protocol=https`nhost=$yozoraHost`n`n" | git credential fill 2>$null
    $user = ($cred | Where-Object { $_ -like 'username=*' }) -replace '^username=', ''
    $pass = ($cred | Where-Object { $_ -like 'password=*' }) -replace '^password=', ''
    if ($pass) {
        $candidates += @{ Authorization = "token $pass" }
        $candidates += @{ Authorization = "Bearer $pass" }
        $candidates += @{ Authorization = "Basic " + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("${user}:$pass")) }
    }
    $h = $null
    foreach ($c in $candidates) {
        try { Invoke-RestMethod -Headers $c -Uri "https://$yozoraHost/api/v1/user" | Out-Null; $h = $c; break } catch { }
    }
    if (-not $h) { throw "no working sign-in (set YOZORA_TOKEN, or push once so Git saves one)" }
    $api = "https://$yozoraHost/api/v1/repos/$Repo/releases"
    $existing = try { Invoke-RestMethod -Uri "$api/tags/$tag" } catch { $null }
    $rel = if ($existing) { $existing } else {
        Invoke-RestMethod -Method Post -Headers $h -Uri $api -ContentType 'application/json' `
            -Body (@{ tag_name = $tag; name = "PocketStylus $Version"; body = $notes } | ConvertTo-Json)
    }
    # Gitea's attachment types here don't include .apk (the upload is cut off): attach it zipped,
    # under the versioned name and a fixed one (PocketStylus.apk.zip) for the README's latest link.
    $zip = "$apk.zip"
    Compress-Archive -Path $apk -DestinationPath $zip -Force
    foreach ($name in @((Split-Path $zip -Leaf), 'PocketStylus.apk.zip')) {
        if (-not ($rel.assets | Where-Object name -eq $name)) {
            Invoke-RestMethod -Method Post -Headers $h -Uri "$api/$($rel.id)/assets?name=$name" -Form @{ attachment = Get-Item $zip } | Out-Null
        }
    }
    Write-Host "yozora: https://$yozoraHost/$Repo/releases/tag/$tag"
} catch { Write-Host "yozora: failed: $($_.Exception.Message)" }

# GitHub: the gh CLI; signs in through the browser (one-time code) when needed.
if (Get-Command gh -ErrorAction SilentlyContinue) {
    gh auth status *> $null
    if ($LASTEXITCODE -ne 0) { gh auth login --hostname github.com --web --git-protocol https }
    # Also under a fixed name, so .../releases/latest/download/PocketStylus.apk always works.
    $fixed = Join-Path (Split-Path $apk) 'PocketStylus.apk'
    Copy-Item $apk $fixed -Force
    gh release view $tag --repo $Repo *> $null
    if ($LASTEXITCODE -eq 0) { gh release upload $tag $apk $fixed --repo $Repo --clobber }
    else { gh release create $tag $apk $fixed --repo $Repo --title "PocketStylus $Version" --notes-file $notesFile --latest }
    Write-Host "GitHub: https://github.com/$Repo/releases/tag/$tag"
} else { Write-Host "GitHub: skipped (install and log in to gh)" }
