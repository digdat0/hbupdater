# Catalog tooling

## `build_catalog.ps1`

Builds / extends `romfs/known_repos.json` (the bundled app catalog) from the
**fortheusers hb-appstore** inventory.

We use the app-store's `repo.json` **only as an inventory source** — it gives us
the GitHub repo URL, title, install path and author for ~470 homebrew apps. We
keep HBUpdater's own GitHub-releases update mechanism, so every imported repo is
**verified** to actually publish a usable `.nro` / `.ovl` / `.bin` release asset
before it's added. The app-store's own `version` and CDN package data (its
repackaged zips) are ignored on purpose.

### Usage

```powershell
# Stage A only (local, instant): parse repo.json -> tools/candidates.json
pwsh -File tools/build_catalog.ps1

# Stage A + B: verify each repo on GitHub, merge with existing catalog.
# Needs the gh CLI, authenticated (gh auth login). Resumable (verify_cache.json).
pwsh -File tools/build_catalog.ps1 -Verify

# Test the verify pass on a handful first:
pwsh -File tools/build_catalog.ps1 -Verify -Limit 20
```

Parameters: `-RepoJson <path>` (default `~/Desktop/repo.json`),
`-Existing <path>` (default `../romfs/known_repos.json`), `-OutDir <path>`,
`-Limit <n>`, `-ThrottleMs <ms>`.

### Outputs (in `tools/`)

- `candidates.json` — all GitHub candidates parsed from repo.json (unverified).
- `known_repos.merged.json` — existing catalog + newly **verified** apps, sorted
  by name. **Review this**, then copy over `romfs/known_repos.json` to ship it.
- `rejected.json` — repos we could not auto-update, with a reason
  (`no_release` / `no_asset` / `archive_only` / `error`).
- `verify_cache.json` — per-repo verification cache (delete to force a re-check).

### Promote a build

```powershell
Copy-Item tools/known_repos.merged.json romfs/known_repos.json
```

Then rebuild (remember: romfs files aren't Makefile deps, so force the repack):

```sh
rm -f HBUpdater.nro && make -j4
```

### Merge policy

Existing hand-curated entries always win (they have verified asset globs). The
import only **adds** repos not already present; it never overwrites an existing
entry. Re-running is safe and resumable.

## Credit

App inventory derived from the **Homebrew App Store** by fortheusers
(<https://github.com/fortheusers/hb-appstore>). HBUpdater is an updater, not an
app store, and does not use the app-store's CDN or distribution.
