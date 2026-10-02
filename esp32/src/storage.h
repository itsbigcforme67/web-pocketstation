// Card library on LittleFS + PocketSync (PC companion) client.
#pragma once
#include <Arduino.h>
#include <vector>
#include "cards.h"

struct Config {
  String wifiSsid, wifiPass, server, token, tz;
  int volume = 128;
  bool idleSkip = true;   // fast-forward through games' wait loops (turn off if a game acts oddly)
};

// One memory card stored at /cards/<file>
struct CardInfo {
  String file;      // "astro.mcd" or "pc-1a2b3c4d.mcd"
  String name;      // display name
  String id;        // PocketSync card id ("" = local only)
  String sha;       // PocketSync sha1 of the version we last synced
  bool dirty = false;     // changed on this device since last sync
  bool conflict = false;  // changed on the PC too
};

struct GameEntry {
  int card;         // index into cards
  int dir;          // directory index (0 = BIOS menu)
  String title;
  uint16_t icon[256];
};

namespace store {
extern Config config;
extern std::vector<CardInfo> cards;
extern std::vector<GameEntry> games;

bool begin();                       // mount LittleFS, load config + index
void saveIndex();
void scan();                        // rebuild cards/games lists from /cards
bool loadCard(int idx, uint8_t* buf128k);
bool saveCard(int idx, const uint8_t* buf128k);
bool loadBios(uint8_t* buf16k);
String statePath(int card, int dir);
void dropStates(int card);
String displayName(const CardInfo& c);

// ---- network ----
bool wifiConfigured();
bool wifiConnect(uint32_t timeoutMs, void (*status)(const char*));
bool syncTime();
// Pull new/changed cards from the PC. Returns number of cards updated, -1 on error.
int pullFromPC(String& message);
// Send dirty cards to the PC. Returns number sent, -1 on error.
int pushToPC(String& message, bool force = false);
bool fetchBiosFromPC();
}  // namespace store
