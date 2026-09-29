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

#import <Foundation/Foundation.h>
#import <ImageCaptureCore/ImageCaptureCore.h>

#include "ImageConversion.h"

#include <algorithm>

@class SUScannerDelegate;

// ---------------------------------------------------------------------------
// Black & White thresholding: 8-bit gray -> 1-bit (bit set = black).
// Kept in software because requesting 1-bit from the scanner is less reliable
// across ICA/AirScan devices than scanning gray and thresholding ourselves.
// ---------------------------------------------------------------------------
static QImage thresholdToMono(const QImage& src) {
    QImage gray = src.convertToFormat(QImage::Format_Grayscale8);
    QImage out(gray.width(), gray.height(), QImage::Format_Mono);
    out.setDotsPerMeterX(gray.dotsPerMeterX());
    out.setDotsPerMeterY(gray.dotsPerMeterY());
    out.fill(0); // all white
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

// ---------------------------------------------------------------------------
// State for one scan job
// ---------------------------------------------------------------------------
struct ScanJob {
    std::string uuid;
    ScanOptions options;
    bool waitingForUnits = false;       // scan() called before caps arrived
    bool pendingUnitSelection = false;  // waiting for didSelectFunctionalUnit
    bool scanning = false;
    bool cancelRequested = false;
    int pagesReceived = 0;

    // band accumulation for the page currently being transferred
    __strong NSMutableData* buffer = nil;
    bool headerParsed = false;
    NSUInteger width = 0, height = 0, bytesPerRow = 0;
    NSUInteger bitsPerPixel = 0, bitsPerComponent = 0, numComponents = 0;
    ICScannerPixelDataType pixelType = ICScannerPixelDataTypeGray;
    BOOL bigEndian = NO;
};

struct ScannerBackend::Impl {
    __strong ICDeviceBrowser* browser = nil;
    __strong NSMutableArray<ICScannerDevice*>* devices = nil;
    __strong SUScannerDelegate* delegate = nil;
    ScanJob job;
    std::vector<ScannerInfo> lastEmitted;
    std::function<void(const std::vector<ScannerInfo>&)> deviceListCb;
    std::function<void(const QImage&)> pageCb;
    std::function<void(bool, const std::string&)> finishedCb;
    std::function<void(const std::string&)> statusCb;

    void status(const std::string& msg) {
        if (statusCb) statusCb(msg);
    }

    static bool isReady(ICScannerDevice* s) {
        return s.hasOpenSession && s.availableFunctionalUnitTypes != nil;
    }

    ICScannerDevice* findScanner(const std::string& uuid) {
        for (ICScannerDevice* s in devices) {
            std::string id = s.UUIDString ? s.UUIDString.UTF8String : (s.name ? s.name.UTF8String : "");
            if (id == uuid) return s;
        }
        return nil;
    }

    void emitDevices() {
        if (!deviceListCb) return;
        std::vector<ScannerInfo> out;
        for (ICScannerDevice* s in devices) {
            ScannerInfo info;
            info.uuid = s.UUIDString ? s.UUIDString.UTF8String : (s.name ? s.name.UTF8String : "");
            info.name = s.name ? s.name.UTF8String : "Scanner";
            info.ready = isReady(s);
            info.hasDocumentFeeder = [s.availableFunctionalUnitTypes containsObject:@(ICScannerFunctionalUnitTypeDocumentFeeder)];
            info.hasFlatbed = [s.availableFunctionalUnitTypes containsObject:@(ICScannerFunctionalUnitTypeFlatbed)];
            ICScannerFunctionalUnit* unit = s.selectedFunctionalUnit;
            if (unit && unit.type == ICScannerFunctionalUnitTypeDocumentFeeder)
                info.supportsDuplex = [(ICScannerFunctionalUnitDocumentFeeder*)unit supportsDuplexScanning];
            out.push_back(info);
        }
        // only notify when something actually changed (the readiness poll ticks often)
        if (out.size() == lastEmitted.size()) {
            bool same = true;
            for (size_t i = 0; i < out.size(); ++i) {
                const ScannerInfo& a = out[i];
                const ScannerInfo& b = lastEmitted[i];
                if (a.uuid != b.uuid || a.name != b.name || a.ready != b.ready
                    || a.hasDocumentFeeder != b.hasDocumentFeeder || a.hasFlatbed != b.hasFlatbed || a.supportsDuplex != b.supportsDuplex) {
                    same = false;
                    break;
                }
            }
            if (same) return;
        }
        lastEmitted = out;
        deviceListCb(out);
    }

    void configureAndScan(ICScannerDevice* scanner) {
        ICScannerFunctionalUnit* unit = scanner.selectedFunctionalUnit;
        if (!unit) {
            job.scanning = job.pendingUnitSelection = false;
            if (finishedCb) finishedCb(false, "no functional unit selected");
            return;
        }

        // Smart Automatic mode: if we picked Feeder but it's empty, switch to Flatbed.
        if (job.options.source == ScanSource::Automatic && unit.type == ICScannerFunctionalUnitTypeDocumentFeeder) {
            if (![(ICScannerFunctionalUnitDocumentFeeder*)unit documentLoaded]) {
                bool hasFlatbed = [scanner.availableFunctionalUnitTypes containsObject:@(ICScannerFunctionalUnitTypeFlatbed)];
                if (hasFlatbed) {
                    status("Feeder empty, switching to Flatbed...");
                    job.pendingUnitSelection = true;
                    [scanner requestSelectFunctionalUnit:ICScannerFunctionalUnitTypeFlatbed];
                    return;
                }
            }
        }

        // resolution: smallest supported dpi >= requested, else largest available
        NSUInteger want = (NSUInteger)std::max(job.options.dpi, 75);
        NSUInteger best = 0;
        NSIndexSet* resolutions = unit.supportedResolutions;
        for (NSUInteger v = resolutions.firstIndex; v != NSNotFound; v = [resolutions indexGreaterThanIndex:v]) {
            if (v >= want && (best == 0 || v < best)) best = v;
        }
        if (best == 0) best = resolutions.lastIndex;
        if (best > 0) unit.resolution = best;

        // color mode: we always pull 8-bit data and threshold for B&W ourselves
        if (job.options.colorMode == ColorMode::Color)
            unit.pixelDataType = ICScannerPixelDataTypeRGB;
        else
            unit.pixelDataType = ICScannerPixelDataTypeGray;
        if ([unit.supportedBitDepths containsIndex:8])
            unit.bitDepth = ICScannerBitDepth8Bits;
        else if ([unit.supportedBitDepths containsIndex:16])
            unit.bitDepth = ICScannerBitDepth16Bits;

        // duplex only if the feeder really supports it
        if (unit.type == ICScannerFunctionalUnitTypeDocumentFeeder) {
            ICScannerFunctionalUnitDocumentFeeder* adf = (ICScannerFunctionalUnitDocumentFeeder*)unit;
            adf.duplexScanningEnabled = job.options.duplex && adf.supportsDuplexScanning;
        } else if (unit.type == ICScannerFunctionalUnitTypeFlatbed) {
            ICScannerFunctionalUnitFlatbed* flatbed = (ICScannerFunctionalUnitFlatbed*)unit;
            // Explicitly set the scan area to the physical glass size to prevent zero-size scans
            NSSize size = flatbed.physicalSize;
            if (size.width > 0 && size.height > 0) {
                flatbed.scanArea = (NSRect){{0, 0}, size};
            }
        }

        scanner.transferMode = ICScannerTransferModeMemoryBased;

        job.buffer = nil;
        job.headerParsed = false;
        job.pagesReceived = 0;
        job.pendingUnitSelection = false;
        job.scanning = true;
        status("Scanning...");
        [scanner requestScan];
    }

    void startScan(ICScannerDevice* scanner) {
        job.waitingForUnits = false;
        if (!isReady(scanner)) {
            if (finishedCb) finishedCb(false, "scanner capabilities not loaded yet — try again in a moment");
            return;
        }

        ICScannerFunctionalUnitType wanted = ICScannerFunctionalUnitTypeFlatbed;
        bool hasFeeder = [scanner.availableFunctionalUnitTypes containsObject:@(ICScannerFunctionalUnitTypeDocumentFeeder)];
        bool hasFlatbed = [scanner.availableFunctionalUnitTypes containsObject:@(ICScannerFunctionalUnitTypeFlatbed)];

        if (job.options.source == ScanSource::Feeder && hasFeeder) {
            wanted = ICScannerFunctionalUnitTypeDocumentFeeder;
        } else if (job.options.source == ScanSource::Flatbed && hasFlatbed) {
            wanted = ICScannerFunctionalUnitTypeFlatbed;
        } else {
            wanted = hasFeeder ? ICScannerFunctionalUnitTypeDocumentFeeder : ICScannerFunctionalUnitTypeFlatbed;
        }

        ICScannerFunctionalUnit* current = scanner.selectedFunctionalUnit;
        if (current && current.type == wanted) {
            configureAndScan(scanner);
        } else {
            job.pendingUnitSelection = true;
            [scanner requestSelectFunctionalUnit:wanted];
        }
    }

    void handleBand(ICScannerBandData* band) {
        if (!job.headerParsed) {
            job.width = band.fullImageWidth;
            job.height = band.fullImageHeight;
            job.bytesPerRow = band.bytesPerRow;
            job.bitsPerPixel = band.bitsPerPixel;
            job.bitsPerComponent = band.bitsPerComponent;
            job.numComponents = band.numComponents;
            job.pixelType = band.pixelDataType;
            job.bigEndian = band.isBigEndian;
            
            NSUInteger size;
            if (__builtin_mul_overflow(job.height, job.bytesPerRow, &size)) {
                status("Image dimensions too large");
                return;
            }
            job.buffer = [NSMutableData dataWithLength:size];
            job.headerParsed = true;
        }
        if (band.dataBuffer && band.dataSize > 0 && job.buffer) {
            NSUInteger offset;
            if (__builtin_mul_overflow(band.dataStartRow, job.bytesPerRow, &offset)) {
                status("Band offset too large");
                return;
            }
            NSUInteger available = job.buffer.length > offset ? job.buffer.length - offset : 0;
            NSUInteger amount = std::min((NSUInteger)band.dataSize, available);
            if (amount > 0)
                memcpy((uint8_t*)job.buffer.mutableBytes + offset, band.dataBuffer.bytes, amount);
        }
        if (band.dataStartRow + band.dataNumRows >= job.height && job.buffer) {
            CGImageRef cg = buildImage();
            job.buffer = nil;
            job.headerParsed = false;
            if (cg) {
                QImage img = qImageFromCGImage(cg, job.options.dpi);
                CGImageRelease(cg);
                if (img.isNull()) {
                    status("Failed to convert scanned page");
                } else {
                    if (job.options.colorMode == ColorMode::BlackWhite)
                        img = thresholdToMono(img);
                    ++job.pagesReceived;
                    if (pageCb) pageCb(img);
                }
            }
        }
    }

    CGImageRef buildImage() {
        if (job.width == 0 || job.height == 0 || job.bytesPerRow == 0 || !job.buffer)
            return nil;
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)job.buffer);
        if (!provider) return nil;

        CGImageRef image = nil;
        if (job.pixelType == ICScannerPixelDataTypeRGB) {
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
            CGBitmapInfo info = kCGImageAlphaNone;
            if (job.numComponents == 4) info = kCGImageAlphaNoneSkipLast;
            if (job.bitsPerComponent == 16)
                info |= job.bigEndian ? kCGBitmapByteOrder16Big : kCGBitmapByteOrder16Little;
            image = CGImageCreate(job.width, job.height, job.bitsPerComponent, job.bitsPerPixel,
                                  job.bytesPerRow, cs, info, provider, NULL, false,
                                  kCGRenderingIntentDefault);
            CGColorSpaceRelease(cs);
        } else if (job.pixelType == ICScannerPixelDataTypeBW && job.bitsPerPixel == 1) {
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
            image = CGImageCreate(job.width, job.height, 1, 1, job.bytesPerRow, cs,
                                  kCGImageAlphaNone, provider, NULL, false,
                                  kCGRenderingIntentDefault);
            CGColorSpaceRelease(cs);
        } else { // Gray and anything else we treat as gray
            CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
            CGBitmapInfo info = kCGImageAlphaNone;
            if (job.bitsPerComponent == 16)
                info |= job.bigEndian ? kCGBitmapByteOrder16Big : kCGBitmapByteOrder16Little;
            image = CGImageCreate(job.width, job.height, job.bitsPerComponent, job.bitsPerPixel,
                                  job.bytesPerRow, cs, info, provider, NULL, false,
                                  kCGRenderingIntentDefault);
            CGColorSpaceRelease(cs);
        }
        CGDataProviderRelease(provider);
        return image;
    }

    void finishScan(ICScannerDevice* scanner, NSError* error) {
        bool cancelled = job.cancelRequested;
        int pages = job.pagesReceived;
        job.scanning = job.pendingUnitSelection = false;
        job.cancelRequested = false;
        job.buffer = nil;
        job.headerParsed = false;
        if (finishedCb) {
            if (error && !cancelled) {
                NSString* desc = error.localizedDescription;
                finishedCb(false, desc ? desc.UTF8String : "scan failed");
            } else if (cancelled) {
                finishedCb(false, "cancelled");
            } else if (pages == 0) {
                finishedCb(false, "no pages received - is paper loaded?");
            } else {
                finishedCb(true, "");
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Delegate: one object serves as browser + device + scanner delegate
// ---------------------------------------------------------------------------
@interface SUScannerDelegate : NSObject <ICDeviceBrowserDelegate, ICDeviceDelegate, ICScannerDeviceDelegate>
@property(nonatomic, assign) ScannerBackend::Impl* impl;
@end

@implementation SUScannerDelegate

// ICDeviceBrowser reports units only after the session settles; on some
// AirScan devices no explicit "available" callback fires, so poll briefly.
- (void)armReadinessPoll:(ICScannerDevice*)scanner {
    __weak ICScannerDevice* weakScanner = scanner;
    __weak typeof(self) weakSelf = self;
    for (int i = 1; i <= 30; ++i) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(i * 300 * NSEC_PER_MSEC)),
                       dispatch_get_main_queue(), ^{
            typeof(self) strongSelf = weakSelf;
            if (!strongSelf) return;
            ICScannerDevice* s = weakScanner;
            if (!s || !strongSelf.impl) return;
            if (!ScannerBackend::Impl::isReady(s)) return;
            strongSelf.impl->emitDevices();
            if (!strongSelf.impl->job.uuid.empty() && strongSelf.impl->job.waitingForUnits) {
                std::string id = s.UUIDString ? s.UUIDString.UTF8String : "";
                if (id == strongSelf.impl->job.uuid)
                    strongSelf.impl->startScan(s);
            }
        });
    }
}

