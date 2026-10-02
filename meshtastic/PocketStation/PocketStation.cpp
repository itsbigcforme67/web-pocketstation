#include "PocketStation.h"
#include "configuration.h"

#if HAS_SCREEN

#include "DebugConfiguration.h"
#include "FSCommon.h"
#include "Observer.h"
#include "PowerFSM.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "cards.h"
#include "concurrency/LockGuard.h"
#include "concurrency/OSThread.h"
#include "gps/RTC.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#include "input/InputBroker.h"
#include "main.h"
#include "memGet.h"
#include "mesh/MeshModule.h"
#include "pscore.h"
#include <atomic>
#include <cstring>
#include <ctime>
#include <vector>

#ifdef ARCH_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#define PS_THREADED 1 // the emulator gets its own task on the other CPU core
#else
#define PS_THREADED 0 // elsewhere it runs in short slices on the main loop
#endif

// The data pack. Missing (null) until pack.py has generated psdata.cpp.
extern "C" {
extern const uint8_t psdata[] __attribute__((weak));
extern const uint32_t psdata_len __attribute__((weak));
}

namespace
{
const uint32_t BLOCK = 0x2000;
const uint32_t PACK_VERSION = 1;
const uint32_t SAVE_MAGIC = 0x56535350; // "PSSV"
const uint16_t SAVE_VERSION = 1;

const uint32_t TAP_HOLD_MS = 120;        // how long one key press holds a PocketStation button
const uint32_t TAP_GAP_MS = 60;          // release between two presses of the same button
const uint32_t SAVE_QUIET_MS = 3000;     // save this long after the game last wrote to its card
const uint32_t AWAKE_AFTER_KEY_MS = 120000;
const uint32_t MIN_FREE_HEAP = 24 * 1024; // to start: 5K decode tables, 3K task stack, 3K emulator, room to spare
const uint32_t TASK_STACK = 3072;         // measured use on the Cardputer is about 1.1K
const uint32_t TICK_PLAY_MS = 33;
const uint32_t TICK_IDLE_MS = 500;

struct PackHeader {
    char magic[4];
    uint32_t version, cardCount, blockCount, biosOffset, blocksOffset;
};
struct PackCard {
    char name[32];
    uint32_t crc;
    uint16_t block[16];
};
struct SaveHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t mask; // which blocks follow, lowest first
    uint32_t baseCrc;
};

struct GameEntry {
    uint8_t card, dir;
    char title[40];
};

enum State : uint8_t { LIST, LOADING, PLAYING, STOPPING };
enum Phase : uint8_t { PH_BOOT, PH_RUN, PH_FAILED };

class PocketStationModule : public MeshModule, public Observable<const UIFrameEvent *>, private concurrency::OSThread
{
  public:
    PocketStationModule();
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
    virtual bool interceptingKeyboardInput() override { return state != LIST; }

  protected:
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override { return false; }
    virtual int32_t runOnce() override;
    virtual bool wantUIFrame() override { return true; }
    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }

  private:
    // ---- data pack ----
    const PackCard *packCards = nullptr;
    const uint8_t *packBios = nullptr;
    const uint8_t *packBlocks = nullptr;
    uint32_t packCardCount = 0;
    const char *packProblem = nullptr;
    std::vector<GameEntry> games;
    void openPack();

    // ---- current card: read-only blocks from the pack, RAM copies of the ones a game has written ----
    int curCard = -1;
    const uint8_t *base[16] = {};
    uint8_t *copy[16] = {};
    std::atomic<uint16_t> dirtyMask{0};
    std::atomic<uint32_t> lastWriteMs{0};
    bool outOfMemory = false;
    void selectCard(int card);
    void dropCopies();
    void loadCopies();
    void saveCopies();
    void savePath(char *out, size_t len) const;
    static uint8_t *writableBlockCb(void *ctx, int block);

    // ---- emulator ----
    ps::PocketStation *emu = nullptr;
    int curGame = -1; // game the emulator currently holds (paused or running), -1 = none
    ps::DateTime bootTime{};
    bool needBoot = false;
    std::atomic<bool> runFlag{false}, taskAlive{false};
    std::atomic<uint8_t> phase{PH_BOOT};
    std::atomic<uint32_t> pressFrom[5], pressTo[5];
    uint32_t lastUs = 0, busyUs = 0, loadWindowMs = 0;
    std::atomic<uint32_t> loadPct{0}, stackSpare{0};
    uint32_t pubFrame[32] = {};
    std::atomic<uint32_t> pubVersion{0};
    void emuBoot();
    void emuSlice();
    uint32_t buttonMask(uint32_t now) const;
