<#
.SYNOPSIS
  Build / extend HBUpdater's known_repos.json catalog from the fortheusers
  hb-appstore repo.json inventory (https://github.com/fortheusers/hb-appstore).

  We use repo.json only as an INVENTORY source (it gives us the GitHub repo URL,
  title, install path, author). We keep our own GitHub-releases update mechanism,
  so every imported repo is VERIFIED to actually publish a usable .nro/.ovl/.bin
  release asset before it is added. repo.json's own version / CDN package data is
  ignored on purpose (that is the app-store's repackaged zip, not what we use).

.DESCRIPTION
  Stage A (always, local, no network):
    parse repo.json -> github candidates -> tools/candidates.json
  Stage B (-Verify, needs gh CLI + auth):
    query GitHub releases per repo, capture the real asset/kind/tag, then merge
    with the existing catalog (existing entries win) -> tools/known_repos.merged.json
    Rejected repos (no release / no installable asset / zip-only) -> tools/rejected.json
    Results are cached in tools/verify_cache.json so re-runs are resumable.

.EXAMPLE
  pwsh -File tools/build_catalog.ps1
  pwsh -File tools/build_catalog.ps1 -Verify
  pwsh -File tools/build_catalog.ps1 -Verify -Limit 20      # test on first 20
#>
[CmdletBinding()]
param(
    [string]$RepoJson = (Join-Path $HOME 'Desktop\repo.json'),
    [string]$Existing = (Join-Path $PSScriptRoot '..\romfs\known_repos.json'),
    [string]$OutDir   = $PSScriptRoot,
    [switch]$Verify,
    [int]$Limit = 0,
    [int]$ThrottleMs = 0
)

$ErrorActionPreference = 'Stop'

# ---- JSON output helpers (manual emitter: clean UTF-8, no \u escaping of
#      accented names, stable field order, no BOM) ---------------------------
function Write-NoBom([string]$path, [string]$text) {
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $text, $enc)
}

function Esc([string]$s) {
    if ($null -eq $s) { return '' }
    $sb = New-Object System.Text.StringBuilder
    foreach ($ch in $s.ToCharArray()) {
        switch ($ch) {
            '"'  { [void]$sb.Append('\"') }
            '\'  { [void]$sb.Append('\\') }
            "`n" { [void]$sb.Append('\n') }
            "`r" { [void]$sb.Append('\r') }
            "`t" { [void]$sb.Append('\t') }
            default {
                if ([int]$ch -lt 0x20) { [void]$sb.Append(('\u{0:x4}' -f [int]$ch)) }
                else { [void]$sb.Append($ch) }
            }
        }
    }
    $sb.ToString()
}

function Entry-Json($e) {
    $aliases = @($e.nacp_name | ForEach-Object { '"' + (Esc $_) + '"' }) -join ', '
    $pre = if ($e.prerelease) { 'true' } else { 'false' }
    $cat = if ($e.PSObject.Properties['category']) { Esc $e.category } else { '' }
@"
    {
      "name": "$(Esc $e.name)",
      "repo": "$(Esc $e.repo)",
      "nacp_name": [$aliases],
      "asset": "$(Esc $e.asset)",
      "asset_kind": "$(Esc $e.asset_kind)",
      "tag_example": "$(Esc $e.tag_example)",
      "prerelease": $pre,
      "author": "$(Esc $e.author)",
      "default_path": "$(Esc $e.default_path)",
      "category": "$cat"
    }
"@
}

function Catalog-Json($entries) {
    $items = @($entries | ForEach-Object { Entry-Json $_ }) -join ",`n"
    "{`n  `"apps`": [`n$items`n  ]`n}`n"
}

# ---- Stage A: parse repo.json into github candidates -----------------------
if (-not (Test-Path $RepoJson)) { throw "repo.json not found: $RepoJson" }
Write-Host "Reading $RepoJson ..."
$repo = Get-Content -Raw -Encoding UTF8 $RepoJson | ConvertFrom-Json
$pkgs = @($repo.packages)
$total = $pkgs.Count

$cands = [ordered]@{}
$skipNoGh = 0
$noPath = 0
foreach ($p in $pkgs) {
    $m = [regex]::Match([string]$p.url, 'github\.com/([^/\s]+)/([^/\s#?]+)')
    if (-not $m.Success) { $skipNoGh++; continue }
    $owner = $m.Groups[1].Value
    $name  = ($m.Groups[2].Value -replace '\.git$', '')
    $repoId = "$owner/$name"
    $key = $repoId.ToLower()

    $bin  = [string]$p.binary                      # e.g. /switch/2048.nro
    $base = if ($bin) { [System.IO.Path]::GetFileName($bin) } else { '' }
    $ext  = if ($base -match '\.([A-Za-z0-9]+)$') { $Matches[1].ToLower() } else { 'nro' }
    $dpath = if ($bin) { 'sdmc:' + $bin } else { '' }
    if (-not $dpath) { $noPath++ }

    $aliases = New-Object System.Collections.Generic.List[string]
    foreach ($a in @([string]$p.title, ($base -replace '\.[^.]+$', ''))) {
        if ($a -and -not $aliases.Contains($a)) { $aliases.Add($a) }
    }

    if ($cands.Contains($key)) {
        foreach ($a in $aliases) {
            if (-not ($cands[$key].nacp_name -contains $a)) { $cands[$key].nacp_name += $a }
        }
        continue
    }
    $cands[$key] = [pscustomobject]@{
        name         = [string]$p.title
        repo         = $repoId
        nacp_name    = @($aliases)
        asset        = $base        # best-guess pre-verify (overwritten by -Verify)
        asset_kind   = $ext
        tag_example  = ''
        prerelease   = $false
        author       = [string]$p.author
        default_path = $dpath
        category     = [string]$p.category
    }
}
$candList = @($cands.Values)
Write-NoBom (Join-Path $OutDir 'candidates.json') (Catalog-Json $candList)
Write-Host ("STAGE A: packages={0}  github_candidates={1}  skipped_non_github={2}  missing_install_path={3}" -f `
    $total, $candList.Count, $skipNoGh, $noPath)
