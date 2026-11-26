/*
 * TiScannUtility — scan, rearrange, rotate and merge pages into a final PDF.
 * Copyright (C) 2026 Giuseppe Marco Randazzo <gmrandazzo@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Qt is linked dynamically; the Qt libraries remain under the LGPL.
 */
#include "ScannerBackend.h"

#if defined(Q_OS_LINUX) || (defined(__unix__) && !defined(__APPLE__))

#include <QCoreApplication>
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// SANE C API standard definitions (SANE standard v1.0)
// This avoids strict compile-time dependency on libsane-dev headers while
// dynamically resolving symbols from libsane.so.1 at runtime.
// ---------------------------------------------------------------------------
extern "C" {

typedef int SANE_Int;
typedef unsigned char SANE_Byte;
typedef int SANE_Bool;
#define SANE_FALSE 0
#define SANE_TRUE 1
typedef char SANE_Char;
typedef const char* SANE_String_Const;
typedef char* SANE_String;
typedef void* SANE_Handle;

typedef enum {
    SANE_STATUS_GOOD = 0,
    SANE_STATUS_UNSUPPORTED,
    SANE_STATUS_CANCELLED,
    SANE_STATUS_DEVICE_BUSY,
    SANE_STATUS_INVAL,
    SANE_STATUS_EOF,
    SANE_STATUS_JAMMED,
    SANE_STATUS_NO_DOCS,
    SANE_STATUS_COVER_OPEN,
    SANE_STATUS_IO_ERROR,
    SANE_STATUS_NO_MEM,
    SANE_STATUS_ACCESS_DENIED
} SANE_Status;

typedef enum {
    SANE_TYPE_BOOL = 0,
    SANE_TYPE_INT,
    SANE_TYPE_FIXED,
    SANE_TYPE_STRING,
    SANE_TYPE_BUTTON,
    SANE_TYPE_GROUP
} SANE_Value_Type;

typedef enum {
    SANE_UNIT_NONE = 0,
    SANE_UNIT_PIXEL,
    SANE_UNIT_BIT,
    SANE_UNIT_MM,
    SANE_UNIT_DPI,
    SANE_UNIT_PERCENT,
    SANE_UNIT_MICROSECOND
} SANE_Unit;

typedef enum {
    SANE_CONSTRAINT_NONE = 0,
    SANE_CONSTRAINT_RANGE,
    SANE_CONSTRAINT_WORD_LIST,
    SANE_CONSTRAINT_STRING_LIST
} SANE_Constraint_Type;

typedef struct {
    SANE_Int min;
    SANE_Int max;
    SANE_Int quant;
} SANE_Range;

typedef struct {
    SANE_String_Const name;
    SANE_String_Const title;
    SANE_String_Const desc;
    SANE_Value_Type type;
    SANE_Unit unit;
    SANE_Int size;
    SANE_Int cap;
    SANE_Constraint_Type constraint_type;
    union {
        const SANE_String_Const* string_list;
        const SANE_Int* word_list;
        const SANE_Range* range;
    } constraint;
} SANE_Option_Descriptor;

typedef enum {
    SANE_ACTION_GET_VALUE = 0,
    SANE_ACTION_SET_VALUE,
    SANE_ACTION_SET_AUTO
} SANE_Action;

typedef enum {
    SANE_FRAME_GRAY = 0,
    SANE_FRAME_RGB,
    SANE_FRAME_RED,
    SANE_FRAME_GREEN,
    SANE_FRAME_BLUE
} SANE_Frame;

typedef struct {
    SANE_Frame format;
    SANE_Bool last_frame;
    SANE_Int lines;
    SANE_Int depth;
    SANE_Int pixels_per_line;
    SANE_Int bytes_per_line;
} SANE_Parameters;

typedef struct {
    SANE_String_Const name;
    SANE_String_Const vendor;
    SANE_String_Const model;
    SANE_String_Const type;
} SANE_Device;

typedef void (*SANE_Auth_Callback)(SANE_String_Const resource,
                                   SANE_Char* username,
                                   SANE_Char* password);

} // extern "C"

namespace {

struct SaneFunctions {
    void* libHandle = nullptr;
    SANE_Status (*sane_init)(SANE_Int* version_code, SANE_Auth_Callback authorize) = nullptr;
    void (*sane_exit)(void) = nullptr;
    SANE_Status (*sane_get_devices)(const SANE_Device*** device_list, SANE_Bool local_only) = nullptr;
    SANE_Status (*sane_open)(SANE_String_Const devicename, SANE_Handle* handle) = nullptr;
    void (*sane_close)(SANE_Handle handle) = nullptr;
    const SANE_Option_Descriptor* (*sane_get_option_descriptor)(SANE_Handle handle, SANE_Int option) = nullptr;
    SANE_Status (*sane_control_option)(SANE_Handle handle, SANE_Int option, SANE_Action action, void* val, SANE_Int* info) = nullptr;
    SANE_Status (*sane_get_parameters)(SANE_Handle handle, SANE_Parameters* params) = nullptr;
    SANE_Status (*sane_start)(SANE_Handle handle) = nullptr;
    SANE_Status (*sane_read)(SANE_Handle handle, SANE_Byte* data, SANE_Int max_length, SANE_Int* length) = nullptr;
    void (*sane_cancel)(SANE_Handle handle) = nullptr;
    SANE_String_Const (*sane_strstatus)(SANE_Status status) = nullptr;