#if PS_THREADED
    SemaphoreHandle_t emuLock = nullptr;
    portMUX_TYPE frameMux = portMUX_INITIALIZER_UNLOCKED;
    static void taskFn(void *arg);
#endif
    void spawn();

    // ---- UI ----
    State state = LIST;
    int selected = 0;
    uint32_t shownFrame[32] = {};
    uint32_t shownVersion = 0;
    uint32_t lastKeyMs = 0, lastAwakeKickMs = 0, lastLoadLogMs = 0;
    uint8_t hiddenTicks = 0;
    void startGame(int index);
    void stopGame();
    void tap(int button);
    void requestRedraw();
    ps::DateTime now() const;
    int handleInputEvent(const InputEvent *event);
    CallbackObserver<PocketStationModule, const InputEvent *> inputObserver =
        CallbackObserver<PocketStationModule, const InputEvent *>(this, &PocketStationModule::handleInputEvent);
};

PocketStationModule::PocketStationModule() : MeshModule("PocketStation"), concurrency::OSThread("PocketStation")
{
    for (int i = 0; i < 5; i++) {
        pressFrom[i] = 0;
        pressTo[i] = 0;
    }
    openPack();
    if (packProblem)
        LOG_WARN("PocketStation: %s", packProblem);
    else
        LOG_INFO("PocketStation: %u games on %u cards, free heap %u", (unsigned)games.size(), (unsigned)packCardCount,
                 (unsigned)memGet.getFreeHeap());
    if (inputBroker)
        inputObserver.observe(inputBroker);
    setIntervalFromNow(TICK_IDLE_MS);
}

// ---------------------------------------------------------------------------------------------
// Data pack
// ---------------------------------------------------------------------------------------------

void PocketStationModule::openPack()
{
    if (!&psdata_len || !psdata) {
        packProblem = "no games built in (run pack.py, then rebuild)";
        return;
    }
    PackHeader h;
    if (psdata_len < sizeof(h)) {
        packProblem = "data pack is damaged";
        return;
    }
    memcpy(&h, psdata, sizeof(h));
    const uint64_t tableEnd = sizeof(h) + (uint64_t)h.cardCount * sizeof(PackCard);
    if (memcmp(h.magic, "PSPK", 4) != 0 || h.version != PACK_VERSION || h.cardCount > 64 || tableEnd > psdata_len ||
        (uint64_t)h.biosOffset + 0x4000 > psdata_len || (uint64_t)h.blocksOffset + (uint64_t)h.blockCount * BLOCK > psdata_len) {
        packProblem = "data pack is damaged or from another version";
        return;
    }
    packCards = reinterpret_cast<const PackCard *>(psdata + sizeof(h));
    packBios = psdata + h.biosOffset;
    packBlocks = psdata + h.blocksOffset;
    packCardCount = h.cardCount;

    for (uint32_t c = 0; c < packCardCount; c++) {
        const PackCard &pc = packCards[c];
        bool ok = true;
        for (int i = 0; i < 16; i++)
            ok = ok && pc.block[i] < h.blockCount;
        if (!ok)
            continue;
        std::vector<cards::Save> found;
        for (cards::Save &s : cards::parseDirectory(packBlocks + pc.block[0] * BLOCK)) {
            if (s.firstBlock < 1 || s.firstBlock > 15)
                continue;
            cards::parseHeader(s, packBlocks + pc.block[s.firstBlock] * BLOCK);
            if (s.pocket)
                found.push_back(s);
        }
        char cardName[sizeof(pc.name) + 1];
        memcpy(cardName, pc.name, sizeof(pc.name));
        cardName[sizeof(pc.name)] = 0;
        for (const cards::Save &s : found) {
            GameEntry g;
            g.card = (uint8_t)c;
            g.dir = (uint8_t)s.dirIndex;
            if (found.size() == 1)
                snprintf(g.title, sizeof(g.title), "%s", cardName);
            else
                snprintf(g.title, sizeof(g.title), "%s: %s", cardName, s.title.length() ? s.title.c_str() : s.filename.c_str());
            games.push_back(g);
        }
    }
    if (games.empty())
        packProblem = "no PocketStation games in the data pack";
}