Write-Host ("  -> {0}" -f (Join-Path $OutDir 'candidates.json'))

if (-not $Verify) {
    Write-Host "Done (Stage A only). Re-run with -Verify to confirm GitHub release assets and merge."
    return
}

# ---- Stage B: verify each repo against GitHub releases ---------------------
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "gh CLI not found on PATH (required for -Verify)."
}
& gh auth status *> $null
if ($LASTEXITCODE -ne 0) { throw "gh is not authenticated. Run: gh auth login" }

function Verify-Repo([string]$repoId, [string]$preferKind) {
    # gh writes to stderr on HTTP errors (e.g. 404 = no releases). In PS 5.1
    # that surfaces as a NativeCommandError which, under -ErrorActionPreference
    # Stop, would terminate the script. Make it non-terminating for this call
    # (the assignment is function-scoped) and rely on $LASTEXITCODE instead.
    $ErrorActionPreference = 'SilentlyContinue'
    $out = & gh api "repos/$repoId/releases?per_page=20" 2>$null
    $code = $LASTEXITCODE
    if ($code -ne 0 -or -not $out) {
        return [pscustomobject]@{ status='no_release'; asset=''; kind=''; tag=''; prerelease=$false }
    }
    try { $rels = @($out | ConvertFrom-Json) } catch {
        return [pscustomobject]@{ status='error'; asset=''; kind=''; tag=''; prerelease=$false }
    }
    if ($rels.Count -eq 0) {
        return [pscustomobject]@{ status='no_release'; asset=''; kind=''; tag=''; prerelease=$false }
    }
    foreach ($phase in @('stable','any')) {
        foreach ($rel in $rels) {
            if ($rel.draft) { continue }
            if ($phase -eq 'stable' -and $rel.prerelease) { continue }
            $assets = @($rel.assets)
            if ($assets.Count -eq 0) { continue }
            foreach ($want in @($preferKind, 'nro', 'ovl', 'bin')) {
                if (-not $want) { continue }
                $pick = $assets | Where-Object { $_.name -match ('\.' + [regex]::Escape($want) + '$') } | Select-Object -First 1
                if ($pick) {
                    $k = ($pick.name -replace '.*\.', '').ToLower()
                    return [pscustomobject]@{ status='ok'; asset=$pick.name; kind=$k; tag=[string]$rel.tag_name; prerelease=[bool]$rel.prerelease }
                }
            }
        }
    }
    $hasArchive = $false
    foreach ($rel in $rels) { foreach ($a in @($rel.assets)) { if ($a.name -match '\.(zip|7z)$') { $hasArchive = $true } } }
    [pscustomobject]@{ status=($(if ($hasArchive) { 'archive_only' } else { 'no_asset' })); asset=''; kind=''; tag=''; prerelease=$false }
}