- (void)deviceBrowser:(ICDeviceBrowser*)browser didAddDevice:(ICDevice*)device moreComing:(BOOL)moreComing {
    (void)browser; (void)moreComing;
    if (![device isKindOfClass:[ICScannerDevice class]]) return;
    ICScannerDevice* scanner = (ICScannerDevice*)device;
    scanner.delegate = self;
    if (![self.impl->devices containsObject:scanner])
        [self.impl->devices addObject:scanner];
    self.impl->emitDevices();
    if (!scanner.hasOpenSession)
        [scanner requestOpenSession];
}

- (void)deviceBrowser:(ICDeviceBrowser*)browser didRemoveDevice:(ICDevice*)device moreGoing:(BOOL)moreGoing {
    (void)browser; (void)moreGoing;
    if (![device isKindOfClass:[ICScannerDevice class]]) return;
    [self.impl->devices removeObject:(ICScannerDevice*)device];
    self.impl->emitDevices();
}

- (void)didRemoveDevice:(ICDevice*)device {
    (void)device;
}

- (void)device:(ICDevice*)device didOpenSessionWithError:(NSError*)error {
    if (error) {
        NSString* d = error.localizedDescription;
        self.impl->status(std::string("session error: ") + (d ? d.UTF8String : ""));
        return;
    }
    if ([device isKindOfClass:[ICScannerDevice class]])
        [self armReadinessPoll:(ICScannerDevice*)device];
    self.impl->emitDevices();
}

