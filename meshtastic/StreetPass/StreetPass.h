// StreetPass: keeps a log of Meshtastic nodes heard directly over LoRa (zero hops), like the 3DS feature.
// Drop-in optional module: lives in src/modules/optional/StreetPass/ and needs no edits elsewhere.
#pragma once

#include "configuration.h"
#include "Observer.h"
#include "concurrency/OSThread.h"
#include "mesh/MeshModule.h"

void setupStreetPass();

class StreetPassModule : public MeshModule, public Observable<const UIFrameEvent *>, private concurrency::OSThread
{
  public:
    static constexpr uint8_t MAX_PASSES = 64;
    // Hearing a node again after this long counts as a new encounter
    static constexpr uint32_t ENCOUNTER_GAP_SECS = 30 * 60;

    // One remembered node. Saved to flash as-is, so keep the layout stable (bump FILE_VERSION if it changes).
    struct __attribute__((packed)) Pass {
        uint32_t node;      // node number, 0 = empty slot
        uint32_t firstSeen; // epoch seconds, 0 = clock was not set
        uint32_t lastSeen;  // epoch seconds, 0 = clock was not set
        uint16_t count;     // number of encounters
        int16_t rssi;       // last signal strength, dBm
        int8_t snr;         // last signal-to-noise, dB
        char name[5];       // short name if known, else ""
    };

    StreetPassModule();

    uint8_t nodesMet() const;
    uint32_t totalPasses() const;
    const Pass *passes() const { return passLog; }

#if HAS_SCREEN
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif

  protected:
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual int32_t runOnce() override;
    virtual bool wantUIFrame() override { return true; }
    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }

  private:
    Pass passLog[MAX_PASSES] = {};
    uint32_t heardAtMs[MAX_PASSES] = {}; // millis() of the last packet this boot, 0 = not heard since boot
    bool dirty = false;                  // an encounter happened: save soon
    bool stale = false;                  // only "last seen" moved: save eventually
    uint32_t lastSaveMs = 0;

    int find(uint32_t node) const;
    int allocSlot();
    uint32_t secondsSince(int slot) const; // UINT32_MAX = unknown
    void load();
    void save();
    void requestRedraw();
};

extern StreetPassModule *streetPassModule;
