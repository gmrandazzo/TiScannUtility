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
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
        const void* range;
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
    out.fill(0); // white in Qt Format_Mono
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

} // namespace

// ---------------------------------------------------------------------------
// ScannerBackend::Impl (Linux / SANE)
// ---------------------------------------------------------------------------
struct ScannerBackend::Impl {
    std::function<void(const std::vector<ScannerInfo>&)> deviceListCb;
    std::function<void(const QImage&)> pageCb;
    std::function<void(bool, const std::string&)> finishedCb;
    std::function<void(const std::string&)> statusCb;

    std::thread browseThread;
    std::thread scanThread;
    std::atomic<bool> browsing{false};
    std::atomic<bool> scanning{false};
    std::atomic<bool> cancelRequested{false};
    SANE_Handle currentHandle = nullptr;
    std::mutex handleMutex;

    void postStatus(const std::string& msg) {
        if (!statusCb) return;
        QMetaObject::invokeMethod(qApp, [this, msg] {
            if (statusCb) statusCb(msg);
        }, Qt::QueuedConnection);
    }

    void postDevices(const std::vector<ScannerInfo>& devs) {
        if (!deviceListCb) return;
        QMetaObject::invokeMethod(qApp, [this, devs] {
            if (deviceListCb) deviceListCb(devs);
        }, Qt::QueuedConnection);
    }

    void postPage(const QImage& img) {
        if (!pageCb) return;
        QMetaObject::invokeMethod(qApp, [this, img] {
            if (pageCb) pageCb(img);
        }, Qt::QueuedConnection);
    }

    void postFinished(bool ok, const std::string& err) {
        scanning = false;
        if (!finishedCb) return;
        QMetaObject::invokeMethod(qApp, [this, ok, err] {
            if (finishedCb) finishedCb(ok, err);
        }, Qt::QueuedConnection);
    }

