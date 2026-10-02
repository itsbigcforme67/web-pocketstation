// PocketStation emulator core (ARM7TDMI + hardware), portable C++11.
// Port of web/core/arm7.js + web/core/pocketstation.js. Builds for ESP32-S3 and desktop.
// Timekeeping is integer-only (the ESP32-S3 has no double-precision FPU).
#pragma once
#include <stdint.h>
#include <stddef.h>

// Optional per-project build options, set in a "psconfig.h" next to this file:
//   PS_FLASH_BLOCKS  the card is 16 separate 8 KiB blocks that may be read-only (e.g. stored in the
//                    firmware image); a block is only copied to RAM when a game writes to it.
//                    The BIOS is used in place (not copied) too.
//   PS_HEAP_TABLES   the instruction decode tables (20 KiB) are allocated on first use and can be
//                    released with ps::freeTables(), instead of taking up RAM permanently.
//   PS_SMALL_TABLES  as PS_HEAP_TABLES, but the tables take 5 KiB (one byte per entry, at the cost
//                    of one extra lookup per instruction).
//   PS_NO_IRAM       on ESP32, keep the hot functions in flash rather than in instruction RAM.
#if defined(__has_include)
#if __has_include("psconfig.h")
#include "psconfig.h"
#endif
#endif

namespace ps {

enum : uint8_t { BTN_FIRE = 1, BTN_RIGHT = 2, BTN_LEFT = 4, BTN_DOWN = 8, BTN_UP = 16 };

static const uint32_t MAX_CLOCK = 7995392;   // master clock (CLK_MODE >= 8)
static const uint32_t STATE_VERSION = 2;

struct DateTime { int year, month, day, hour, minute, second, dow; /* dow 0=Sunday */ };

struct Timer { uint32_t reload, count, mode, acc; };

struct Rtc { int sec, min, hour, dow, day, month, year; uint32_t mode; uint32_t phase; bool adjusted; };

class PocketStation;

class Cpu {
 public:
  int32_t r[16];
  int32_t bankR13[6], bankR14[6], spsrBank[6], usrR8[5], fiqR8[5];
  uint32_t n, z, c, v, iflag, fflag, t, mode;
  uint32_t pc;
  uint32_t cycles;
  bool irqLine, fiqLine, stopRequested;
  // idle-loop detection: a backward branch taken twice with identical registers and no
  // memory writes in between means the program is spinning until the next hardware event.
  bool idle;
  struct IdleSlot { uint32_t pc, writes, h1, h2; } idleSlots[8];
  uint32_t idlePc;  // kept for API compatibility (unused)
  uint32_t idleSkips, lastIdleAt;
  void checkIdle(uint32_t at);
  uint32_t sc;
  PocketStation* bus;

  void reset();
  uint32_t getCPSR() const;
  void setCPSR(uint32_t v);
  void switchMode(uint32_t m);
  uint32_t getSPSR() const;
  void setSPSR(uint32_t v);
  int32_t getUserReg(int i) const;
  void setUserReg(int i, int32_t v);
  void exception(uint32_t vector, uint32_t newMode, uint32_t lr, bool disableFiq);
  uint32_t run(uint32_t budget);
  void step();
  bool condPassed(uint32_t cond) const;
  void setReg(int rd, int32_t val) { if (rd == 15) branchTo(val); else r[rd] = val; }
  void branchTo(uint32_t a) { pc = t ? (a & ~1u) : (a & ~3u); cycles += 2; }
  void branchExchange(uint32_t a) { t = a & 1; pc = a & ~1u; if (!t) pc &= ~3u; cycles += 2; }
  // memory helpers
  int32_t ldr32(uint32_t a);
  int32_t ldr16(uint32_t a);
  int32_t ldrs16(uint32_t a);
  int32_t ldrs8(uint32_t a);
  // ALU
  void setNZ(int32_t res) { n = (uint32_t)res >> 31; z = res == 0; }
  int32_t add(int32_t a, int32_t b, bool s);
  int32_t adc(int32_t a, int32_t b, bool s);
  int32_t sub(int32_t a, int32_t b, bool s);
  int32_t sbc(int32_t a, int32_t b, bool s);
  int32_t shiftImm(uint32_t type, int32_t val, uint32_t amt);
  int32_t shiftReg(uint32_t type, int32_t val, uint32_t amt);
};

class PocketStation {
 public:
#ifdef PS_FLASH_BLOCKS
  // Memory. The BIOS and each 8 KiB card block are caller-owned and may be read-only.
  const uint8_t* bios;
  const uint8_t* blk[16];
  // Called before the first write to a block: must return a writable copy of it (which the core
  // then also reads from), or nullptr if there is no memory, in which case the write is dropped.
  uint8_t* (*writableBlock)(void* ctx, int block);
#else
  // Memory. `flash` may point at caller-owned 128 KiB storage (e.g. a memory card image).
  uint8_t bios[0x4000];
  uint8_t* flash;
#endif
  uint8_t ram[0x800];
  uint32_t vram[32];
  uint16_t extra[128];

