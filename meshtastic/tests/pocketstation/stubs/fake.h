// Minimal stand-ins for the Meshtastic firmware pieces the PocketStation module uses:
// a fake screen that records what is drawn, a fake keyboard, an in-memory filesystem and a clock.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define HAS_SCREEN 1

// ---- clock: fake (stepped by the test) or real ----
extern uint32_t fakeMs;
extern uint32_t fakeEpoch;
#ifdef REAL_CLOCK
inline uint32_t micros() { return (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline uint32_t millis() { return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
#else
inline uint32_t millis() { return fakeMs; }
inline uint32_t micros() { return fakeMs * 1000u; }
#endif
extern bool quietLog;
#define LOG_INFO(...) (quietLog ? 0 : (printf(__VA_ARGS__), printf("\n")))
#define LOG_WARN(...) (printf("WARN: " __VA_ARGS__), printf("\n"))
#define LOG_ERROR(...) (printf("ERROR: " __VA_ARGS__), printf("\n"))
#define LOG_DEBUG(...)
enum RTCQuality { RTCQualityNone, RTCQualityDevice, RTCQualityFromNet };
inline uint32_t getValidTime(RTCQuality, bool = false) { return fakeEpoch; }

// ---- observers ----
template <class T> class Observable;
template <class T> class Observer {
  public:
    virtual ~Observer() {}
    virtual int onNotify(T arg) = 0;
    void observe(Observable<T> *o) { o->observers.push_back(this); }
};
template <class T> class Observable {
  public:
    std::vector<Observer<T> *> observers;
    int notifyObservers(T arg) { for (auto *o : observers) { int r = o->onNotify(arg); if (r) return r; } return 0; }
};
template <class Callback, class T> class CallbackObserver : public Observer<T> {
    typedef int (Callback::*M)(T);
    Callback *obj; M method;
  public:
    CallbackObserver(Callback *o, M m) : obj(o), method(m) {}
    int onNotify(T arg) override { return (obj->*method)(arg); }
};

// ---- threads / locks ----
namespace concurrency {
struct OSThread { OSThread(const char *) {} virtual ~OSThread() {} void setIntervalFromNow(uint32_t) {} virtual int32_t runOnce() = 0; };
struct LockGuard { template <class L> LockGuard(L) {} };
}
extern int spiLock;

// ---- screen ----
enum OLEDDISPLAY_COLOR { BLACK = 0, WHITE = 1 };
enum OLEDDISPLAY_TEXT_ALIGNMENT { TEXT_ALIGN_LEFT, TEXT_ALIGN_RIGHT, TEXT_ALIGN_CENTER };
#define FONT_SMALL 0
inline int _fontHeight(int) { return 19; }
struct OLEDDisplayUiState {};
struct OLEDDisplay {
    static const int W = 240, H = 135;
    uint8_t px[W * H]; int color = WHITE; std::vector<std::string> text;
    void clear() { memset(px, 0, sizeof(px)); text.clear(); }
    void setColor(int c) { color = c; }
    void setTextAlignment(int) {}
    void setFont(int) {}
    int getWidth() { return W; }
    int getHeight() { return H; }
    void fillRect(int x, int y, int w, int h) { for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) if (i >= 0 && j >= 0 && i < W && j < H) px[j * W + i] = (uint8_t)color; }
    void drawString(int, int, const char *s) { text.push_back(s); }
};
namespace graphics {
inline void drawCommonHeader(OLEDDisplay *d, int, int, const char *t) { d->text.push_back(std::string("[") + t + "]"); }
inline const int *getTextPositions(OLEDDisplay *) { static int r[7] = {0, 22, 41, 60, 79, 98, 117}; return r; }
}
struct MeshModule;
struct Screen {
    bool showing = true;
    bool isShowingModuleFrame(const MeshModule *) const { return showing; }
    bool isOverlayBannerShowing() { return false; }
    std::string banner;
    void showSimpleBanner(const char *m, uint32_t = 0) { banner = m; }
};
extern Screen *screen;

// ---- keys ----
enum input_broker_event { INPUT_BROKER_NONE = 0, INPUT_BROKER_SELECT = 10, INPUT_BROKER_UP = 17, INPUT_BROKER_DOWN = 18, INPUT_BROKER_LEFT = 19,
                          INPUT_BROKER_RIGHT = 20, INPUT_BROKER_CANCEL = 24, INPUT_BROKER_BACK = 27, INPUT_BROKER_ANYKEY = 0xff };
struct InputEvent { const char *source; input_broker_event inputEvent; unsigned char kbchar; uint16_t touchX, touchY; };
struct InputBroker : Observable<const InputEvent *> {};
extern InputBroker *inputBroker;

// ---- power, memory ----
#define EVENT_PRESS 1
struct FakeFsm { int presses = 0; void trigger(int) { presses++; } };
extern FakeFsm powerFSM;
struct MemGet { uint32_t freeHeap = 123456; uint32_t getFreeHeap() { return freeHeap; } };
extern MemGet memGet;

// ---- modules ----
struct UIFrameEvent { enum Action { REDRAW_ONLY } action = REDRAW_ONLY; };
struct meshtastic_MeshPacket {};
struct MeshModule {
    MeshModule(const char *) {}
    virtual ~MeshModule() {}
    virtual bool wantPacket(const meshtastic_MeshPacket *p) = 0;
    virtual void drawFrame(OLEDDisplay *, OLEDDisplayUiState *, int16_t, int16_t) {}
    virtual bool interceptingKeyboardInput() { return false; }
    virtual bool wantUIFrame() { return false; }
    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() { return nullptr; }
};

// ---- in-memory filesystem ----
extern std::map<std::string, std::string> fakeFiles;
#define FILE_O_READ 0
struct FakeFile {
    const std::string *d = nullptr; size_t pos = 0;
    explicit operator bool() const { return d != nullptr; }
    size_t read(uint8_t *b, size_t n) { size_t k = std::min(n, d->size() - pos); memcpy(b, d->data() + pos, k); pos += k; return k; }
    void close() {}
};
struct FakeFS {
    FakeFile open(const char *p, int) { FakeFile f; auto it = fakeFiles.find(p); if (it != fakeFiles.end()) f.d = &it->second; return f; }
    void mkdir(const char *) {}
};
extern FakeFS FSCom;
#define FSCom FSCom
struct SafeFile {
    std::string path, buf;
    SafeFile(const char *p, bool) : path(p) {}
    size_t write(const uint8_t *b, size_t n) { buf.append((const char *)b, n); return n; }
    bool close() { fakeFiles[path] = buf; return true; }
};