    void inspectDeviceCaps(const std::string& deviceName, bool* hasFeeder, bool* hasFlatbed, bool* supportsDuplex) {
        *hasFeeder = false;
        *hasFlatbed = true;
        *supportsDuplex = false;
        SANE_Handle h = nullptr;
        if (gSane.sane_open(deviceName.c_str(), &h) != SANE_STATUS_GOOD) return;

        SANE_Int numOpts = 0;
        gSane.sane_control_option(h, 0, SANE_ACTION_GET_VALUE, &numOpts, nullptr);

        for (SANE_Int i = 1; i < numOpts; ++i) {
            const SANE_Option_Descriptor* desc = gSane.sane_get_option_descriptor(h, i);
            if (!desc || !desc->name) continue;
            std::string optName = toLower(desc->name);
            if (optName == "source") {
                if (desc->constraint_type == SANE_CONSTRAINT_STRING_LIST && desc->constraint.string_list) {
                    for (int s = 0; desc->constraint.string_list[s] != nullptr; ++s) {
                        std::string src = toLower(desc->constraint.string_list[s]);
                        if (src.find("adf") != std::string::npos ||
                            src.find("feeder") != std::string::npos ||
                            src.find("automatic") != std::string::npos) {
                            *hasFeeder = true;
                        }
                        if (src.find("flatbed") != std::string::npos ||
                            src.find("platen") != std::string::npos) {
                            *hasFlatbed = true;
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
        gSane.sane_close(h);
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
            for (int i = 0; deviceList[i] != nullptr; ++i) {
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

        postDevices(detected);
        if (detected.empty()) {
            postStatus("No scanners found.");
        } else {
            postStatus(std::to_string(detected.size()) + " scanner(s) found.");
        }
        browsing = false;
    }

    void configureOptionString(SANE_Handle h, const std::string& name, const std::vector<std::string>& candidates) {
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
                                gSane.sane_control_option(h, i, SANE_ACTION_SET_VALUE, buf, nullptr);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }

    void configureOptionInt(SANE_Handle h, const std::string& name, int targetVal) {
        SANE_Int numOpts = 0;
        gSane.sane_control_option(h, 0, SANE_ACTION_GET_VALUE, &numOpts, nullptr);
        for (SANE_Int i = 1; i < numOpts; ++i) {
            const SANE_Option_Descriptor* desc = gSane.sane_get_option_descriptor(h, i);
            if (!desc || !desc->name) continue;
            if (toLower(desc->name) == name) {
                if (desc->type == SANE_TYPE_INT) {
                    SANE_Int val = targetVal;
                    gSane.sane_control_option(h, i, SANE_ACTION_SET_VALUE, &val, nullptr);
                    return;
                } else if (desc->type == SANE_TYPE_FIXED) {
                    SANE_Int val = targetVal << 16;
                    gSane.sane_control_option(h, i, SANE_ACTION_SET_VALUE, &val, nullptr);
                    return;
                }
            }
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

        // Configure Resolution
        configureOptionInt(h, "resolution", options.dpi);

        // Configure Color Mode
        if (options.colorMode == ColorMode::Color) {
            configureOptionString(h, "mode", {"Color", "Colour", "RGB"});
        } else {
            // For Gray and BlackWhite, pull 8-bit Gray and threshold in software
            configureOptionString(h, "mode", {"Gray", "Grayscale", "Grey", "Color"});
        }

        // Configure Source / Duplex
        if (options.source == ScanSource::Flatbed) {
            configureOptionString(h, "source", {"Flatbed", "Platen", "Normal"});
        } else if (options.source == ScanSource::Feeder) {
            configureOptionString(h, "source", {"ADF", "Automatic Document Feeder", "Feeder"});
        }

        if (options.duplex) {
            configureOptionString(h, "source", {"ADF Duplex", "Duplex", "Automatic Document Feeder", "ADF"});
            configureOptionString(h, "duplex", {"true", "yes", "on", "1"});
        }

        int pagesReceived = 0;
        postStatus("Scanning...");

        while (!cancelRequested) {
            SANE_Status status = gSane.sane_start(h);
            if (status == SANE_STATUS_NO_DOCS) {
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
            if (height > 0) buffer.reserve(size_t(height) * bpl);

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
                height = int(buffer.size() / bpl);
            }

            if (height > 0 && width > 0 && !buffer.empty()) {
                QImage pageImg;
                if (params.format == SANE_FRAME_RGB && depth == 8) {
                    QImage rgb(width, height, QImage::Format_RGB888);
                    size_t copyBytes = std::min(buffer.size(), size_t(rgb.sizeInBytes()));
                    std::memcpy(rgb.bits(), buffer.data(), copyBytes);
                    pageImg = rgb.copy();
                } else if (params.format == SANE_FRAME_GRAY && depth == 8) {
                    QImage gray(width, height, QImage::Format_Grayscale8);
                    size_t copyBytes = std::min(buffer.size(), size_t(gray.sizeInBytes()));
                    std::memcpy(gray.bits(), buffer.data(), copyBytes);
                    pageImg = gray.copy();
                } else if (depth == 1) {
                    QImage mono(width, height, QImage::Format_Mono);
                    size_t copyBytes = std::min(buffer.size(), size_t(mono.sizeInBytes()));
                    std::memcpy(mono.bits(), buffer.data(), copyBytes);
                    pageImg = mono.copy();
                } else {
                    // Fallback to Grayscale8
                    QImage gray(width, height, QImage::Format_Grayscale8);
                    size_t copyBytes = std::min(buffer.size(), size_t(gray.sizeInBytes()));
                    std::memcpy(gray.bits(), buffer.data(), copyBytes);
                    pageImg = gray.copy();
                }

                if (!pageImg.isNull()) {
                    int dpm = int(options.dpi / 0.0254 + 0.5);
                    pageImg.setDotsPerMeterX(dpm);
                    pageImg.setDotsPerMeterY(dpm);

                    if (options.colorMode == ColorMode::BlackWhite) {
                        pageImg = thresholdToMono(pageImg);
                    }

                    pagesReceived++;
                    postPage(pageImg);
                }
            }

            // If flatbed or single page, stop after one page
            // Otherwise, loop to attempt next feeder page
            if (!params.last_frame) {
                // multi-frame color (red, green, blue) not common in modern scanners, but avoid infinite loop
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(handleMutex);
            currentHandle = nullptr;
        }
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
    stopBrowsing();
    cancelScan();
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
    if (d->browsing) return;
    d->browsing = true;
    if (d->browseThread.joinable()) d->browseThread.join();
    d->browseThread = std::thread([this] { d->browseWorker(); });
}

void ScannerBackend::stopBrowsing() {
    d->browsing = false;
    if (d->browseThread.joinable()) d->browseThread.join();
}

void ScannerBackend::scan(const std::string& deviceUuid, const ScanOptions& options) {
    if (d->scanning) return;
    d->scanning = true;
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
