# Web PocketStation + PocketSync

A PocketStation emulator that runs in a phone or desktop browser, written in plain JavaScript with no dependencies. It installs as an app, works offline, and has touch buttons, sound, the red LED and automatic resume.

**No BIOS and no games are included.** You load your own BIOS dump and memory cards; they are stored in your browser and never uploaded anywhere.

There are two ways to use it:

* **As a website** (this repo publishes one with GitHub Pages; the address is in the repo's *About* box). Open it, tap **Choose BIOS file**, then **Import card or game file…**. Everything stays on that device.
* **From your own Linux PC with PocketSync**, which serves the app to your phone and keeps its memory cards in sync with DuckStation. The rest of this README covers that.

```
 DuckStation ──writes──▶ memcards/*.mcd ◀──watches── PocketSync (on your PC) ──Wi-Fi/Tailscale──▶ phone browser
                                          ◀──writes back (with backups)──────────────────────────  (PocketStation emulator)
```

* **web/**: the PocketStation emulator and phone UI. It runs the real PocketStation BIOS on an ARM7TDMI emulator written for this project, with touch buttons, sound, the red LED and save states.
* **companion/pocketsync.py**: the "plugin" side. It watches DuckStation's memory cards, keeps a history of every version, serves the app to your phone, and accepts changed cards back.

DuckStation has no plugin system, so PocketSync runs next to it. In practice it acts like a PocketStation in the memory card slot. When a game saves a mini-game or data to the card (e.g. FF8 → Chocobo World), it shows up on your phone within a couple of seconds.

## Quick start

1. Run it once to try it:
   ```bash
   bash run.sh
   ```
   It prints addresses like `http://192.168.1.20:8765`. Open one on your phone, on the same Wi-Fi.
2. On the phone, tap **Choose BIOS file** and pick your PocketStation BIOS (`J110.bin`, recommended). It is stored only on the phone. Alternatively, pass the BIOS to the installer (below) and the phone fetches it from your PC automatically.
3. Tap a game. **Add to Home Screen** from your browser menu makes it full-screen like an app.

### Install as a background service (starts when you log in)

```bash
bash install.sh "/path/to/J110.bin"         # BIOS argument optional
bash install.sh "/path/to/J110.bin" --token mysecret   # require a token (recommended with Tailscale/public Wi-Fi)
```

### Away from home

Install [Tailscale](https://tailscale.com) on the PC and the phone (free), then open `http://<pc-tailscale-ip>:8765`. Use `--token` so only you can get in. Open the link once as `http://…:8765/?token=mysecret` and the phone remembers it.

## Where your games come from

* **DuckStation cards** (`~/.local/share/duckstation/memcards`, plus the Flatpak location) are found automatically, both shared and per-game cards.
* **Other cards** (`.mcd .mcr .gme .mem .vgs …`): drop them into `~/.local/share/pocketsync/cards/` and they appear on the phone.
* **Single game files** (raw `SC…` files like `Tetris.bin`, `.mcs`, `.psx`): use **Import card or game file…** on the phone.

## Using it

| | |
|---|---|
| Tap a game | Boots straight into it. The clock is set from your phone automatically, so the BIOS date screen is skipped. |
| ← back | Saves a snapshot. Next time the game resumes exactly where you left it. |
| ⋯ → PocketStation menu | The real BIOS menus (clock, alarm, file viewer, volume). |
| "Send to PC" | Appears after a game saves to its memory card (e.g. Chocobo World progress). It writes the card back to DuckStation's file. The old version is backed up first. |
| Keyboard | Arrow keys + Z / Space / Enter (for testing on a desktop browser). |

**Conflicts:** if the card changed on the PC *and* the phone since the last sync, you're asked which one to keep. Either way, every version is stored in `~/.local/share/pocketsync/history/<card>/`.

**Tip:** close the game in DuckStation before tapping *Send to PC*. DuckStation keeps the card in memory and can write over it on its next save.

## Tested

With the retail J110 BIOS: Astro Fighter, Curling, Drop Zone, World Time (Pocket MuuMuu), FF8 Chocobo World, Tetris and Tic-Tac-Toe all boot and play. Sound, flash saves, resume from snapshot, and phone→PC write-back with conflict detection all work.

## Developer notes

* `web/core/arm7.js`: ARM7TDMI interpreter (ARM + Thumb), no dependencies, runs in browsers and Node.
* `web/core/pocketstation.js`: hardware (memory map, FLASH banking/programming, IRQ/FIQ, timers, RTC, LCD, DAC audio, sleep), plus `bootFile()` (direct launch via the kernel's own SWI 08h/09h) and `saveState()`/`loadState()`.
* `web/core/memcard.js`: memory card parsing, icons, and container formats.
* `tests/`: Node scripts. Put `J110.bin` in `tests/` and your games in `../games`, then run e.g. `node tests/smoke.mjs` or `node tests/boot_game.mjs card.gme 1`.

The core is plain JS with no DOM dependencies, so it can also run several PocketStations at once (one object each). The planned ESP32 port can follow the same structure in C.

## Roadmap ideas

* Multiple PocketStations running at once, with push notifications (e.g. Chocobo World events, PocketStation alarms).
* Infrared link between emulated PocketStations (Chocobo World multiplayer).
* ESP32 handheld build (the core is about 1,500 lines and fits in an ESP32-S3's RAM).

## Publishing your own copy

`bash publish.sh` creates a public GitHub repo from this folder, pushes the code to `main`, pushes the `web/` folder to a `gh-pages` branch and switches on GitHub Pages for it (it needs `git` and the GitHub CLI `gh`, logged in).

To publish a change later: `git add -A && git commit -m "what changed" && bash publish.sh`.

By hand: create an empty public repo, `git init -b main && git add -A && git commit -m "Initial public release" && git remote add origin <repo url> && git push -u origin main`, then `git push origin "$(git subtree split --prefix web main)":refs/heads/gh-pages --force`, and set *Settings → Pages → Deploy from a branch* to `gh-pages`, `/ (root)`.

`.gitignore` keeps the BIOS, memory cards, game files and `esp32/data/config.json` (your Wi-Fi password) out of the repo. Do not add them.

## Licence

Web PocketStation is free software under the **GNU General Public License, version 3 or later** (see `LICENSE`). It comes with no warranty. Hardware reference: the "Pocketstation" chapters of psx-spx.

This is an unofficial fan project. It is not affiliated with, endorsed by or sponsored by Sony Interactive Entertainment. "PocketStation" and "PlayStation" are trademarks of their owner and are used here only to say what the emulator is compatible with.
