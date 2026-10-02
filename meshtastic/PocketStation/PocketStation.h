// PocketStation: plays PocketStation games inside Meshtastic, on a screen page of its own.
// Drop-in optional module: lives in src/modules/optional/PocketStation/ and needs no edits elsewhere.
// The BIOS and games come from psdata.cpp, which pack.py generates from your own dumps.
#pragma once

void setupPocketStation();
