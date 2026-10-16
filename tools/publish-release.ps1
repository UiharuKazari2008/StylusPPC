# Publishes a tagged release with its APK to yozora (Gitea/Forgejo) and GitHub.
#   $env:YOZORA_TOKEN = "..."   # a yozora access token with repository write scope (your terminal only)
#   gh auth login               # once, for GitHub
#   .\tools\publish-release.ps1 -Version 1.0.2
param([Parameter(Mandatory)][string]$Version, [string]$Repo = 'UiharuKazari2008/StylusPPC')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tag = "v$Version"
$apk = Join-Path $root "dist\PocketStylus-$Version.apk"
$notesFile = Join-Path $root "dist\RELEASE-$tag.md"
if (-not (Test-Path $apk)) { throw "missing $apk" }
$notes = if (Test-Path $notesFile) { Get-Content $notesFile -Raw } else { "PocketStylus $Version" }

# yozora
if ($env:YOZORA_TOKEN) {
    $api = "https://yozora.bluesteel.737.jp.net/api/v1/repos/$Repo/releases"
    $h = @{ Authorization = "token $env:YOZORA_TOKEN" }
    $existing = try { Invoke-RestMethod -Headers $h -Uri "$api/tags/$tag" } catch { $null }
    $rel = if ($existing) { $existing } else {
        Invoke-RestMethod -Method Post -Headers $h -Uri $api -ContentType 'application/json' `
            -Body (@{ tag_name = $tag; name = "PocketStylus $Version"; body = $notes } | ConvertTo-Json)
    }
    $name = Split-Path $apk -Leaf
    Invoke-RestMethod -Method Post -Headers $h -Uri "$api/$($rel.id)/assets?name=$name" -Form @{ attachment = Get-Item $apk } | Out-Null
    Write-Host "yozora: https://yozora.bluesteel.737.jp.net/$Repo/releases/tag/$tag"
} else { Write-Host "yozora: skipped (set YOZORA_TOKEN)" }

# GitHub
if (Get-Command gh -ErrorAction SilentlyContinue) {
    gh release view $tag --repo $Repo *> $null
    if ($LASTEXITCODE -eq 0) { gh release upload $tag $apk --repo $Repo --clobber }
    else { gh release create $tag $apk --repo $Repo --title "PocketStylus $Version" --notes-file $notesFile }
    Write-Host "GitHub: https://github.com/$Repo/releases/tag/$tag"
} else { Write-Host "GitHub: skipped (install and log in to gh)" }