    bool load() {
        if (libHandle) return true;
        const char* libNames[] = {"libsane.so.1", "libsane.so", "libsane.so.0"};
        for (const char* name : libNames) {
            libHandle = dlopen(name, RTLD_LAZY);
            if (libHandle) break;
        }
        if (!libHandle) return false;

#define LOAD_SYM(name) name = reinterpret_cast<decltype(name)>(dlsym(libHandle, #name))
        LOAD_SYM(sane_init);
        LOAD_SYM(sane_exit);
        LOAD_SYM(sane_get_devices);
        LOAD_SYM(sane_open);
        LOAD_SYM(sane_close);
        LOAD_SYM(sane_get_option_descriptor);
        LOAD_SYM(sane_control_option);
        LOAD_SYM(sane_get_parameters);
        LOAD_SYM(sane_start);
        LOAD_SYM(sane_read);
        LOAD_SYM(sane_cancel);
        LOAD_SYM(sane_strstatus);
#undef LOAD_SYM

        return sane_init && sane_exit && sane_get_devices && sane_open && sane_close
            && sane_get_option_descriptor && sane_control_option && sane_get_parameters
            && sane_start && sane_read && sane_cancel;
    }

    void unload() {
        if (libHandle) {
            dlclose(libHandle);
            libHandle = nullptr;
        }
    }
};

static SaneFunctions gSane;
static std::mutex gSaneMutex;
static int gSaneInitCount = 0;

static bool initSaneGlobal() {
    std::lock_guard<std::mutex> lock(gSaneMutex);
    if (gSaneInitCount > 0) {
        gSaneInitCount++;
        return true;
    }
    if (!gSane.load()) return false;
    SANE_Int version = 0;
    if (gSane.sane_init(&version, nullptr) != SANE_STATUS_GOOD) {
        return false;
    }
    gSaneInitCount = 1;
    return true;
}

static void exitSaneGlobal() {
    std::lock_guard<std::mutex> lock(gSaneMutex);
    if (gSaneInitCount > 0) {
        gSaneInitCount--;
        if (gSaneInitCount == 0 && gSane.sane_exit) {
            gSane.sane_exit();
        }
    }
}

static QImage thresholdToMono(const QImage& src) {
    QImage gray = src.convertToFormat(QImage::Format_Grayscale8);
    QImage out(gray.width(), gray.height(), QImage::Format_Mono);
    out.setDotsPerMeterX(gray.dotsPerMeterX());
    out.setDotsPerMeterY(gray.dotsPerMeterY());
    out.setColor(0, qRgb(255, 255, 255)); // 0 = white
    out.setColor(1, qRgb(0, 0, 0));       // 1 = black
    out.fill(0);                          // fill all white
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* s = gray.constScanLine(y);
        uchar* d = out.scanLine(y);
        for (int x = 0; x < gray.width(); ++x) {
            if (s[x] < 128)
                d[x >> 3] |= uchar(0x80 >> (x & 7));
        }
    }
    return out;
}

static std::string toLower(const std::string& str) {
    std::string out = str;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// ---------------------------------------------------------------------------
// Lightweight userspace mDNS fallback for eSCL scanners (_uscan._tcp / _uscans._tcp).
// Allows discovering network eSCL scanners even when avahi-daemon is disabled.
// ---------------------------------------------------------------------------
struct MdnsEsclDevice {
    std::string name;
    std::string scheme; // "http" or "https"
    std::string ip;
    uint16_t port = 80;
};

static uint16_t readBe16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

static bool readDnsName(const uint8_t* pkt, size_t pktLen, size_t& offset, std::string& outName) {
    outName.clear();
    size_t pos = offset;
    bool jumped = false;
    int jumps = 0;

    while (pos < pktLen) {
        uint8_t len = pkt[pos];
        if (len == 0) {
            if (!jumped) offset = pos + 1;
            return true;
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= pktLen) return false;
            uint16_t ptr = static_cast<uint16_t>(((len & 0x3F) << 8) | pkt[pos + 1]);
            if (ptr >= pktLen) return false;
            if (!jumped) offset = pos + 2;
            pos = ptr;
            jumped = true;
            if (++jumps > 32) return false;
        } else if ((len & 0xC0) == 0) {
            ++pos;
            if (pos + len > pktLen) return false;
            if (!outName.empty()) outName.push_back('.');
            outName.append(reinterpret_cast<const char*>(pkt + pos), len);
            pos += len;
        } else {
            return false;
        }
    }
    return false;
}

