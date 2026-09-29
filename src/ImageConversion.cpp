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
#include "ImageConversion.h"

#include <CoreGraphics/CGBitmapContext.h>
#include <CoreGraphics/CGDataProvider.h>
#include <CoreGraphics/CGColorSpace.h>

#if defined(__APPLE__)

QImage qImageFromCGImage(CGImageRef image, int dpi) {
    if (!image) return QImage();
    size_t w = CGImageGetWidth(image);
    size_t h = CGImageGetHeight(image);
    if (w == 0 || h == 0) return QImage();

    QImage out(int(w), int(h), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // QImage ARGB32_Premultiplied on little-endian is B,G,R,A in memory,
    // which matches kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little.
    CGContextRef ctx = CGBitmapContextCreate(out.bits(), w, h, 8, size_t(out.bytesPerLine()),
                                             cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    if (ctx) {
        CGContextDrawImage(ctx, CGRectMake(0, 0, CGFloat(w), CGFloat(h)), image);
        CGContextRelease(ctx);
    }
    CGColorSpaceRelease(cs);

    if (dpi > 0) {
        int dpm = int(dpi / 0.0254 + 0.5);
        out.setDotsPerMeterX(dpm);
        out.setDotsPerMeterY(dpm);
    }
    return out;
}

static void releaseImageData(void *info, const void *data, size_t size) {
    (void)data;
    (void)size;
    delete static_cast<QImage*>(info);
}

CGImageRef cgImageFromQImage(const QImage& source) {
    if (source.isNull()) return nullptr;

    if (source.format() == QImage::Format_Mono) {
        // Keep true 1-bit data: a mask paints black where a bit is set,
        // which matches Qt's Format_Mono convention (bit set = black).
        QImage* heapImg = new QImage(source);
        CGDataProviderRef provider = CGDataProviderCreateWithData(heapImg, heapImg->constBits(), heapImg->sizeInBytes(), releaseImageData);
        if (!provider) { delete heapImg; return nullptr; }
        CGImageRef mask = CGImageMaskCreate(heapImg->width(), heapImg->height(), 1, 1,
                                            size_t(heapImg->bytesPerLine()), provider, nullptr, false);
        CGDataProviderRelease(provider);
        return mask;
    }

    QImage* heapImg = new QImage(source.convertToFormat(QImage::Format_RGBA8888));
    CGDataProviderRef provider = CGDataProviderCreateWithData(heapImg, heapImg->constBits(), heapImg->sizeInBytes(), releaseImageData);
    if (!provider) { delete heapImg; return nullptr; }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef cg = CGImageCreate(heapImg->width(), heapImg->height(), 8, 32, size_t(heapImg->bytesPerLine()),
                                  cs, kCGImageAlphaNoneSkipLast,
                                  provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);
    return cg;
}

#endif // __APPLE__

