#include "StreetPass.h"

#include "DebugConfiguration.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "concurrency/LockGuard.h"
#include "gps/RTC.h"
#include "main.h"
#include "mesh/NodeDB.h"
#include "mesh/Throttle.h"
#include <ErriezCRC32.h>
#include <cstddef>
#include <cstring>

#if HAS_SCREEN
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "graphics/SharedUIDisplay.h"
#endif

StreetPassModule *streetPassModule;

void setupStreetPass()
{
    streetPassModule = new StreetPassModule();
}

namespace
{
const char *const SAVE_PATH = "/prefs/streetpass.bin";
const uint32_t FILE_MAGIC = 0x53545250; // "STRP"
const uint8_t FILE_VERSION = 1;

const uint32_t TICK_MS = 30 * 1000;
const uint32_t STALE_SAVE_MS = 30 * 60 * 1000;

struct __attribute__((packed)) SaveFile {
    uint32_t magic;
    uint8_t version;
    uint8_t reserved[3];
    StreetPassModule::Pass entries[StreetPassModule::MAX_PASSES];
    uint32_t crc; // over everything before it
};

#if HAS_SCREEN
void formatAgo(char *out, size_t len, uint32_t secs)
{
    if (secs == UINT32_MAX)
        snprintf(out, len, "--");
    else if (secs < 60)
        snprintf(out, len, "now");
    else if (secs < 3600)
        snprintf(out, len, "%um", (unsigned)(secs / 60));
    else if (secs < 86400)
        snprintf(out, len, "%uh", (unsigned)(secs / 3600));
    else
        snprintf(out, len, "%ud", (unsigned)(secs / 86400));
}
#endif
} // namespace

StreetPassModule::StreetPassModule() : MeshModule("StreetPass"), concurrency::OSThread("StreetPass")
{
    isPromiscuous = true; // packets addressed to other nodes still prove their sender is nearby
    encryptedOk = true;   // so do packets on channels we can't read
    load();
    LOG_INFO("StreetPass: %u nodes met, %lu passes", (unsigned)nodesMet(), (unsigned long)totalPasses());
    setIntervalFromNow(TICK_MS);
}

uint8_t StreetPassModule::nodesMet() const
{
    uint8_t n = 0;
    for (const Pass &p : passLog)
        if (p.node)
            n++;
    return n;
}

uint32_t StreetPassModule::totalPasses() const
{
    uint32_t n = 0;
    for (const Pass &p : passLog)
        if (p.node)
            n += p.count;
    return n;
}

int StreetPassModule::find(uint32_t node) const
{
    for (int i = 0; i < MAX_PASSES; i++)
        if (passLog[i].node == node)
            return i;
    return -1;
}

// A free slot, or failing that the node we have gone longest without hearing
int StreetPassModule::allocSlot()
{
    int oldest = 0;
    for (int i = 0; i < MAX_PASSES; i++) {
        if (!passLog[i].node)
            return i;
        const bool heardI = heardAtMs[i] != 0, heardO = heardAtMs[oldest] != 0;
        if (heardI != heardO) {
            if (!heardI)
                oldest = i;
        } else if (heardI) {
            if ((uint32_t)(millis() - heardAtMs[i]) > (uint32_t)(millis() - heardAtMs[oldest]))
                oldest = i;
        } else if (passLog[i].lastSeen < passLog[oldest].lastSeen) {
            oldest = i;
        }
    }
    return oldest;
}

uint32_t StreetPassModule::secondsSince(int slot) const
{
    if (heardAtMs[slot])
        return (uint32_t)(millis() - heardAtMs[slot]) / 1000;
    const uint32_t now = getValidTime(RTCQualityFromNet);
    if (now && passLog[slot].lastSeen && now >= passLog[slot].lastSeen)
        return now - passLog[slot].lastSeen;
    return UINT32_MAX;
}

bool StreetPassModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return p->from != 0 && p->from != nodeDB->getNodeNum();
}

ProcessMessage StreetPassModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Only packets that reached us straight from the sender's own radio count as "nearby"
    if (mp.transport_mechanism != meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA || mp.via_mqtt)
        return ProcessMessage::CONTINUE;
    if (getHopsAway(mp) != 0)
        return ProcessMessage::CONTINUE;

    const uint32_t now = getValidTime(RTCQualityFromNet); // 0 until the clock is set
    int slot = find(mp.from);
    bool isNew = false, isEncounter = false;

    if (slot < 0) {
        slot = allocSlot();
        passLog[slot] = Pass{};
        passLog[slot].node = mp.from;
        passLog[slot].firstSeen = now;
        heardAtMs[slot] = 0;
        isNew = isEncounter = true;
    } else {
        const uint32_t gap = secondsSince(slot);
        isEncounter = gap == UINT32_MAX || gap >= ENCOUNTER_GAP_SECS;
    }

    Pass &p = passLog[slot];
    if (isEncounter && p.count < UINT16_MAX)
        p.count++;
    if (now)
        p.lastSeen = now;
    heardAtMs[slot] = millis() ? millis() : 1;
    p.snr = (int8_t)constrain((int)lroundf(mp.rx_snr), -127, 127);
    if (mp.has_rx_rssi)
        p.rssi = (int16_t)mp.rx_rssi;

    const meshtastic_NodeInfoLite *info = nodeDB->getMeshNode(mp.from);
    if (nodeInfoLiteHasUser(info) && info->short_name[0]) {
        strncpy(p.name, info->short_name, sizeof(p.name) - 1);
        p.name[sizeof(p.name) - 1] = 0;
    }

    stale = true;
    if (isEncounter) {
        dirty = true;
        LOG_INFO("StreetPass: %s !%08x (%s) pass #%u, snr %d dB, rssi %d dBm", isNew ? "NEW" : "again", (unsigned)p.node,
                 p.name[0] ? p.name : "?", (unsigned)p.count, (int)p.snr, (int)p.rssi);
#if HAS_SCREEN
        if (isNew && screen) {
            char msg[40];
            if (p.name[0])
                snprintf(msg, sizeof(msg), "StreetPass!\n%s", p.name);
            else
                snprintf(msg, sizeof(msg), "StreetPass!\n!%08x", (unsigned)p.node);
            screen->showSimpleBanner(msg, 4000);
        }
#endif
        requestRedraw();
    }
    return ProcessMessage::CONTINUE;
}