$cachePath = Join-Path $OutDir 'verify_cache.json'
$cache = @{}
if (Test-Path $cachePath) {
    (Get-Content -Raw $cachePath | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $cache[$_.Name] = $_.Value }
}

$verified = New-Object System.Collections.Generic.List[object]
$rejected = New-Object System.Collections.Generic.List[object]
$i = 0
foreach ($c in $candList) {
    $i++
    if ($Limit -gt 0 -and $i -gt $Limit) { break }
    $key = $c.repo.ToLower()
    if ($cache.ContainsKey($key)) {
        $r = $cache[$key]
    } else {
        $r = Verify-Repo $c.repo $c.asset_kind
        $cache[$key] = $r
        if ($ThrottleMs -gt 0) { Start-Sleep -Milliseconds $ThrottleMs }
    }
    if ($r.status -eq 'ok') {
        $c.asset = $r.asset; $c.asset_kind = $r.kind; $c.tag_example = $r.tag; $c.prerelease = [bool]$r.prerelease
        $verified.Add($c)
    } else {
        $rejected.Add([pscustomobject]@{ repo = $c.repo; name = $c.name; reason = $r.status })
    }
    if (($i % 25) -eq 0) { Write-Host ("  verified {0}/{1} ..." -f $i, $candList.Count) }
}
Write-NoBom $cachePath ($cache | ConvertTo-Json -Depth 6)

# ---- Merge with existing catalog (existing wins) ---------------------------
$existingEntries = @()
if (Test-Path $Existing) {
    $ex = Get-Content -Raw -Encoding UTF8 $Existing | ConvertFrom-Json
    $existingEntries = @($ex.apps)
}
$exKeys = @{}
foreach ($e in $existingEntries) { $exKeys[([string]$e.repo).ToLower()] = $true }

$merged = New-Object System.Collections.Generic.List[object]
foreach ($e in $existingEntries) { $merged.Add($e) }   # keep verified hand-curated entries as-is
$added = 0
foreach ($v in $verified) {
    if ($exKeys.ContainsKey($v.repo.ToLower())) { continue }
    $merged.Add($v); $added++
}
$sorted = $merged | Sort-Object { ([string]$_.name).ToLower() }

Write-NoBom (Join-Path $OutDir 'known_repos.merged.json') (Catalog-Json $sorted)
Write-NoBom (Join-Path $OutDir 'rejected.json') ($rejected | ConvertTo-Json -Depth 4)

$byReason = ($rejected | Group-Object reason | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join '  '
Write-Host ("STAGE B: verified_ok={0}  rejected={1} [{2}]" -f $verified.Count, $rejected.Count, $byReason)
Write-Host ("MERGE:   existing_kept={0}  new_added={1}  total={2}" -f $existingEntries.Count, $added, $sorted.Count)
Write-Host ("  -> {0}  (review, then copy over romfs/known_repos.json)" -f (Join-Path $OutDir 'known_repos.merged.json'))
Write-Host ("  -> {0}  (apps we could not auto-update)" -f (Join-Path $OutDir 'rejected.json'))
