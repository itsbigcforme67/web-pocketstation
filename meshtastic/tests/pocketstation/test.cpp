// Desktop test for the PocketStation Meshtastic module. Needs psdata.cpp made by pack.py.
// Two builds: stepped fake clock on the main loop (default), or -DTHREADED with real threads and time.
#include "fake.h"
#ifdef THREADED
#define ARCH_ESP32 1
#include "freertos/FreeRTOS.h"
std::atomic<int> tasksStarted{0};
#endif
#define private public
#define protected public
#include "PocketStation.cpp"
#undef private
#undef protected
#include <cassert>

uint32_t fakeMs = 100000, fakeEpoch = 1790000000;
bool quietLog = false;
int spiLock = 0;
Screen theScreen; Screen *screen = &theScreen;
InputBroker theBroker; InputBroker *inputBroker = &theBroker;
FakeFsm powerFSM; MemGet memGet;
std::map<std::string, std::string> fakeFiles; FakeFS FSCom;
static OLEDDisplay disp;

static int key(input_broker_event e, unsigned char ch = 0) { InputEvent ev{"test", e, ch, 0, 0}; return inputBroker->notifyObservers(&ev); }
static void tick(PocketStationModule *m, uint32_t ms) {
#ifdef THREADED
    for (uint32_t end = millis() + ms; (int32_t)(end - millis()) > 0;) { m->runOnce(); std::this_thread::sleep_for(std::chrono::milliseconds(33)); }
#else
    for (uint32_t t = 0; t < ms; t += 33) { fakeMs += 33; m->runOnce(); }
#endif
}
static int find(PocketStationModule *m, const char *title) { for (size_t i = 0; i < m->games.size(); i++) if (!strcmp(m->games[i].title, title)) return (int)i; return -1; }
static void choose(PocketStationModule *m, int idx) { while (m->selected != idx) key(INPUT_BROKER_DOWN); }
// Read the 32x32 LCD back out of what was drawn on the 240x135 screen
static void decode(uint32_t rows[32]) {
    for (int r = 0; r < 32; r++) { rows[r] = 0; for (int c = 0; c < 32; c++) if (disp.px[(3 + r * 4 + 1) * 240 + 56 + c * 4 + 1] == BLACK) rows[r] |= 1u << c; }
}
static int darkPixels(const uint32_t rows[32]) { int n = 0; for (int r = 0; r < 32; r++) n += __builtin_popcount(rows[r]); return n; }
static void untilList(PocketStationModule *m) { for (int i = 0; i < 400 && m->state != LIST; i++) tick(m, 33); assert(m->state == LIST); }

