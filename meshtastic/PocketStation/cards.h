// Memory card helpers: directory parsing, titles, icons. Pure C++ (no Arduino), testable on desktop.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace cards {

static const uint32_t CARD_SIZE = 0x20000;
static const uint32_t BLOCK = 0x2000;
static const uint32_t FRAME = 0x80;

struct Save {
  int dirIndex;          // 1..15
  int firstBlock;        // physical block (1..15)
  int blocks;
  bool pocket;           // has an "MCX0"/"MCX1" PocketStation executable header
  std::string filename;  // e.g. BISLPSP01880ODCRPG
  std::string title;     // best-effort ASCII title ("" if none)
  bool titlePartial;     // title had Japanese text that was dropped
};

// Parse the directory frames (first 0x800 bytes of a card).
std::vector<Save> parseDirectory(const uint8_t* dir2k);

// Given the first 0x100 bytes of a save's first block, fill pocket flag + title.
void parseHeader(Save& s, const uint8_t* head256);

// Decode the save's 16x16 colour icon (first frame) to RGB565 (big-endian swapped for LCD
// pushImage when swapBytes is true). `head256` = first 0x100 bytes of the save's first block.
void decodeIcon(const uint8_t* head256, uint16_t out[256], bool swapBytes);

// Best-effort ASCII from a Shift-JIS title: prefers text inside 「」, converts full-width
// ASCII. Returns "" if there's nothing readable.
std::string sjisTitle(const uint8_t* p, int maxLen, bool* partial = nullptr);

// Container detection: returns header length (0 raw, 3904 .gme, 64 .mem) or -1.
int containerHeader(const uint8_t* data, size_t len);

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0);

}  // namespace cards
