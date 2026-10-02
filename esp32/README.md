# PocketStation for M5Stack CoreS3

The same emulator as the web version, ported to C++ for the **CoreS3 + Faces Bottom3 + Gamepad3**. It boots your PocketStation games, saves where you left off, and syncs memory cards with PocketSync on your PC over Wi-Fi.

## First-time setup

1. **Wi-Fi (optional, for PC sync):** copy `data/config.example.json` to `data/config.json` and edit it (`config.json` is in `.gitignore`, so your Wi-Fi password is never published):
   ```json
   "wifi_ssid": "YourNetwork",
   "wifi_pass": "YourPassword",
   "server": "http://192.168.1.20:8765",
   ```
   Use the address PocketSync prints when it starts. If you started PocketSync with `--token`, put the same token in `"token"`. The time zone is already set to US Eastern.
2. Plug the CoreS3 in with USB-C. Then, in a terminal in this `esp32` folder:
   ```bash
   pio run -t uploadfs            # copies data/ (BIOS, config, games) to the device
   pio run -t upload -t monitor   # builds, flashes, then shows the serial log
   ```
   The first build downloads the ESP32 toolchain, which takes a few minutes. If the upload can't find the port, hold the reset button for about 3 seconds until the green LED flashes (download mode), then run it again.
3. Run `uploadfs` again only when you change something in `data/`. It **replaces everything on the device**, including save states and any game progress not yet synced to the PC, so sync first.

## Controls

| Gamepad3 | In a game | In the library / menus |
|---|---|---|
| D-pad | D-pad | move |
| A or B | PocketStation button | A = play / select, B = back |
| START | pause menu | sync & settings |
| SELECT | mute / unmute | change volume |

Without the Gamepad3, on-screen touch buttons appear under the screen, and you tap games in the list to play them.

Each memory card also gets a **"Menu: …"** entry at the bottom of the list. It opens the real PocketStation menus (clock, file browser), with the clock already set.

## Saving and syncing

* Leaving a game (START → *Save & back to library*) saves a snapshot, so the game resumes exactly where you were.
* When a game writes to its memory card (e.g. Chocobo World progress), the card is saved on the device within a few seconds.
* With Wi-Fi set up:
  * At power-on it sets the clock from the internet and pulls new or changed cards from your PC.
  * On leaving a game it sends a changed card back to the PC. PocketSync keeps a backup of the old one.
  * If a card changed in both places, it's flagged "changed on PC too" and nothing is overwritten. START → *Send changes, overwrite PC copy* forces the device's version.
* Cards in `data/cards` that are identical to ones on your PC are linked, not duplicated.

## Notes

* Serial monitor output includes `emu load NN%` every 5 seconds. If a game ever goes above about 90%, tell me which one.
* `test/` holds the desktop test harness: the C++ core checked frame-by-frame against the web version (`host.cpp`, `jsref.mjs`), plus save-state and title tests. It isn't part of the firmware.
* The BIOS (`data/bios.bin`) and memory cards (`data/cards/`) are not part of the repository: put your own dumps there before running `uploadfs`.