- (void)device:(ICDevice*)device didCloseSessionWithError:(NSError*)error {
    (void)device; (void)error;
    self.impl->emitDevices();
}

- (void)scannerDeviceDidBecomeAvailable:(ICScannerDevice*)scanner {
    self.impl->emitDevices();
    if (!self.impl->job.uuid.empty() && self.impl->job.waitingForUnits) {
        std::string id = scanner.UUIDString ? scanner.UUIDString.UTF8String : "";
        if (id == self.impl->job.uuid)
            self.impl->startScan(scanner);
    }
}

- (void)scannerDevice:(ICScannerDevice*)scanner didSelectFunctionalUnit:(ICScannerFunctionalUnit*)functionalUnit error:(NSError*)error {
    (void)functionalUnit;
    if (error) {
        NSString* d = error.localizedDescription;
        self.impl->status(std::string("unit selection error: ") + (d ? d.UTF8String : ""));
        if (self.impl->job.pendingUnitSelection) {
            self.impl->job.pendingUnitSelection = false;
            if (self.impl->finishedCb)
                self.impl->finishedCb(false, "could not select scanner unit");
        }
        return;
    }
    self.impl->emitDevices();
    if (self.impl->job.pendingUnitSelection)
        self.impl->configureAndScan(scanner);
}