// ---------------------------------------------------------------------------------------------
// Card blocks
// ---------------------------------------------------------------------------------------------

void PocketStationModule::selectCard(int card)
{
    if (card == curCard)
        return;
    dropCopies();
    curCard = card;
    for (int i = 0; i < 16; i++)
        base[i] = packBlocks + packCards[card].block[i] * BLOCK;
    loadCopies();
}

void PocketStationModule::dropCopies()
{
    for (int i = 0; i < 16; i++) {
        free(copy[i]);
        copy[i] = nullptr;
    }
    dirtyMask = 0;
}

void PocketStationModule::savePath(char *out, size_t len) const
{
    snprintf(out, len, "/prefs/pstation-%08x.sav", (unsigned)packCards[curCard].crc);
}

// Blocks a game changed on an earlier run. Ignored if the card in the data pack has changed since.
void PocketStationModule::loadCopies()
{
#ifdef FSCom
    char path[40];
    savePath(path, sizeof(path));
    concurrency::LockGuard g(spiLock);
    auto f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return;
    SaveHeader h;
    bool ok = f.read(reinterpret_cast<uint8_t *>(&h), sizeof(h)) == sizeof(h) && h.magic == SAVE_MAGIC &&
              h.version == SAVE_VERSION && h.baseCrc == packCards[curCard].crc;
    for (int i = 0; ok && i < 16; i++) {
        if (!(h.mask & (1u << i)))
            continue;
        uint8_t *p = static_cast<uint8_t *>(malloc(BLOCK));
        if (!p || f.read(p, BLOCK) != BLOCK) {
            free(p);
            ok = false;
            break;
        }
        copy[i] = p;
    }
    f.close();
    if (!ok) {
        LOG_WARN("PocketStation: saved progress for this card could not be used, starting from the packed card");
        for (int i = 0; i < 16; i++) {
            free(copy[i]);
            copy[i] = nullptr;
        }
    }
#endif
}

void PocketStationModule::saveCopies()
{
    dirtyMask = 0;
#ifdef FSCom
    SaveHeader h{SAVE_MAGIC, SAVE_VERSION, 0, packCards[curCard].crc};
    for (int i = 0; i < 16; i++)
        if (copy[i])
            h.mask |= (uint16_t)(1u << i);
    if (!h.mask)
        return;
    {
        concurrency::LockGuard g(spiLock);
        FSCom.mkdir("/prefs");
    }
    char path[40];
    savePath(path, sizeof(path));
    auto out = SafeFile(path, true);
    size_t written = out.write(reinterpret_cast<uint8_t *>(&h), sizeof(h));
    size_t expected = sizeof(h);
    for (int i = 0; i < 16; i++) {
        if (!copy[i])
            continue;
        written += out.write(copy[i], BLOCK);
        expected += BLOCK;
    }
    if (!out.close() || written != expected)
        LOG_WARN("PocketStation: could not save game progress");
    else
        LOG_INFO("PocketStation: saved progress (%u bytes)", (unsigned)written);
#endif
}

// Runs on the emulator's thread, just before a game writes to a card block.
uint8_t *PocketStationModule::writableBlockCb(void *ctx, int block)
{
    PocketStationModule *self = static_cast<PocketStationModule *>(ctx);
    if (!self->copy[block]) {
        uint8_t *p = static_cast<uint8_t *>(malloc(BLOCK));
        if (!p) {
            self->outOfMemory = true;
            return nullptr;
        }
        memcpy(p, self->base[block], BLOCK);
        self->copy[block] = p;
    }
    self->dirtyMask.fetch_or((uint16_t)(1u << block));
    self->lastWriteMs = millis();
    return self->copy[block];
}

// ---------------------------------------------------------------------------------------------
// Emulator
// ---------------------------------------------------------------------------------------------

