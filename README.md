# Homebrew Updater (HBUpdater)

> **ALPHA SOFTWARE — USE AT YOUR OWN RISK.**
> This project is in early development. Features may be incomplete, unstable, or change without notice. Back up your SD card before use.

A Nintendo Switch homebrew app that keeps your **other** homebrew up to date.
HBUpdater scans your SD card for installed `.nro` apps, matches them against a
catalog of known GitHub repos, and lets you check for updates and install them
in place. Built with devkitPro / libnx and
[Plutonium](https://github.com/XorTroll/Plutonium) UI.

---

## Features

- **Auto-detect installed apps** — scans the SD for `.nro` files and matches
  them to a catalog of known GitHub repos. No manual setup needed.
- **Check & update** — checks each app's latest GitHub release and downloads
  the new asset in place (`.nro`, `.ovl`, `.bin`, or `.zip`).
- **Background worker** — all network I/O runs on a worker thread; the UI
  stays responsive with live progress.
- **Catalog system** — a bundled catalog of known repos ships with the app;
  OTA catalog updates pull the latest list from GitHub.
- **Pin / hold version** — pin an app to skip it during check-all.
- **Search / filter** — press Y on the home screen to filter by name; B clears.
- **Pre-update diff summary** — before installing, a dialog shows installed vs
  latest version, asset size, kind, and target path.
- **Update history timeline** — a parsed, reverse-chronological view of every
  update you've performed (Settings → View logs → Update history).
- **Contextual footer** — the home screen footer color-codes itself: green when
  all apps are up to date, amber when updates are available, red on failures.
- **Exclude / re-include** — hide apps from the home list without deleting
  them; restore anytime from Settings → Excluded apps.
- **Backup & revert** — automatic pre-update snapshots let you roll back to any
  prior version. Manage per-app or browse all backups.
- **Self-update** — HBUpdater can update itself from its own GitHub releases.
- **Manual repo add** — track any GitHub repo by entering `owner/repo` in
  Settings, even if it isn't in the catalog.
- **GitHub token** — paste a personal access token in Settings to raise the API
  rate limit from 60 to 5,000 requests/hour.
- **File install controls** — overlays, sysmodules, and payloads are gated
  behind opt-in toggles in Settings → File install.
- **Sorted lists** — catalog, excluded, and home lists are sorted A–Z.

## Controls

| Key | Home | Other screens |
|-----|------|---------------|
| A | action menu (check, update, pin, backups, reinstall) | select / info |
| X | check all apps | — |
| Y | search / filter by name | — |
| B | clear filter (or no-op) | back |
| R | open settings | — |
| − | exclude selected app | — |
| ZL / ZR | page up / down | page up / down |
| + | exit | exit |

## Install

1. Copy `HBUpdater.nro` to `sdmc:/switch/HBUpdater/HBUpdater.nro`.
2. Launch from the homebrew menu.

On first launch HBUpdater scans your SD, matches installed apps to the catalog,
and offers to check for updates.

## Configuration

Config lives at `sdmc:/switch/HBUpdater/`:

| File | Purpose |
|------|---------|
| `apps.json` | tracked apps (auto-managed, hand-editable) |
| `settings.json` | preferences, GitHub token, toggle states |
| `excludes.json` | repos hidden from the home list |
| `catalog.json` | OTA catalog cache (auto-updated) |
| `history.log` | update history (append-only) |
| `debug.log` | network/install debug log |

## Building from source

Built on [Plutonium](https://github.com/XorTroll/Plutonium) (SDL2) with the
devkitPro toolchain. Plutonium is a git submodule and is built automatically.

```sh
git clone --recursive <repo-url>
cd hbupdater
make
```

Prerequisites: devkitPro + `switch-dev`, plus the portlibs:

```sh
dkp-pacman -S switch-curl switch-zlib switch-sdl2 switch-sdl2_ttf \
              switch-sdl2_image switch-sdl2_gfx switch-sdl2_mixer
```

> Networking (GitHub API + downloads) uses the libnx `ssl` backend and only
> works on **real hardware** — emulators that stub `ssl` will fail HTTPS.

## License

MIT — see [LICENSE](LICENSE). UI by [Plutonium](https://github.com/XorTroll/Plutonium)
(XorTroll, MIT).

## Credits

- Built with [devkitPro / libnx](https://devkitpro.org/),
  [libcurl](https://curl.se/libcurl/), and
  [Plutonium](https://github.com/XorTroll/Plutonium) by XorTroll. JSON via the
  vendored [jsmn](https://github.com/zserge/jsmn).
- Backend (net / update / json / fs) shared with TicoDL+.