static std::vector<uint8_t> buildMdnsPtrQuery(const std::string& serviceName) {
    std::vector<uint8_t> pkt(12, 0);
    pkt[5] = 1; // QDCOUNT = 1

    size_t start = 0;
    while (start < serviceName.size()) {
        size_t dot = serviceName.find('.', start);
        if (dot == std::string::npos) dot = serviceName.size();
        size_t labelLen = dot - start;
        if (labelLen == 0 || labelLen > 63) break;
        pkt.push_back(static_cast<uint8_t>(labelLen));
        pkt.insert(pkt.end(), serviceName.begin() + static_cast<ptrdiff_t>(start),
                   serviceName.begin() + static_cast<ptrdiff_t>(dot));
        start = dot + 1;
    }
    pkt.push_back(0);
    // QTYPE = PTR (12), QCLASS = IN (1)
    pkt.push_back(0x00);
    pkt.push_back(0x0C);
    pkt.push_back(0x00);
    pkt.push_back(0x01);
    return pkt;
}

static bool hasEsclConfig() {
    const char* envDir = std::getenv("SANE_CONFIG_DIR");
    if (envDir && *envDir) {
        std::string dirs(envDir);
        size_t pos = 0;
        while (pos <= dirs.size()) {
            size_t sep = dirs.find(':', pos);
            std::string dir = (sep == std::string::npos) ? dirs.substr(pos) : dirs.substr(pos, sep - pos);
            if (!dir.empty()) {
                std::string candidate = dir + "/escl.conf";
                if (::access(candidate.c_str(), R_OK) == 0) return true;
            }
            if (sep == std::string::npos) break;
            pos = sep + 1;
        }
        return false;
    }
    return ::access("/etc/sane.d/escl.conf", R_OK) == 0;
}