uint32_t PocketStationModule::buttonMask(uint32_t t) const
{
    uint32_t m = 0;
    for (int i = 0; i < 5; i++)
        if ((int32_t)(t - pressFrom[i].load()) >= 0 && (int32_t)(pressTo[i].load() - t) > 0)
            m |= 1u << i;
    return m;
}

// A key press on the device holds the PocketStation button briefly (the keyboard only reports taps)
void PocketStationModule::tap(int i)
{
    const uint32_t t = millis();
    uint32_t from = t;
    if ((int32_t)(pressTo[i].load() - t) > 0)
        from = ((int32_t)(t - pressFrom[i].load()) >= 0 ? t : pressTo[i].load()) + TAP_GAP_MS;
    pressFrom[i] = from;
    pressTo[i] = from + TAP_HOLD_MS;
}

void PocketStationModule::emuBoot()
{
    if (needBoot) {
        needBoot = false;
        if (!emu->bootFile(games[curGame].dir, bootTime)) {
            phase = PH_FAILED;
            return;
        }
    }
    lastUs = micros();
    busyUs = 0;
    loadWindowMs = millis();
    phase = PH_RUN;
}

// Advance the emulator by the real time that has passed, and publish the screen if it changed
void PocketStationModule::emuSlice()
{
    const uint32_t t0 = micros();
    uint32_t dt = t0 - lastUs;
    lastUs = t0;
    if (dt > 100000)
        dt = 100000; // too slow to keep up: let the game slow down instead of falling further behind
    emu->setButtons(buttonMask(millis()));
    if (dt)
        emu->runMaster((uint64_t)dt * ps::MAX_CLOCK / 1000000);

    uint32_t rows[32];
    emu->getFrame(rows);
    if (memcmp(rows, pubFrame, sizeof(rows)) != 0) {
#if PS_THREADED
        portENTER_CRITICAL(&frameMux);
#endif
        memcpy(pubFrame, rows, sizeof(rows));
#if PS_THREADED
        portEXIT_CRITICAL(&frameMux);
#endif
        pubVersion++;
    }

    busyUs += micros() - t0;
    const uint32_t window = millis() - loadWindowMs;
    if (window >= 5000) {
        loadPct = (uint32_t)((uint64_t)busyUs / ((uint64_t)window * 10));
        busyUs = 0;
        loadWindowMs = millis();
    }
}

#if PS_THREADED
void PocketStationModule::taskFn(void *arg)
{
    PocketStationModule *self = static_cast<PocketStationModule *>(arg);
    self->emuBoot();
    while (self->runFlag && self->phase == PH_RUN) {
        xSemaphoreTake(self->emuLock, portMAX_DELAY);
        self->emuSlice();
        xSemaphoreGive(self->emuLock);
        self->stackSpare = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
        vTaskDelay(1); // always give the core back; the time is made up in the next slice
    }
    self->taskAlive = false;
    vTaskDelete(nullptr);
}
#endif

void PocketStationModule::spawn()
{
    phase = PH_BOOT;
    runFlag = true;
    taskAlive = true;
#if PS_THREADED
    if (!emuLock)
        emuLock = xSemaphoreCreateMutex();
    if (!emuLock || xTaskCreatePinnedToCore(taskFn, "pocketstation", TASK_STACK, this, 1, nullptr, 0) != pdPASS) {
        LOG_ERROR("PocketStation: could not start the emulator task");
        taskAlive = false;
        phase = PH_FAILED;
    }
#endif
}

ps::DateTime PocketStationModule::now() const
{
    time_t t = (time_t)getValidTime(RTCQualityDevice, true);
    if (!t)
        return ps::DateTime{2026, 1, 1, 12, 0, 0, 4};
    struct tm tmv;
    gmtime_r(&t, &tmv);
    return ps::DateTime{tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tmv.tm_wday};
}

