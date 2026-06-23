# Homebrew Updater (HBUpdater)

A small Nintendo Switch homebrew app that keeps your **other** homebrew up to
date. Track each app by its GitHub repo (`owner/name`) and the `.nro` path on
your SD card; HBUpdater checks each repo's latest release and overwrites the
`.nro` in place when a newer version is available. Built with devkitPro / libnx
and the [Plutonium](https://github.com/XorTroll/Plutonium) UI.

> Early scaffold. It only touches the `.nro` files you tell it to, and only
> downloads release assets from the GitHub repos you list. Use at your own risk.

---

## Features (0.1.0)

- Track a list of homebrew apps: **GitHub `owner/name`** + the **`.nro` path**.
- **Check** each app against its latest GitHub release (tag vs. last installed).
- **Update** in place when a newer release is available — downloads the release's
  `.nro` asset and overwrites the file.
- Status per app: up to date / update available / not checked / failed.
- Config stored at `sdmc:/switch/HBUpdater/apps.json` (editable by hand).

## Controls

| Key | Action |
|-----|--------|
| D-pad / left stick | move (hold to repeat) |
| A | check the selected app, and offer to update if newer |
| X | check all apps |
| Y | add an app (repo + path + name) |
| − | stop tracking the selected app |
| ZL / ZR | page |
| + | exit |

## Install

1. Copy `HBUpdater.nro` to `sdmc:/switch/HBUpdater/HBUpdater.nro`.
2. Launch from the homebrew menu.

## apps.json

```json
{
  "apps": [
    { "name": "My App", "repo": "owner/name",
      "path": "sdmc:/switch/MyApp/MyApp.nro", "version": "" }
  ]
}
```

- `repo` — GitHub `owner/name`; its **latest release** must attach a `.nro`.
- `path` — the `.nro` on your SD card that gets overwritten.
- `version` — last-installed release tag (HBUpdater fills this in after updating).

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

## Roadmap / TODO

- Run network checks/downloads on a background thread (the UI currently blocks
  during a check/update — same pattern already solved in TicoDL+).
- Self-update HBUpdater itself (the plumbing — `argv[0]` launch path, update
  endpoint — is already stubbed in).
- Optional: keep a `.previous` backup before overwriting; "update all".

## License

MIT — see [LICENSE](LICENSE). UI by [Plutonium](https://github.com/XorTroll/Plutonium)
(XorTroll, MIT).

## Credits

- Built with [devkitPro / libnx](https://devkitpro.org/),
  [libcurl](https://curl.se/libcurl/), and
  [Plutonium](https://github.com/XorTroll/Plutonium) by XorTroll. JSON via the
  vendored [jsmn](https://github.com/zserge/jsmn).
- Backend (net / update / json / fs) shared with TicoDL+.
