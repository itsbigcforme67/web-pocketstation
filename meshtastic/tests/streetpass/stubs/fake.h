// Minimal stand-ins for the Meshtastic firmware pieces StreetPass uses, with a fake clock and an in-memory filesystem.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define HAS_SCREEN 0
extern uint32_t fakeMillis, fakeEpoch;
inline uint32_t millis() { return fakeMillis; }
#define constrain(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define LOG_INFO(...) (printf(__VA_ARGS__), printf("\n"))
#define LOG_WARN(...) (printf("WARN: " __VA_ARGS__), printf("\n"))
#define LOG_DEBUG(...)
enum RTCQuality { RTCQualityNone, RTCQualityDevice, RTCQualityFromNet };
inline uint32_t getValidTime(RTCQuality) { return fakeEpoch; }
struct UIFrameEvent { enum Action { REDRAW_ONLY } action = REDRAW_ONLY; };
extern int redraws;
template <class T> struct Observable { void notifyObservers(T) { redraws++; } };
namespace concurrency {
struct OSThread { OSThread(const char *) {} virtual ~OSThread() {} void setIntervalFromNow(uint32_t) {} virtual int32_t runOnce() = 0; };
struct LockGuard { template <class L> LockGuard(L) {} };
}
extern int spiLock;
enum ProcessMessage_ { };
enum class ProcessMessage { CONTINUE, STOP };
enum { meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA = 1 };
struct meshtastic_MeshPacket { uint32_t from = 0; int transport_mechanism = 1; bool via_mqtt = false; float rx_snr = 0; bool has_rx_rssi = true; int32_t rx_rssi = -80; int hops = 0; };
enum meshtastic_PortNum { meshtastic_PortNum_UNKNOWN_APP };
struct MeshModule {
    MeshModule(const char *) {}
    virtual ~MeshModule() {}
    bool isPromiscuous = false, encryptedOk = false;
    virtual bool wantPacket(const meshtastic_MeshPacket *p) = 0;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &) { return ProcessMessage::CONTINUE; }
    virtual bool wantUIFrame() { return false; }
    virtual Observable<const UIFrameEvent *> *getUIFrameObservable() { return nullptr; }
};
struct meshtastic_NodeInfoLite { uint32_t num; char short_name[5]; };
inline bool nodeInfoLiteHasUser(const meshtastic_NodeInfoLite *n) { return n != nullptr; }
struct NodeDB_ {
    std::map<uint32_t, meshtastic_NodeInfoLite> nodes;
    uint32_t getNodeNum() { return 0xAAAA0001; }
    meshtastic_NodeInfoLite *getMeshNode(uint32_t n) { auto it = nodes.find(n); return it == nodes.end() ? nullptr : &it->second; }
};
extern NodeDB_ *nodeDB;
inline int8_t getHopsAway(const meshtastic_MeshPacket &p) { return p.hops; }
struct Throttle { static bool hasElapsed(uint32_t last, uint32_t iv) { return (uint32_t)(millis() - last) >= iv; } };
// in-memory filesystem
extern std::map<std::string, std::string> fakeFiles;
extern int fileWrites;
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
struct SafeFile {
    std::string path, buf;
    SafeFile(const char *p, bool) : path(p) {}
    size_t write(const uint8_t *b, size_t n) { buf.append((const char *)b, n); return n; }
    bool close() { fakeFiles[path] = buf; fileWrites++; return true; }
};
#define FSCom FSCom
inline uint32_t crc32Buffer(const void *p, size_t n) { uint32_t c = ~0u; const uint8_t *b = (const uint8_t *)p; while (n--) { c ^= *b++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); } return ~c; }