void PocketStationModule::startGame(int index)
{
    if (state != LIST || index < 0 || index >= (int)games.size())
        return;
    if (!emu && memGet.getFreeHeap() < MIN_FREE_HEAP) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Low memory\nneed %uK, have %uK", (unsigned)(MIN_FREE_HEAP / 1024),
                 (unsigned)(memGet.getFreeHeap() / 1024));
        LOG_WARN("PocketStation: not enough free memory to start (%u bytes free)", (unsigned)memGet.getFreeHeap());
        screen->showSimpleBanner(msg, 4000);
        return;
    }
    if (index != curGame) {
        if (curCard >= 0 && dirtyMask)
            saveCopies();
        selectCard(games[index].card);
        if (!emu)
            emu = new ps::PocketStation();
        const uint8_t *blocks[16];
        for (int i = 0; i < 16; i++)
            blocks[i] = copy[i] ? copy[i] : base[i];
        emu->init(packBios, blocks);
        emu->writableBlock = writableBlockCb;
        emu->cbCtx = this;
        curGame = index;
        bootTime = now();
        needBoot = true;
        memset(pubFrame, 0, sizeof(pubFrame));
        memset(shownFrame, 0, sizeof(shownFrame));
    }
    for (int i = 0; i < 5; i++) {
        pressFrom[i] = 0;
        pressTo[i] = 0;
    }
    LOG_INFO("PocketStation: %s %s", needBoot ? "starting" : "resuming", games[index].title);
    state = needBoot ? LOADING : PLAYING;
    lastKeyMs = millis();
    hiddenTicks = 0;
    spawn();
    setIntervalFromNow(0);
    requestRedraw();
}

void PocketStationModule::stopGame()
{
    if (state == LIST || state == STOPPING)
        return;
    runFlag = false;
    state = STOPPING;
    requestRedraw();
}

int32_t PocketStationModule::runOnce()
{
#if !PS_THREADED
    if (taskAlive) {
        if (phase == PH_BOOT)
            emuBoot();
        if (runFlag && phase == PH_RUN)
            emuSlice();
        else
            taskAlive = false;
    }
#endif
    const uint32_t t = millis();

    if (state == LOADING) {
        if (phase == PH_RUN)
            state = PLAYING;
        requestRedraw();
    }
    if ((state == LOADING || state == PLAYING) && phase == PH_FAILED)
        stopGame();

    if (state == PLAYING) {
        if (pubVersion != shownVersion) {
            shownVersion = pubVersion;
#if PS_THREADED
            portENTER_CRITICAL(&frameMux);
#endif
            memcpy(shownFrame, pubFrame, sizeof(shownFrame));
#if PS_THREADED
            portEXIT_CRITICAL(&frameMux);
#endif
            requestRedraw();
        }
        if (t - lastKeyMs < AWAKE_AFTER_KEY_MS && t - lastAwakeKickMs > 1500) {
            powerFSM.trigger(EVENT_PRESS); // keep the display on while someone is playing
            lastAwakeKickMs = t;
        }
        if (dirtyMask && t - lastWriteMs.load() > SAVE_QUIET_MS) {
#if PS_THREADED
            if (xSemaphoreTake(emuLock, pdMS_TO_TICKS(20)) == pdTRUE) {
                saveCopies();
                xSemaphoreGive(emuLock);
            }
#else
            saveCopies();
#endif
        }
        if (t - lastLoadLogMs > 10000) {
            lastLoadLogMs = t;
            LOG_INFO("PocketStation: emulator load %u%%, free heap %u, spare stack %u", (unsigned)loadPct.load(),
                     (unsigned)memGet.getFreeHeap(), (unsigned)stackSpare.load());
        }
        // If another page took over the screen, don't keep hold of the keys
        if (screen && !screen->isShowingModuleFrame(this)) {
            if (++hiddenTicks > 90)
                stopGame();
        } else {
            hiddenTicks = 0;
        }
    }

    if (state == STOPPING && !taskAlive) {
        if (phase == PH_FAILED) {
            LOG_WARN("PocketStation: %s did not start", games[curGame].title);
            curGame = -1;
        }
        if (dirtyMask)
            saveCopies();
        if (outOfMemory) {
            LOG_WARN("PocketStation: ran out of memory, some game progress was not kept");
            outOfMemory = false;
        }
        state = LIST;
        requestRedraw();
    }

    return state == LIST ? TICK_IDLE_MS : TICK_PLAY_MS;
}

// ---------------------------------------------------------------------------------------------
// Keys and screen
// ---------------------------------------------------------------------------------------------