int32_t StreetPassModule::runOnce()
{
    if (dirty || (stale && Throttle::hasElapsed(lastSaveMs, STALE_SAVE_MS)))
        save();
    return TICK_MS;
}

void StreetPassModule::requestRedraw()
{
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REDRAW_ONLY;
    notifyObservers(&e);
}

void StreetPassModule::load()
{
#ifdef FSCom
    SaveFile *sf = new SaveFile;
    bool ok = false;
    {
        concurrency::LockGuard g(spiLock);
        auto f = FSCom.open(SAVE_PATH, FILE_O_READ);
        if (f) {
            ok = f.read(reinterpret_cast<uint8_t *>(sf), sizeof(*sf)) == sizeof(*sf);
            f.close();
        }
    }
    if (ok && sf->magic == FILE_MAGIC && sf->version == FILE_VERSION && crc32Buffer(sf, offsetof(SaveFile, crc)) == sf->crc) {
        memcpy(passLog, sf->entries, sizeof(passLog));
        for (Pass &p : passLog)
            p.name[sizeof(p.name) - 1] = 0;
    } else if (ok) {
        LOG_WARN("StreetPass: saved log is damaged or from another version, starting fresh");
    }
    delete sf;
#endif
    lastSaveMs = millis();
}

void StreetPassModule::save()
{
    dirty = stale = false;
    lastSaveMs = millis();
#ifdef FSCom
    {
        concurrency::LockGuard g(spiLock);
        FSCom.mkdir("/prefs");
    }
    SaveFile *sf = new SaveFile;
    memset(sf, 0, sizeof(*sf));
    sf->magic = FILE_MAGIC;
    sf->version = FILE_VERSION;
    memcpy(sf->entries, passLog, sizeof(passLog));
    sf->crc = crc32Buffer(sf, offsetof(SaveFile, crc));

    auto out = SafeFile(SAVE_PATH, true);
    const size_t written = out.write(reinterpret_cast<uint8_t *>(sf), sizeof(*sf));
    if (!out.close() || written != sizeof(*sf))
        LOG_WARN("StreetPass: could not save the log");
    delete sf;
#endif
}

#if HAS_SCREEN
void StreetPassModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    display->clear();
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    graphics::drawCommonHeader(display, x, y, "StreetPass");

    const int *rows = graphics::getTextPositions(display);
    const int rowH = _fontHeight(FONT_SMALL);
    const int right = x + display->getWidth();
    int row = 1;
    char line[48];

    snprintf(line, sizeof(line), "Met %u  Passes %lu", (unsigned)nodesMet(), (unsigned long)totalPasses());
    display->drawString(x, y + rows[row++], line);

    // Most recently heard first; a slot is "taken" once drawn
    bool drawn[MAX_PASSES] = {};
    for (; row <= 6 && rows[row] + rowH <= display->getHeight(); row++) {
        int best = -1;
        uint32_t bestAge = 0;
        for (int i = 0; i < MAX_PASSES; i++) {
            if (!passLog[i].node || drawn[i])
                continue;
            const uint32_t age = secondsSince(i);
            if (best < 0 || age < bestAge) {
                best = i;
                bestAge = age;
            }
        }
        if (best < 0)
            break;
        drawn[best] = true;
        const Pass &p = passLog[best];

        char who[10];
        if (p.name[0])
            snprintf(who, sizeof(who), "%s", p.name);
        else
            snprintf(who, sizeof(who), "%04x", (unsigned)(p.node & 0xFFFF));
        char ago[8];
        formatAgo(ago, sizeof(ago), bestAge);

        snprintf(line, sizeof(line), "%s  x%u", who, (unsigned)p.count);
        display->drawString(x, y + rows[row], line);
        snprintf(line, sizeof(line), "%s  %ddB", ago, (int)p.snr);
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString(right, y + rows[row], line);
        display->setTextAlignment(TEXT_ALIGN_LEFT);
    }
    if (row == 2)
        display->drawString(x, y + rows[row], "Nobody nearby yet");

    graphics::drawCommonFooter(display, x, y);
}
#endif