  Cpu cpu;
  uint32_t writeCount;          // bumped on side-effecting writes (I/O, FLASH) and timer reads
  uint32_t ramHash;             // incremental fingerprint of RAM contents (idle-loop detection)
  bool idleSkip = true;         // enable idle-loop fast-forwarding

  // Hardware state
  uint32_t fCtrl, fBankFlg, fBankVal[16], fWait1, fWait2, flashUnlock;
  int32_t pageBase;
  bool remapped;
  int8_t virtMap[16];
  uint32_t intInput, intHold, intMask;
  uint32_t buttons, docked;
  Timer timers[3];
  uint32_t clkMode;
  bool sleeping;
  uint32_t lcdMode, lcdCal, iopCtrl, iopData, dacCtrl, dacData, battCtrl, irdaMode, comMode, comCtrl1, comCtrl2;
  Rtc rtc;
  uint64_t masterTime;          // master clock ticks since reset (MAX_CLOCK per second)
  bool flashDirty;

  // Audio: signed 16-bit samples pushed into a ring the host drains.
  uint32_t sampleRate;
  uint64_t audioPhase;
  int16_t* audioRing;           // optional; host-owned
  uint32_t audioRingSize, audioW;
  volatile uint32_t audioR;

  void (*onFlashWrite)(void* ctx, uint32_t page);
  void* cbCtx;

  PocketStation();
#ifdef PS_FLASH_BLOCKS
  void init(const uint8_t* biosData, const uint8_t* const blocks[16]);
#else
  void init(const uint8_t* biosData, uint8_t* flash128k);
#endif
  void hardReset(const DateTime& d);
  bool bootFile(int dirIndex, const DateTime& d);
  bool bootMenu(const DateTime& d);
  void syncClock(const DateTime& d);
  void setButtons(uint32_t mask);
  void runMs(uint32_t ms);             // run for real milliseconds of emulated time
  void runMaster(uint64_t masterTicks);
  bool displayOn() const { return (lcdMode & 0x40) != 0; }
  bool ledOn() const { return (iopData & 2) == 0 && (iopCtrl & 2) != 0; }
  // 32 rows in on-screen orientation, bit x = pixel x (1 = black)
  void getFrame(uint32_t out[32]) const;
  uint32_t clockHz() const { return (clkMode & 15) >= 8 ? MAX_CLOCK : MAX_CLOCK >> (8 - (clkMode & 15)); }
  uint32_t clockShift() const { return (clkMode & 15) >= 8 ? 0 : 8 - (clkMode & 15); }

  // Save states (fixed-size binary blob)
  size_t stateSize() const;
  void saveState(uint8_t* out) const;
  bool loadState(const uint8_t* in, size_t len, const DateTime& now);

  // bus
  uint32_t read32(uint32_t a);
  uint32_t read16(uint32_t a);
  uint32_t read8(uint32_t a);
  void write32(uint32_t a, uint32_t v);
  void write16(uint32_t a, uint32_t v);
  void write8(uint32_t a, uint32_t v);

 private:
  uint32_t ioRead(uint32_t a);
  void ioWrite(uint32_t a, uint32_t v, uint32_t mask);
  int32_t flash1Offset(uint32_t a) const;
  void flashWrite(uint32_t off, uint32_t val, int width);
  void rebuildVirtMap();
  void setInput(uint32_t bits, bool level);
  void pulse(uint32_t bits);
  void updateIrq();
  uint32_t intLatch() const;
  void tick(uint32_t cycles);
  void advanceMaster(uint32_t m);
  uint32_t cyclesToNextEvent() const;
  void rtcInit(const DateTime& d);
  void rtcIncrement(int field);
  void rtcTickSecond();
  uint32_t rtcTime() const;
  uint32_t rtcDate() const;
};

#if defined(PS_HEAP_TABLES) || defined(PS_SMALL_TABLES)
// Release the decode tables. Only when no PocketStation is running; they are rebuilt on demand.
void freeTables();
#endif

}  // namespace ps