static std::vector<MdnsEsclDevice> discoverMdnsEsclDevices(const std::atomic<bool>& cancelFlag, int timeoutMs = 1800) {
    std::vector<MdnsEsclDevice> results;
    int unicastFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (unicastFd < 0) return results;

    int reuse = 1;
    ::setsockopt(unicastFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    unsigned char ttl = 255;
    ::setsockopt(unicastFd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    // Also try opening a multicast listener on port 5353 so we catch both unicast
    // and multicast mDNS replies (falls back gracefully to unicastFd alone if bind fails).
    int mcastFd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mcastFd >= 0) {
        ::setsockopt(mcastFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        ::setsockopt(mcastFd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif
        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(5353);
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        ip_mreq mreq{};
        ::inet_pton(AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::bind(mcastFd, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0 ||
            ::setsockopt(mcastFd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            ::close(mcastFd);
            mcastFd = -1;
        }
    }

    sockaddr_in mcast{};
    mcast.sin_family = AF_INET;
    mcast.sin_port = htons(5353);
    ::inet_pton(AF_INET, "224.0.0.251", &mcast.sin_addr);

    const std::vector<std::string> services = {"_uscan._tcp.local", "_uscans._tcp.local"};
    auto sendQueries = [&]() {
        for (const auto& svc : services) {
            std::vector<uint8_t> query = buildMdnsPtrQuery(svc);
            ::sendto(unicastFd, query.data(), query.size(), 0, reinterpret_cast<const sockaddr*>(&mcast), sizeof(mcast));
        }
    };
    sendQueries();

    struct SrvInfo {
        uint16_t port = 0;
        std::string target;
    };
    struct InstanceInfo {
        std::string rawName;
        std::string scheme; // "http" or "https"
        std::string fallbackIp;
    };

    std::map<std::string, InstanceInfo> instances;   // key = lowercase instance fqdn
    std::map<std::string, SrvInfo> srvMap;           // key = lowercase instance fqdn
    std::map<std::string, std::string> txtDisplay;   // key = lowercase instance fqdn -> ty=
    std::map<std::string, std::string> hostIps;      // key = lowercase target host -> ipv4

    auto startTime = std::chrono::steady_clock::now();
    auto retryTime = startTime + std::chrono::milliseconds(500);
    auto deadline = startTime + std::chrono::milliseconds(timeoutMs);
    bool retried = false;
    uint8_t buf[4096];

    auto processPacket = [&](const uint8_t* pkt, size_t pktLen, const std::string& senderIp) {
        if (pktLen < 12) return;
        // Ignore pure queries (QR bit == 0)
        if ((pkt[2] & 0x80) == 0) return;

        uint16_t qdCount = readBe16(pkt + 4);
        uint16_t anCount = readBe16(pkt + 6);
        uint16_t nsCount = readBe16(pkt + 8);
        uint16_t arCount = readBe16(pkt + 10);

        size_t off = 12;
        for (uint16_t i = 0; i < qdCount; ++i) {
            std::string qname;
            if (!readDnsName(pkt, pktLen, off, qname) || off + 4 > pktLen) return;
            off += 4;
        }

        uint32_t totalRRs = uint32_t(anCount) + uint32_t(nsCount) + uint32_t(arCount);
        for (uint32_t i = 0; i < totalRRs && off < pktLen; ++i) {
            std::string rrName;
            if (!readDnsName(pkt, pktLen, off, rrName) || off + 10 > pktLen) break;
            uint16_t rrType = readBe16(pkt + off);
            uint16_t rdLen = readBe16(pkt + off + 8);
            off += 10;
            if (off + rdLen > pktLen) break;

            const size_t rdStart = off;
            const size_t rdEnd = off + rdLen;
            std::string lowerName = toLower(rrName);

            if (rrType == 12) { // PTR
                std::string ptrTarget;
                size_t ptrOff = rdStart;
                if (readDnsName(pkt, pktLen, ptrOff, ptrTarget)) {
                    if (lowerName == "_uscan._tcp.local" || lowerName == "_uscans._tcp.local") {
                        std::string key = toLower(ptrTarget);
                        InstanceInfo& inst = instances[key];
                        inst.rawName = ptrTarget;
                        inst.scheme = (lowerName == "_uscans._tcp.local") ? "https" : "http";
                        if (inst.fallbackIp.empty()) inst.fallbackIp = senderIp;
                    }
                }
            } else if (rrType == 33 && rdLen >= 6) { // SRV
                uint16_t port = readBe16(pkt + rdStart + 4);
                size_t tgtOff = rdStart + 6;
                std::string target;
                if (readDnsName(pkt, pktLen, tgtOff, target)) {
                    srvMap[lowerName] = {port, toLower(target)};
                }
            } else if (rrType == 16) { // TXT
                size_t p = rdStart;
                while (p < rdEnd) {
                    uint8_t slen = pkt[p++];
                    if (p + slen > rdEnd) break;
                    std::string entry(reinterpret_cast<const char*>(pkt + p), slen);
                    p += slen;
                    if (entry.size() > 3 && toLower(entry.substr(0, 3)) == "ty=") {
                        txtDisplay[lowerName] = entry.substr(3);
                    }
                }
            } else if (rrType == 1 && rdLen == 4) { // A
                char ipBuf[INET_ADDRSTRLEN]{};
                ::inet_ntop(AF_INET, pkt + rdStart, ipBuf, sizeof(ipBuf));
                hostIps[lowerName] = ipBuf;
            }

            off = rdEnd;
        }
    };

    while (!cancelFlag.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        if (!retried && now >= retryTime && instances.empty()) {
            sendQueries();
            retried = true;
        }
        auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        int waitMs = static_cast<int>(std::min<long long>(remainingMs, 100));

        pollfd pfds[2]{};
        nfds_t nfds = 0;
        pfds[nfds].fd = unicastFd;
        pfds[nfds].events = POLLIN;
        ++nfds;
        if (mcastFd >= 0) {
            pfds[nfds].fd = mcastFd;
            pfds[nfds].events = POLLIN;
            ++nfds;
        }

        int pr = ::poll(pfds, nfds, waitMs);
        if (pr <= 0) continue;

        for (nfds_t k = 0; k < nfds; ++k) {
            if (!(pfds[k].revents & POLLIN)) continue;
            sockaddr_in sender{};
            socklen_t senderLen = sizeof(sender);
            ssize_t n = ::recvfrom(pfds[k].fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&sender), &senderLen);
            if (n < 12) continue;
            char senderIpBuf[INET_ADDRSTRLEN]{};
            ::inet_ntop(AF_INET, &sender.sin_addr, senderIpBuf, sizeof(senderIpBuf));
            processPacket(buf, static_cast<size_t>(n), senderIpBuf);
        }
    }

    ::close(unicastFd);
    if (mcastFd >= 0) ::close(mcastFd);

    std::set<std::string> seenEndpoints;
    for (const auto& kv : instances) {
        const std::string& key = kv.first;
        const InstanceInfo& inst = kv.second;

        uint16_t port = (inst.scheme == "https") ? 443 : 80;
        std::string ip = inst.fallbackIp;
        auto srvIt = srvMap.find(key);
        if (srvIt != srvMap.end()) {
            if (srvIt->second.port > 0) port = srvIt->second.port;
            auto ipIt = hostIps.find(srvIt->second.target);
            if (ipIt != hostIps.end() && !ipIt->second.empty()) {
                ip = ipIt->second;
            }
        }
        if (ip.empty()) continue;

        std::string endpointKey = inst.scheme + "://" + ip + ":" + std::to_string(port);
        if (!seenEndpoints.insert(endpointKey).second) continue;

        MdnsEsclDevice dev;
        auto tyIt = txtDisplay.find(key);
        if (tyIt != txtDisplay.end() && !tyIt->second.empty()) {
            dev.name = tyIt->second;
        } else {
            size_t dot = inst.rawName.find('.');
            dev.name = (dot != std::string::npos && dot > 0) ? inst.rawName.substr(0, dot) : inst.rawName;
        }
        dev.scheme = inst.scheme;
        dev.ip = ip;
        dev.port = port;
        results.push_back(std::move(dev));
    }

    return results;
}

} // namespace

// ---------------------------------------------------------------------------
// ScannerBackend::Impl (Linux / SANE)
// ---------------------------------------------------------------------------
struct ScannerBackend::Impl {
    std::function<void(const std::vector<ScannerInfo>&)> deviceListCb;
    std::function<void(const QImage&)> pageCb;
    std::function<void(bool, const std::string&)> finishedCb;
    std::function<void(const std::string&)> statusCb;

    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::thread browseThread;
    std::thread scanThread;
    std::atomic<bool> browsing{false};
    std::atomic<bool> cancelBrowse{false};
    std::atomic<bool> scanning{false};
    std::atomic<bool> cancelRequested{false};
    SANE_Handle currentHandle = nullptr;
    std::mutex handleMutex;

    void postStatus(const std::string& msg) {
        if (!statusCb || !qApp) return;
        auto guard = alive;
        QMetaObject::invokeMethod(qApp, [this, guard, msg] {
            if (*guard && statusCb) statusCb(msg);
        }, Qt::QueuedConnection);
    }

    void postDevices(const std::vector<ScannerInfo>& devs) {
        if (!deviceListCb || !qApp) return;
        auto guard = alive;
        QMetaObject::invokeMethod(qApp, [this, guard, devs] {
            if (*guard && deviceListCb) deviceListCb(devs);
        }, Qt::QueuedConnection);
    }

    void postPage(const QImage& img) {
        if (!pageCb || !qApp) return;
        auto guard = alive;
        QMetaObject::invokeMethod(qApp, [this, guard, img] {
            if (*guard && pageCb) pageCb(img);
        }, Qt::QueuedConnection);
    }

    void postFinished(bool ok, const std::string& err) {
        scanning = false;
        if (!finishedCb || !qApp) return;
        auto guard = alive;
        QMetaObject::invokeMethod(qApp, [this, guard, ok, err] {
            if (*guard && finishedCb) finishedCb(ok, err);
        }, Qt::QueuedConnection);
    }

    void inspectHandleCaps(SANE_Handle h, bool* hasFeeder, bool* hasFlatbed, bool* supportsDuplex) {
        *hasFeeder = false;
        *hasFlatbed = true;
        *supportsDuplex = false;

        SANE_Int numOpts = 0;
        gSane.sane_control_option(h, 0, SANE_ACTION_GET_VALUE, &numOpts, nullptr);

        bool sawSourceList = false;
        bool foundFlatbed = false;
        for (SANE_Int i = 1; i < numOpts; ++i) {
            const SANE_Option_Descriptor* desc = gSane.sane_get_option_descriptor(h, i);
            if (!desc || !desc->name) continue;
            std::string optName = toLower(desc->name);
            if (optName == "source") {
                if (desc->constraint_type == SANE_CONSTRAINT_STRING_LIST && desc->constraint.string_list) {
                    sawSourceList = true;
                    for (int s = 0; desc->constraint.string_list[s] != nullptr; ++s) {
                        std::string src = toLower(desc->constraint.string_list[s]);
                        if (src.find("adf") != std::string::npos ||
                            src.find("feeder") != std::string::npos ||
                            src.find("automatic") != std::string::npos) {
                            *hasFeeder = true;
                        }
                        if (src.find("flatbed") != std::string::npos ||
                            src.find("platen") != std::string::npos ||
                            src.find("normal") != std::string::npos) {
                            foundFlatbed = true;
                        }
                        if (src.find("duplex") != std::string::npos) {
                            *supportsDuplex = true;
                        }
                    }
                }
            } else if (optName == "duplex") {
                *supportsDuplex = true;
            }
        }
        if (sawSourceList) {
            *hasFlatbed = foundFlatbed || !(*hasFeeder);
        }
    }

    bool inspectDeviceCaps(const std::string& deviceName, bool* hasFeeder, bool* hasFlatbed, bool* supportsDuplex) {
        *hasFeeder = false;
        *hasFlatbed = true;
        *supportsDuplex = false;
        SANE_Handle h = nullptr;
        if (gSane.sane_open(deviceName.c_str(), &h) != SANE_STATUS_GOOD) return false;
        inspectHandleCaps(h, hasFeeder, hasFlatbed, supportsDuplex);
        gSane.sane_close(h);
        return true;
    }

    void browseWorker() {
        postStatus("Looking for SANE scanners...");
        if (!initSaneGlobal()) {
            postStatus("SANE library (libsane.so.1) not found or failed to initialize.");
            browsing = false;
            return;
        }

        const SANE_Device** deviceList = nullptr;
        // Search all devices including network/AirScan/eSCL devices
        SANE_Status status = gSane.sane_get_devices(&deviceList, SANE_FALSE);
        if (status != SANE_STATUS_GOOD) {
            postStatus("Failed to enumerate SANE scanners.");
            browsing = false;
            return;
        }

        std::vector<ScannerInfo> detected;
        if (deviceList) {
            for (int i = 0; deviceList[i] != nullptr && !cancelBrowse.load(); ++i) {
                const SANE_Device* d = deviceList[i];
                ScannerInfo info;
                info.uuid = d->name ? d->name : "";
                std::string vendor = d->vendor ? d->vendor : "";
                std::string model = d->model ? d->model : "";
                if (vendor.empty() && model.empty()) {
                    info.name = info.uuid;
                } else if (vendor.empty()) {
                    info.name = model;
                } else if (model.empty()) {
                    info.name = vendor;
                } else {
                    info.name = vendor + " " + model;
                }
                info.ready = true;
                inspectDeviceCaps(info.uuid, &info.hasDocumentFeeder, &info.hasFlatbed, &info.supportsDuplex);
                detected.push_back(info);
            }
        }

        // Fallback when SANE finds no devices (e.g. avahi-daemon is disabled):
        // probe mDNS directly in userspace for eSCL devices and open via SANE's escl backend.
        // Guard with hasEsclConfig() because upstream libsane-escl dereferences NULL if escl.conf is missing.
        if (detected.empty() && !cancelBrowse.load() && hasEsclConfig()) {
            auto mdnsDevs = discoverMdnsEsclDevices(cancelBrowse);
            for (const auto& m : mdnsDevs) {
                if (cancelBrowse.load()) break;
                std::string esclUri = "escl:" + m.scheme + "://" + m.ip + ":" + std::to_string(m.port);
                ScannerInfo info;
                info.uuid = esclUri;
                info.name = m.name.empty() ? esclUri : m.name;
                info.ready = true;
                if (inspectDeviceCaps(info.uuid, &info.hasDocumentFeeder, &info.hasFlatbed, &info.supportsDuplex)) {
                    detected.push_back(std::move(info));
                }
            }
        }

        if (!cancelBrowse.load()) {
            postDevices(detected);
            if (detected.empty()) {
                if (::access("/run/avahi-daemon/socket", F_OK) != 0) {
                    postStatus("No scanners found. For network scanners, ensure avahi-daemon is running.");
                } else {
                    postStatus("No scanners found.");
                }
            } else {
                postStatus(std::to_string(detected.size()) + " scanner(s) found.");
            }
        }
        browsing = false;
    }

    bool configureOptionString(SANE_Handle h, const std::string& name, const std::vector<std::string>& candidates) {
        SANE_Int numOpts = 0;
        gSane.sane_control_option(h, 0, SANE_ACTION_GET_VALUE, &numOpts, nullptr);
        for (SANE_Int i = 1; i < numOpts; ++i) {
            const SANE_Option_Descriptor* desc = gSane.sane_get_option_descriptor(h, i);
            if (!desc || !desc->name) continue;
            if (toLower(desc->name) == name && desc->type == SANE_TYPE_STRING) {
                if (desc->constraint_type == SANE_CONSTRAINT_STRING_LIST && desc->constraint.string_list) {
                    for (const auto& cand : candidates) {
                        for (int s = 0; desc->constraint.string_list[s] != nullptr; ++s) {
                            if (toLower(desc->constraint.string_list[s]) == toLower(cand)) {
                                char buf[256];
                                std::strncpy(buf, desc->constraint.string_list[s], sizeof(buf) - 1);
                                buf[sizeof(buf) - 1] = '\0';
                                if (gSane.sane_control_option(h, i, SANE_ACTION_SET_VALUE, buf, nullptr) == SANE_STATUS_GOOD) {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    bool configureOptionBool(SANE_Handle h, const std::string& name, bool value) {
        SANE_Int numOpts = 0;
        gSane.sane_control_option(h, 0, SANE_ACTION_GET_VALUE, &numOpts, nullptr);
        for (SANE_Int i = 1; i < numOpts; ++i) {
            const SANE_Option_Descriptor* desc = gSane.sane_get_option_descriptor(h, i);
            if (!desc || !desc->name) continue;
            if (toLower(desc->name) == name && desc->type == SANE_TYPE_BOOL) {
                SANE_Bool v = value ? SANE_TRUE : SANE_FALSE;
                return gSane.sane_control_option(h, i, SANE_ACTION_SET_VALUE, &v, nullptr) == SANE_STATUS_GOOD;
            }
        }
        return false;
    }

    int configureOptionInt(SANE_Handle h, const std::string& name, int targetVal) {
        SANE_Int numOpts = 0;
        gSane.sane_control_option(h, 0, SANE_ACTION_GET_VALUE, &numOpts, nullptr);
        for (SANE_Int i = 1; i < numOpts; ++i) {
            const SANE_Option_Descriptor* desc = gSane.sane_get_option_descriptor(h, i);
            if (!desc || !desc->name) continue;
            if (toLower(desc->name) != name) continue;
            if (desc->type != SANE_TYPE_INT && desc->type != SANE_TYPE_FIXED) continue;

            const bool isFixed = (desc->type == SANE_TYPE_FIXED);
            SANE_Int rawTarget = isFixed ? (targetVal << 16) : targetVal;
            SANE_Int chosen = rawTarget;

            if (desc->constraint_type == SANE_CONSTRAINT_WORD_LIST && desc->constraint.word_list) {
                SANE_Int count = desc->constraint.word_list[0];
                if (count > 0) {
                    // Pick smallest supported value >= target, else largest supported value
                    bool haveGe = false;
                    SANE_Int bestGe = 0;
                    SANE_Int maxVal = desc->constraint.word_list[1];
                    for (SANE_Int k = 1; k <= count; ++k) {
                        SANE_Int v = desc->constraint.word_list[k];
                        if (v > maxVal) maxVal = v;
                        if (v >= rawTarget && (!haveGe || v < bestGe)) {
                            bestGe = v;
                            haveGe = true;
                        }
                    }
                    chosen = haveGe ? bestGe : maxVal;
                }
            } else if (desc->constraint_type == SANE_CONSTRAINT_RANGE && desc->constraint.range) {
                const SANE_Range* r = desc->constraint.range;
                chosen = std::clamp(rawTarget, r->min, r->max);
                if (r->quant > 0) {
                    chosen = r->min + ((chosen - r->min) / r->quant) * r->quant;
                }
            }

            gSane.sane_control_option(h, i, SANE_ACTION_SET_VALUE, &chosen, nullptr);
            SANE_Int actual = chosen;
            if (gSane.sane_control_option(h, i, SANE_ACTION_GET_VALUE, &actual, nullptr) == SANE_STATUS_GOOD) {
                return isFixed ? int(actual >> 16) : int(actual);
            }
            return isFixed ? int(chosen >> 16) : int(chosen);
        }
        return targetVal;
    }

    void selectScanSource(SANE_Handle h, bool feeder, bool duplex) {
        if (feeder) {
            if (duplex) {
                configureOptionString(h, "source",
                    {"ADF Duplex", "Duplex", "ADF Front/Back", "Automatic Document Feeder", "ADF", "Feeder"});
                if (!configureOptionBool(h, "duplex", true)) {
                    configureOptionString(h, "duplex", {"true", "yes", "on", "1"});
                }
            } else {
                configureOptionString(h, "source",
                    {"ADF", "ADF Front", "Automatic Document Feeder", "Feeder"});
                if (!configureOptionBool(h, "duplex", false)) {
                    configureOptionString(h, "duplex", {"false", "no", "off", "0"});
                }
            }
        } else {
            configureOptionString(h, "source", {"Flatbed", "Platen", "Normal"});
        }
    }

    void scanWorker(std::string deviceUuid, ScanOptions options) {
        postStatus("Connecting to scanner...");
        if (!initSaneGlobal()) {
            postFinished(false, "SANE library not available");
            return;
        }

        SANE_Handle h = nullptr;
        if (gSane.sane_open(deviceUuid.c_str(), &h) != SANE_STATUS_GOOD) {
            postFinished(false, "Failed to open scanner");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(handleMutex);
            currentHandle = h;
        }

        bool hasFeeder = false;
        bool hasFlatbed = true;
        bool supportsDuplex = false;
        inspectHandleCaps(h, &hasFeeder, &hasFlatbed, &supportsDuplex);

        bool usingFeeder = false;
        if (options.source == ScanSource::Flatbed) {
            usingFeeder = false;
        } else if (options.source == ScanSource::Feeder) {
            usingFeeder = true;
        } else { // ScanSource::Automatic
            usingFeeder = hasFeeder;
        }

        // Configure Source / Duplex first (some backends reset mode/resolution when source changes)
        selectScanSource(h, usingFeeder, options.duplex && supportsDuplex);

        // Configure Color Mode
        if (options.colorMode == ColorMode::Color) {
            configureOptionString(h, "mode", {"Color", "Colour", "RGB"});
        } else {
            // For Gray and BlackWhite, pull 8-bit Gray and threshold in software
            configureOptionString(h, "mode", {"Gray", "Grayscale", "Grey", "Color"});
        }

        // Configure Resolution
        int actualDpi = configureOptionInt(h, "resolution", options.dpi);

        int pagesReceived = 0;
        postStatus("Scanning...");

        while (!cancelRequested) {
            SANE_Status status = gSane.sane_start(h);
            if (status == SANE_STATUS_NO_DOCS) {
                // Automatic mode: if feeder is empty on first page, fall back to Flatbed
                if (pagesReceived == 0 && options.source == ScanSource::Automatic && usingFeeder && hasFlatbed) {
                    gSane.sane_cancel(h);
                    postStatus("Feeder empty, switching to Flatbed...");
                    usingFeeder = false;
                    selectScanSource(h, false, false);
                    actualDpi = configureOptionInt(h, "resolution", options.dpi);
                    continue;
                }
                // Feeder is empty
                break;
            }
            if (status != SANE_STATUS_GOOD) {
                if (pagesReceived == 0) {
                    const char* str = gSane.sane_strstatus ? gSane.sane_strstatus(status) : "Failed to start scan";
                    postFinished(false, str ? str : "Scan failed");
                } else {
                    postFinished(true, "");
                }
                {
                    std::lock_guard<std::mutex> lock(handleMutex);
                    currentHandle = nullptr;
                }
                gSane.sane_cancel(h);
                gSane.sane_close(h);
                return;
            }

            SANE_Parameters params{};
            if (gSane.sane_get_parameters(h, &params) != SANE_STATUS_GOOD) {
                break;
            }

            int width = params.pixels_per_line;
            int height = params.lines;
            int bpl = params.bytes_per_line;
            int depth = params.depth;

            if (width <= 0 || bpl <= 0) break;

            std::vector<uint8_t> buffer;
            if (height > 0) buffer.reserve(size_t(height) * size_t(bpl));

            const int kChunk = 64 * 1024;
            std::vector<uint8_t> chunk(kChunk);
            SANE_Int readLen = 0;
            bool eof = false;

            while (!eof && !cancelRequested) {
                SANE_Status readStatus = gSane.sane_read(h, chunk.data(), kChunk, &readLen);
                if (readStatus == SANE_STATUS_GOOD) {
                    if (readLen > 0) {
                        buffer.insert(buffer.end(), chunk.data(), chunk.data() + readLen);
                    }
                } else if (readStatus == SANE_STATUS_EOF) {
                    if (readLen > 0) {
                        buffer.insert(buffer.end(), chunk.data(), chunk.data() + readLen);
                    }
                    eof = true;
                } else {
                    break;
                }
            }

            if (cancelRequested) break;

            if (height <= 0 && bpl > 0) {
                height = int(buffer.size() / size_t(bpl));
            }

            if (height > 0 && width > 0 && !buffer.empty()) {
                // Copy row-by-row: SANE's bytes_per_line need not be 4-byte aligned,
                // whereas QImage scanlines are always 32-bit aligned.
                auto copyRows = [&](QImage& dst) {
                    const size_t rowBytes = std::min(size_t(bpl), size_t(dst.bytesPerLine()));
                    for (int y = 0; y < height; ++y) {
                        const size_t srcOff = size_t(y) * size_t(bpl);
                        if (srcOff >= buffer.size()) break;
                        const size_t avail = std::min(rowBytes, buffer.size() - srcOff);
                        std::memcpy(dst.scanLine(y), buffer.data() + srcOff, avail);
                    }
                };

                QImage pageImg;
                if (params.format == SANE_FRAME_RGB && depth == 8) {
                    pageImg = QImage(width, height, QImage::Format_RGB888);
                    copyRows(pageImg);
                } else if (params.format == SANE_FRAME_GRAY && depth == 8) {
                    pageImg = QImage(width, height, QImage::Format_Grayscale8);
                    copyRows(pageImg);
                } else if (depth == 1) {
                    pageImg = QImage(width, height, QImage::Format_Mono);
                    pageImg.setColor(0, qRgb(255, 255, 255));
                    pageImg.setColor(1, qRgb(0, 0, 0));
                    pageImg.fill(0);
                    copyRows(pageImg);
                } else {
                    pageImg = QImage(width, height, QImage::Format_Grayscale8);
                    copyRows(pageImg);
                }

                if (!pageImg.isNull()) {
                    int dpiToUse = actualDpi > 0 ? actualDpi : options.dpi;
                    int dpm = int(dpiToUse / 0.0254 + 0.5);
                    pageImg.setDotsPerMeterX(dpm);
                    pageImg.setDotsPerMeterY(dpm);

                    if (options.colorMode == ColorMode::BlackWhite) {
                        pageImg = thresholdToMono(pageImg);
                    }

                    pagesReceived++;
                    postPage(pageImg);
                }
            }

            // Flatbed scans a single page; only loop for document feeder batches.
            if (!usingFeeder || !params.last_frame) {
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(handleMutex);
            currentHandle = nullptr;
        }
        gSane.sane_cancel(h);
        gSane.sane_close(h);

        if (cancelRequested) {
            postFinished(false, "cancelled");
        } else if (pagesReceived == 0) {
            postFinished(false, "no pages received - is paper loaded?");
        } else {
            postFinished(true, "");
        }
    }
};

// ---------------------------------------------------------------------------
// ScannerBackend Public API (Linux / SANE)
// ---------------------------------------------------------------------------
ScannerBackend::ScannerBackend() : d(std::make_unique<Impl>()) {}

ScannerBackend::~ScannerBackend() {
    *d->alive = false;
    stopBrowsing();
    cancelScan();
    if (d->scanThread.joinable()) d->scanThread.join();
    if (d->browseThread.joinable()) d->browseThread.join();
    exitSaneGlobal();
}

void ScannerBackend::setDeviceListCallback(std::function<void(const std::vector<ScannerInfo>&)> cb) {
    d->deviceListCb = std::move(cb);
}

void ScannerBackend::setPageCallback(std::function<void(const QImage&)> cb) {
    d->pageCb = std::move(cb);
}

void ScannerBackend::setScanFinishedCallback(std::function<void(bool, const std::string&)> cb) {
    d->finishedCb = std::move(cb);
}

void ScannerBackend::setStatusCallback(std::function<void(const std::string&)> cb) {
    d->statusCb = std::move(cb);
}

void ScannerBackend::startBrowsing() {
    if (d->browsing.exchange(true)) return;
    d->cancelBrowse = false;
    if (d->browseThread.joinable()) d->browseThread.join();
    d->browseThread = std::thread([this] { d->browseWorker(); });
}

void ScannerBackend::stopBrowsing() {
    d->cancelBrowse = true;
    if (d->browseThread.joinable()) d->browseThread.join();
    d->browsing = false;
}

void ScannerBackend::scan(const std::string& deviceUuid, const ScanOptions& options) {
    if (d->scanning.exchange(true)) return;
    d->cancelRequested = false;
    if (d->scanThread.joinable()) d->scanThread.join();
    d->scanThread = std::thread([this, deviceUuid, options] {
        d->scanWorker(deviceUuid, options);
    });
}

void ScannerBackend::cancelScan() {
    d->cancelRequested = true;
    std::lock_guard<std::mutex> lock(d->handleMutex);
    if (d->currentHandle && gSane.sane_cancel) {
        gSane.sane_cancel(d->currentHandle);
    }
}

#endif // Q_OS_LINUX