int main() {
    auto *m = new PocketStationModule();
    assert(!m->packProblem && m->games.size() == 7 && m->packCardCount == 7);
    const int chocobo = find(m, "Chocobo World"), tetris = find(m, "Tetris");
    assert(chocobo >= 0 && tetris >= 0);

    // ---- list page: up/down are ours, left/right are left for page navigation ----
    m->drawFrame(&disp, nullptr, 0, 0);
    assert(disp.text[0] == "[PocketStation]" && disp.text.size() == 6 && disp.text[1].rfind("> ", 0) == 0);
    assert(key(INPUT_BROKER_DOWN) == 1 && m->selected == 1 && key(INPUT_BROKER_UP) == 1 && m->selected == 0);
    assert(key(INPUT_BROKER_UP) == 1 && m->selected == 6);                                  // wraps
    m->drawFrame(&disp, nullptr, 0, 0);
    assert(disp.text.back().rfind("> ", 0) == 0);                                             // list scrolled to keep it visible
    assert(key(INPUT_BROKER_LEFT) == 0 && key(INPUT_BROKER_RIGHT) == 0 && !m->interceptingKeyboardInput());
    theScreen.showing = false; assert(key(INPUT_BROKER_DOWN) == 0 && m->selected == 6); theScreen.showing = true;  // another page is up

    // ---- button taps: one press holds briefly; a second press of the same button gets a gap first ----
#ifndef THREADED
    { uint32_t t = fakeMs; m->tap(0); assert(m->buttonMask(t) == 1 && m->buttonMask(t + 119) == 1 && m->buttonMask(t + 120) == 0);
      fakeMs = t + 50; m->tap(0); assert(m->buttonMask(t + 50) == 0 && m->buttonMask(t + 109) == 0 && m->buttonMask(t + 110) == 1 && m->buttonMask(t + 229) == 1 && m->buttonMask(t + 230) == 0);
      m->tap(4); assert(m->buttonMask(t + 55) == 16); fakeMs = t + 1000; }
#endif

    // ---- start a game (refused, with a message, when memory is short) ----
    choose(m, chocobo);
    memGet.freeHeap = 20000;
    assert(key(INPUT_BROKER_SELECT) == 1 && m->state == LIST && !m->emu && theScreen.banner == "Low memory\nneed 24K, have 19K");
    memGet.freeHeap = 123456;
    assert(key(INPUT_BROKER_SELECT) == 1 && (m->state == LOADING || m->state == PLAYING) && m->interceptingKeyboardInput());
    m->drawFrame(&disp, nullptr, 0, 0);
    for (int i = 0; i < 600 && m->state != PLAYING; i++) tick(m, 33);
    assert(m->state == PLAYING && m->curGame == chocobo && m->curCard == m->games[chocobo].card);
    uint32_t drawn[32];
    for (int tries = 0; tries < 40; tries++) {   // games blank the screen between scenes: wait for a picture
        tick(m, tries ? 250 : 3000);
        m->drawFrame(&disp, nullptr, 0, 0);
        decode(drawn);
        assert(memcmp(drawn, m->shownFrame, sizeof(drawn)) == 0);   // the screen shows exactly the emulator's picture
        if (darkPixels(drawn) > 20 && darkPixels(drawn) < 1000) break;
    }
    assert(darkPixels(drawn) > 20 && darkPixels(drawn) < 1000);
    assert(disp.px[0] == BLACK && disp.px[(3 + 64) * 240 + 20] == BLACK);   // outside the LCD stays dark
    assert(powerFSM.presses > 0);                                             // display kept awake while playing
    assert(key(INPUT_BROKER_LEFT) == 1 && key(INPUT_BROKER_ANYKEY, ' ') == 1);  // keys belong to the game now

    // ---- play: the game writes to its card; only those blocks are copied, then saved ----
    const input_broker_event keys[] = {INPUT_BROKER_SELECT, INPUT_BROKER_DOWN, INPUT_BROKER_SELECT, INPUT_BROKER_UP, INPUT_BROKER_SELECT, INPUT_BROKER_RIGHT, INPUT_BROKER_LEFT};
    for (auto k : keys) { key(k); tick(m, 1500); }
    tick(m, 6000);
    int copies = 0; for (int i = 0; i < 16; i++) copies += m->copy[i] != nullptr;
    assert(copies >= 1 && copies <= 3 && !m->outOfMemory);
    char path[40]; m->savePath(path, sizeof(path));
    assert(fakeFiles.count(path) && fakeFiles[path].size() == 12 + (size_t)copies * 0x2000 && m->dirtyMask == 0);
    for (int i = 0; i < 16; i++) assert(m->emu->blk[i] == (m->copy[i] ? m->copy[i] : m->base[i]));
    const uint32_t packLen = psdata_len; (void)packLen;
    printf("played Chocobo World: %d block(s) copied to RAM, save file %u bytes, load %u%%\n", copies, (unsigned)fakeFiles[path].size(), (unsigned)m->loadPct.load());

    // ---- leave with Esc, come back: same game resumes without restarting ----
    assert(key(INPUT_BROKER_CANCEL) == 1 && m->state == STOPPING);
    untilList(m);
    const uint64_t before = m->emu->masterTime;
    assert(!m->interceptingKeyboardInput() && m->curGame == chocobo && !m->taskAlive);
    m->drawFrame(&disp, nullptr, 0, 0);
    bool starred = false; for (auto &s : disp.text) starred = starred || s.find("Chocobo World *") != std::string::npos;
    assert(starred);
    assert(key(INPUT_BROKER_SELECT) == 1 && m->state == PLAYING && !m->needBoot);
    tick(m, 1000);
    assert(m->state == PLAYING);
    key(INPUT_BROKER_BACK); untilList(m);
    assert(m->emu->masterTime > before);                        // it carried on from where it was

    // ---- another card: copies of the old card are dropped, the new card runs from the pack ----
    choose(m, tetris); key(INPUT_BROKER_SELECT);
    for (int i = 0; i < 600 && m->state != PLAYING; i++) tick(m, 33);
    assert(m->state == PLAYING && m->curCard == m->games[tetris].card);
    for (int tries = 0; tries < 40; tries++) { tick(m, tries ? 250 : 3000); m->drawFrame(&disp, nullptr, 0, 0); decode(drawn); if (darkPixels(drawn) > 20) break; }
    assert(darkPixels(drawn) > 20);
    for (int i = 0; i < 16; i++) assert(!m->copy[i] && m->emu->blk[i] == m->base[i]);

    // ---- another page takes the screen: the game lets go of the keys after a few seconds ----
    theScreen.showing = false; tick(m, 4000); untilList(m); theScreen.showing = true;
    assert(!m->interceptingKeyboardInput());

    // ---- restart the device: saved blocks come back ----
    std::string saved = fakeFiles[path];
    theBroker.observers.clear();
    auto *m2 = new PocketStationModule();
    choose(m2, chocobo); key(INPUT_BROKER_SELECT);
    for (int i = 0; i < 600 && m2->state != PLAYING; i++) tick(m2, 33);
    int restored = 0; size_t off = 12;
    for (int i = 0; i < 16; i++) if (m2->copy[i]) { restored++; assert(memcmp(m2->copy[i], saved.data() + off, 0x2000) == 0 || m2->dirtyMask); off += 0x2000; assert(m2->emu->blk[i] == m2->copy[i]); }
    assert(restored == copies);
    key(INPUT_BROKER_CANCEL); untilList(m2);

    // ---- a save made for a different version of the card is ignored ----
    fakeFiles[path][8] ^= 0x5A;
    theBroker.observers.clear();
    auto *m3 = new PocketStationModule();
    m3->selectCard(m3->games[chocobo].card);
    for (int i = 0; i < 16; i++) assert(!m3->copy[i]);

    printf("ALL TESTS PASSED\n");
    return 0;
}
