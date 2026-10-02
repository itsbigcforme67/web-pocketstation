// Build options for the emulator core (pscore) when it runs inside Meshtastic, where RAM is tight.
#pragma once
#define PS_FLASH_BLOCKS 1 // read the card from the firmware image in 8 KiB blocks, copy a block only when written
#define PS_SMALL_TABLES 1 // decode tables take 5 KiB (not 20) and are allocated when a game starts
#define PS_NO_IRAM 1      // leave instruction RAM to Meshtastic