int PocketStationModule::handleInputEvent(const InputEvent *event)
{
    if (!screen || !screen->isShowingModuleFrame(this) || screen->isOverlayBannerShowing())
        return 0;
    const input_broker_event ev = event->inputEvent;
    const unsigned char ch = ev == INPUT_BROKER_ANYKEY ? event->kbchar : 0;
    const bool isBack = ev == INPUT_BROKER_CANCEL || ev == INPUT_BROKER_BACK;

    if (state == LIST) {
        const int n = (int)games.size();
        if (n && (ev == INPUT_BROKER_UP || ev == INPUT_BROKER_DOWN)) {
            selected = (selected + (ev == INPUT_BROKER_DOWN ? 1 : n - 1)) % n;
            requestRedraw();
            return 1;
        }
        if (n && ev == INPUT_BROKER_SELECT) {
            startGame(selected);
            return 1;
        }
        return 0; // left/right still move between pages
    }

    lastKeyMs = millis();
    if (isBack) {
        stopGame();
        return 1;
    }
    if (state == PLAYING) {
        // bit order matches ps::BTN_*: 0 fire, 1 right, 2 left, 3 down, 4 up
        // (letter keys can't be used: Meshtastic opens its message composer on them first)
        if (ev == INPUT_BROKER_SELECT || ch == ' ')
            tap(0);
        else if (ev == INPUT_BROKER_RIGHT)
            tap(1);
        else if (ev == INPUT_BROKER_LEFT)
            tap(2);
        else if (ev == INPUT_BROKER_DOWN)
            tap(3);
        else if (ev == INPUT_BROKER_UP)
            tap(4);
    }
    return 1;
}

void PocketStationModule::requestRedraw()
{
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REDRAW_ONLY;
    notifyObservers(&e);
}

void PocketStationModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *uiState, int16_t x, int16_t y)
{
    display->clear();
    display->setColor(WHITE);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    const int w = display->getWidth(), h = display->getHeight();

    if (state == PLAYING) {
        // The 32x32 LCD as large as fits: a lit panel with the dark pixels cut out of it
        int scale = (h < w ? h : w) / 32;
        if (scale < 1)
            scale = 1;
        const int size = 32 * scale, ox = x + (w - size) / 2, oy = y + (h - size) / 2;
        display->fillRect(ox, oy, size, size);
        display->setColor(BLACK);
        for (int row = 0; row < 32; row++) {
            const uint32_t bits = shownFrame[row];
            for (int col = 0; col < 32;) {
                if (!(bits & (1u << col))) {
                    col++;
                    continue;
                }
                int end = col;
                while (end < 32 && (bits & (1u << end)))
                    end++;
                display->fillRect(ox + col * scale, oy + row * scale, (end - col) * scale, scale);
                col = end;
            }
        }
        display->setColor(WHITE);
        return;
    }

    graphics::drawCommonHeader(display, x, y, "PocketStation");
    const int *rows = graphics::getTextPositions(display);
    const int rowH = _fontHeight(FONT_SMALL);

    if (state == LOADING || state == STOPPING) {
        display->drawString(x, y + rows[1], state == LOADING ? "Starting..." : "Saving...");
        if (curGame >= 0)
            display->drawString(x, y + rows[2], games[curGame].title);
        return;
    }

    if (games.empty()) {
        display->drawString(x, y + rows[1], "No games built in.");
        display->drawString(x, y + rows[2], "Run pack.py, rebuild.");
        return;
    }

    int visible = 0;
    for (int r = 1; r <= 6 && rows[r] + rowH <= h; r++)
        visible++;
    if (visible < 1)
        visible = 1;
    int first = selected - visible / 2;
    if (first > (int)games.size() - visible)
        first = (int)games.size() - visible;
    if (first < 0)
        first = 0;
    for (int r = 0; r < visible && first + r < (int)games.size(); r++) {
        const int i = first + r;
        char line[48];
        snprintf(line, sizeof(line), "%s %s%s", i == selected ? ">" : "  ", games[i].title, i == curGame ? " *" : "");
        display->drawString(x, y + rows[1 + r], line);
    }
}

} // namespace

void setupPocketStation()
{
    new PocketStationModule();
}

#else // !HAS_SCREEN

void setupPocketStation() {}

#endif
