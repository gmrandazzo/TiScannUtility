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
 */
#import <Foundation/Foundation.h>
#import <ImageCaptureCore/ImageCaptureCore.h>
#import <ImageIO/ImageIO.h>

@interface Del : NSObject <ICDeviceBrowserDelegate, ICDeviceDelegate, ICScannerDeviceDelegate>
@property (nonatomic, strong) ICScannerDevice* scanner;
@property (nonatomic, strong) NSMutableData* buffer;
@property int pages;
@end

static void writePNG(CGImageRef img, NSString* path) {
    CFURLRef url = (__bridge CFURLRef)[NSURL fileURLWithPath:path];
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dest, img, NULL);
    CGImageDestinationFinalize(dest);
    CFRelease(dest);
}

@implementation Del

- (void)deviceBrowser:(ICDeviceBrowser*)b didAddDevice:(ICDevice*)d moreComing:(BOOL)m {
    (void)b; (void)m;
    if (![d isKindOfClass:[ICScannerDevice class]]) return;
    self.scanner = (ICScannerDevice*)d;
    self.scanner.delegate = self;
    NSLog(@"ADD '%@', opening session", d.name);
    [self.scanner requestOpenSession];
}

- (void)deviceBrowser:(ICDeviceBrowser*)b didRemoveDevice:(ICDevice*)d moreGoing:(BOOL)m { (void)b; (void)d; (void)m; }
- (void)didRemoveDevice:(ICDevice*)device { (void)device; }

- (void)device:(ICDevice*)device didOpenSessionWithError:(NSError*)error {
    NSLog(@"SESSION error=%@", error);
    __weak Del* weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        Del* s = weakSelf;
        if (!s) return;
        NSLog(@"units=%@ -> scanning FLATBED at 150dpi RGB", s.scanner.availableFunctionalUnitTypes);
        ICScannerFunctionalUnit* unit = s.scanner.selectedFunctionalUnit;
        unit.resolution = 150;
        unit.pixelDataType = ICScannerPixelDataTypeRGB;
        if ([unit.supportedBitDepths containsIndex:8]) unit.bitDepth = ICScannerBitDepth8Bits;
        s.scanner.transferMode = ICScannerTransferModeMemoryBased;
        s.buffer = [NSMutableData dataWithLength:unit.scanArea.size.height * 0]; // placeholder
        s.buffer = nil;
        [s.scanner requestScan];
    });
}

- (void)device:(ICDevice*)device didCloseSessionWithError:(NSError*)error { (void)device; (void)error; }

- (void)scannerDevice:(ICScannerDevice*)scanner didScanToBandData:(ICScannerBandData*)band {
    (void)scanner;
    if (!self.buffer)
        self.buffer = [NSMutableData dataWithLength:band.fullImageHeight * band.bytesPerRow];
    NSUInteger offset = band.dataStartRow * band.bytesPerRow;
    NSUInteger amount = MIN((NSUInteger)band.dataSize, self.buffer.length - offset);
    memcpy((uint8_t*)self.buffer.mutableBytes + offset, band.dataBuffer.bytes, amount);
    NSLog(@"band: rows %lu..%lu of %lu, bpp=%lu comp=%lu data=%lub",
          (unsigned long)band.dataStartRow, (unsigned long)(band.dataStartRow + band.dataNumRows),
          (unsigned long)band.fullImageHeight, (unsigned long)band.bitsPerPixel,
          (unsigned long)band.numComponents, (unsigned long)band.dataSize);

    if (band.dataStartRow + band.dataNumRows >= band.fullImageHeight) {
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)self.buffer);
        CGImageRef img = CGImageCreate(band.fullImageWidth, band.fullImageHeight, 8, band.bitsPerPixel,
                                       band.bytesPerRow, CGColorSpaceCreateDeviceRGB(),
                                       band.numComponents == 4 ? kCGImageAlphaNoneSkipLast : kCGImageAlphaNone,
                                       provider, NULL, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        NSString* path = [NSString stringWithFormat:@"/tmp/probe_scan_page%d.png", ++self.pages];
        writePNG(img, path);
        NSLog(@"PAGE %d saved: %@ (%lux%lu)", self.pages, path,
              (unsigned long)band.fullImageWidth, (unsigned long)band.fullImageHeight);
        CGImageRelease(img);
        self.buffer = nil;
    }
}

- (void)scannerDevice:(ICScannerDevice*)scanner didCompleteScanWithError:(NSError*)error {
    NSLog(@"SCAN COMPLETE error=%@ pages=%d", error, self.pages);
    [scanner cancelScan];
    exit(error ? 1 : 0);
}

@end

int main(void) {
    @autoreleasepool {
        Del* del = [Del new];
        ICDeviceBrowser* browser = [ICDeviceBrowser new];
        browser.delegate = del;
        browser.browsedDeviceTypeMask = ICDeviceTypeMaskScanner | ICDeviceLocationTypeMaskLocal
            | ICDeviceLocationTypeMaskShared | ICDeviceLocationTypeMaskBonjour;
        [browser start];
        NSLog(@"waiting 30s for scan...");
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:30]];
        NSLog(@"TIMEOUT");
    }
    return 1;
}