- (void)scannerDevice:(ICScannerDevice*)scanner didScanToBandData:(ICScannerBandData*)data {
    (void)scanner;
    self.impl->handleBand(data);
}

- (void)scannerDevice:(ICScannerDevice*)scanner didCompleteScanWithError:(NSError*)error {
    self.impl->finishScan(scanner, error);
}

@end

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------
ScannerBackend::ScannerBackend() {
    d = std::make_unique<Impl>();
    d->delegate = [SUScannerDelegate new];
    d->delegate.impl = d.get();
    d->devices = [NSMutableArray new];
}

ScannerBackend::~ScannerBackend() {
    stopBrowsing();
    d->delegate.impl = nullptr;
    d->delegate = nil;
    d->devices = nil;
}

void ScannerBackend::setDeviceListCallback(std::function<void(const std::vector<ScannerInfo>&)> cb) { d->deviceListCb = std::move(cb); }
void ScannerBackend::setPageCallback(std::function<void(const QImage&)> cb) { d->pageCb = std::move(cb); }
void ScannerBackend::setScanFinishedCallback(std::function<void(bool, const std::string&)> cb) { d->finishedCb = std::move(cb); }
void ScannerBackend::setStatusCallback(std::function<void(const std::string&)> cb) { d->statusCb = std::move(cb); }

