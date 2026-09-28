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

@interface Del : NSObject <ICDeviceBrowserDelegate, ICDeviceDelegate, ICScannerDeviceDelegate>
@property (nonatomic, strong) ICScannerDevice* scanner;
@end

@implementation Del

- (void)deviceBrowser:(ICDeviceBrowser*)b didAddDevice:(ICDevice*)d moreComing:(BOOL)m {
    (void)b; (void)m;
    NSLog(@"ADD: '%@' class=%@", d.name, NSStringFromClass([d class]));
    if ([d isKindOfClass:[ICScannerDevice class]]) {
        self.scanner = (ICScannerDevice*)d;
        self.scanner.delegate = self;
        [self.scanner requestOpenSession];
    }
}

- (void)deviceBrowser:(ICDeviceBrowser*)b didRemoveDevice:(ICDevice*)d moreGoing:(BOOL)m { (void)b; (void)d; (void)m; }
- (void)didRemoveDevice:(ICDevice*)device { (void)device; }

- (void)device:(ICDevice*)device didOpenSessionWithError:(NSError*)error {
    NSLog(@"SESSION: error=%@", error);
    __weak Del* weakSelf = self;
    for (int i = 1; i <= 8; ++i) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(i * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            Del* s = weakSelf;
            if (!s) return;
            NSLog(@"t=%ds units=%@ selected=%@", i, s.scanner.availableFunctionalUnitTypes,
                  s.scanner.selectedFunctionalUnit ? @(s.scanner.selectedFunctionalUnit.type) : @(-1));
            if (i == 4) {
                NSLog(@"--> requesting DocumentFeeder");
                [s.scanner requestSelectFunctionalUnit:ICScannerFunctionalUnitTypeDocumentFeeder];
            }
        });
    }
}

- (void)device:(ICDevice*)device didCloseSessionWithError:(NSError*)error { (void)device; (void)error; }

- (void)scannerDeviceDidBecomeAvailable:(ICScannerDevice*)scanner {
    NSLog(@"DID-BECOME-AVAILABLE units=%@", scanner.availableFunctionalUnitTypes);
}

- (void)scannerDevice:(ICScannerDevice*)scanner didSelectFunctionalUnit:(ICScannerFunctionalUnit*)unit error:(NSError*)error {
    NSLog(@"SELECTED unit type=%lu error=%@", (unsigned long)unit.type, error);
    if (unit.type == ICScannerFunctionalUnitTypeDocumentFeeder) {
        ICScannerFunctionalUnitDocumentFeeder* adf = (ICScannerFunctionalUnitDocumentFeeder*)unit;
        NSLog(@"FEEDER: duplex=%d loaded=%d resolutions=%@", (int)adf.supportsDuplexScanning,
              (int)adf.documentLoaded, unit.supportedResolutions);
    }
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
        NSLog(@"browsing 15s...");
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:15]];
    }
    return 0;
}
