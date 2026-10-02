# Meshtastic modules

Our own code that runs **inside** Meshtastic firmware. Each folder here is a drop-in module: copy it into the firmware's `src/modules/optional/` folder and the firmware build picks it up by itself. Nothing in Meshtastic's own files is edited, so updating to a newer Meshtastic is just a `git pull`.

## StreetPass

Keeps a log of every Meshtastic node the device hears **directly** over LoRa (not relayed, not via the internet), like StreetPass on the 3DS.

* Hearing a node for the first time pops up a "StreetPass!" banner.
* Hearing it again after 30 quiet minutes counts as another pass.
* It also counts nodes on channels it can't read, shown by the last 4 digits of their ID.
* A **StreetPass** page is added to the screen pages: nodes met, total passes, then the most recent nodes with pass count, how long ago, and signal.
* The log (up to 64 nodes) is saved on the device and survives restarts.
* The serial log prints a line for every pass.

## PocketStation

Plays PocketStation games on a page of its own, using the same emulator core as the web and CoreS3 versions. The emulator runs on the ESP32-S3's second CPU core, so the mesh keeps working while you play.

* The **PocketStation** page lists the games. Up/down (`;` and `.`) choose, Enter starts.
* While playing: the arrow keys (`;` `.` `,` `/`) are the D-pad, Enter or space is the action button, Esc (`` ` ``) or Backspace goes back to the list.
* Going back pauses the game; choosing it again carries on where it was (marked `*` in the list). Choosing another game replaces it.
* When a game saves to its memory card, only the changed 8 KB blocks are kept on the device (in `/prefs/pstation-*.sav`) and they come back after a restart. Re-packing a changed card from the PC discards the progress saved on the device for that card.
* The serial log prints `PocketStation: emulator load NN%, free heap NNNNN` every 10 seconds while playing.

Limits of this first version: no sound, no sync with the PC, and a paused game is lost when the device restarts (card saves are kept). The keyboard only reports a key when it is released, so every press is a short tap; games that need a held button will be awkward. Letter keys open Meshtastic's message composer, so keep to the keys above.

The BIOS and games are built into the firmware from your own dumps by `pack.py`, which writes `psdata.cpp` into the module folder inside the firmware source. That file contains your BIOS and games: never commit or share it, or a firmware file built with it.

## Build and flash: Cardputer ADV + LoRa cap

Flashing replaces the Meshtastic app on the Cardputer with one built here. Settings and channels are normally kept. To go back to stock, use flasher.meshtastic.org.

1. Get the firmware source (once, about 40 MB). It must live in a folder with **no spaces anywhere in its path**, or the build stops with "Detected a whitespace character in project paths":
   ```bash
   cd ~
   git clone --depth 1 --branch v2.8.1.8e6a88d --recurse-submodules --shallow-submodules \
       https://github.com/meshtastic/firmware.git meshtastic-firmware
   ```
2. Copy the modules in and pack your games (repeat this whenever a module or a card changes):
   ```bash
   mkdir -p ~/meshtastic-firmware/src/modules/optional
   cd ~/"claude projects/WEB POCKETSTATION/web-pocketstation/meshtastic"
   cp -r StreetPass PocketStation ~/meshtastic-firmware/src/modules/optional/
   python3 pack.py ~/meshtastic-firmware/src/modules/optional/PocketStation
   ```
   `pack.py` takes the BIOS and cards from `esp32/data` (use `--bios` and `--cards` for other locations). If the build later says the program is too big, pack fewer cards.
3. Set up a separate PlatformIO just for Meshtastic (once). This is the same flavour and version Meshtastic's own ESP32 builds use, kept apart from the one used for the PocketStation projects:
   ```bash
   python3 -m venv ~/pio-meshtastic
   ~/pio-meshtastic/bin/pip install pioarduino==6.2.0
   ```
   Every command below starts with this prefix, so save it as a shortcut for the terminal session:
   ```bash
   mpio() { PLATFORMIO_CORE_DIR=~/.platformio-meshtastic ~/pio-meshtastic/bin/pio "$@"; }
   ```
4. Build. The first build downloads Meshtastic's toolchain into `~/.platformio-meshtastic` and takes a while:
   ```bash
   cd ~/meshtastic-firmware
   mpio pkg install -e m5stack-cardputer-adv
   sed -i 's#"package-version": "4.40801.0"#"package-version": "4.41101.0"#; s#download/0.0.1/scons-4.8.1.zip#download/0.0.1/scons-4.11.1.zip#' \
       ~/.platformio-meshtastic/platforms/espressif32*/platform.json
   rm -rf ~/.platformio-meshtastic/packages/tool-scons*
   mpio run -e m5stack-cardputer-adv
   ```
   Near the top of the output you should see `optional-modules: PocketStation, StreetPass`.

   The `sed` and `rm` lines are a one-time fix. Meshtastic 2.8.1's build platform still expects the build tool SCons 4.8.1, PlatformIO 6.2.0 runs 4.11.1, and the platform deletes the "wrong" one in the middle of the build (it stops with `No module named 'SCons.Tool.FortranCommon'`). The `sed` line tells the platform to expect 4.11.1, which is what the current pioarduino platform does.
5. Flash. Plug the Cardputer in, then:
   ```bash
   mpio run -e m5stack-cardputer-adv -t upload --upload-port /dev/ttyACM0
   ```
   If it can't connect: switch the Cardputer off, hold the **G0** button, switch it on while still holding, then run the command again. Switch it off and on afterwards.
6. Watch the log:
   ```bash
   mpio device monitor -p /dev/ttyACM0 -b 115200
   ```
   Look for `StreetPass: 0 nodes met, 0 passes` at start-up, and `StreetPass: NEW !xxxxxxxx ...` when another node is heard.

Notes

* To see a pass you need a second Meshtastic node nearby on the same region and LoRa preset. The Unit C6L works if you put Meshtastic back on it with flasher.meshtastic.org.

## Tests

Both modules have tests that run on the PC without a device:

* `bash tests/streetpass/run.sh` should print `ALL TESTS PASSED`.
* `bash tests/pocketstation/run.sh` plays real games through the module with a fake screen and keyboard, once on the main loop and once with the emulator on its own thread. It needs the BIOS and cards in `esp32/data` and takes about a minute.

`PocketStation/pscore.*` and `cards.*` are copies of the files in `esp32/src`; keep the two sets identical.