void ScannerBackend::startBrowsing() {
    if (d->browser) return;
    d->browser = [ICDeviceBrowser new];
    d->browser.delegate = d->delegate;
    d->browser.browsedDeviceTypeMask = (ICDeviceTypeMask)(ICDeviceTypeMaskScanner | ICDeviceLocationTypeMaskLocal
        | ICDeviceLocationTypeMaskShared | ICDeviceLocationTypeMaskBonjour);
    [d->browser start];
    d->status("Looking for scanners...");
}

void ScannerBackend::stopBrowsing() {
    if (!d->browser) return;
    for (ICScannerDevice* s in d->devices) {
        if (s.hasOpenSession) [s requestCloseSession];
    }
    [d->browser stop];
    d->browser.delegate = nil;
    d->browser = nil;
}

void ScannerBackend::scan(const std::string& deviceUuid, const ScanOptions& options) {
    ICScannerDevice* scanner = d->findScanner(deviceUuid);
    if (!scanner) {
        if (d->finishedCb) d->finishedCb(false, "scanner not found");
        return;
    }
    d->job.uuid = deviceUuid;
    d->job.options = options;
    d->job.cancelRequested = false;
    d->job.pagesReceived = 0;
    if (Impl::isReady(scanner)) {
        d->startScan(scanner);
    } else {
        d->job.waitingForUnits = true;
        d->status("Waiting for scanner capabilities...");
        [scanner requestOpenSession]; // in case the session dropped
    }
}

void ScannerBackend::cancelScan() {
    d->job.cancelRequested = true;
    ICScannerDevice* scanner = d->findScanner(d->job.uuid);
    if (scanner) [scanner cancelScan];
}
