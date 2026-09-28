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
#include "PdfAssembler.h"

#include "ImageConversion.h"

#include <CoreGraphics/CGPDFDocument.h>
#include <CoreGraphics/CGContext.h>
#include <CoreGraphics/CGPDFContext.h>
#include <CoreGraphics/CGBitmapContext.h>

#include <cmath>
#include <map>

namespace {

int normalizeRotation(int deg) {
    int r = deg % 360;
    if (r < 0) r += 360;
    return r;
}

int dpiOf(const QImage& img) {
    int dpm = img.dotsPerMeterX();
    if (dpm <= 0) return 150;
    int dpi = int(std::lround(dpm * 0.0254));
    return dpi > 0 ? dpi : 150;
}

CFURLRef makeUrl(const std::string& path) {
    return CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8*>(path.c_str()), CFIndex(path.size()), false);
}

void applyRotation(CGContextRef ctx, int rotation, double w, double h) {
    switch (rotation) {
    case 90:
        CGContextTranslateCTM(ctx, CGFloat(w), 0);
        CGContextRotateCTM(ctx, CGFloat(M_PI_2));
        break;
    case 180:
        CGContextTranslateCTM(ctx, CGFloat(w), CGFloat(h));
        CGContextRotateCTM(ctx, CGFloat(M_PI));
        break;
    case 270:
        CGContextTranslateCTM(ctx, 0, CGFloat(h));
        CGContextRotateCTM(ctx, CGFloat(-M_PI_2));
        break;
    default:
        break;
    }
}

} // namespace

struct PdfAssembler::Impl {
    CGContextRef ctx = nullptr;
    std::map<std::string, CGPDFDocumentRef> openDocs;

    CGPDFDocumentRef docFor(const std::string& path) {
        auto it = openDocs.find(path);
        if (it != openDocs.end()) return it->second;
        CFURLRef url = makeUrl(path);
        if (!url) return nullptr;
        CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(url);
        CFRelease(url);
        if (!doc) return nullptr;
        openDocs[path] = doc;
        return doc;
    }

    bool beginPage(CGRect box) {
        CFDataRef boxData = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(&box), sizeof(box));
        if (!boxData) return false;
        const void* keys[] = { kCGPDFContextMediaBox };
        const void* values[] = { boxData };
        CFDictionaryRef info = CFDictionaryCreate(nullptr, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFRelease(boxData);
        if (!info) return false;
        CGPDFContextBeginPage(ctx, info);
        CFRelease(info);
        return true;
    }
};

PdfAssembler::PdfAssembler() : d(new Impl) {}
PdfAssembler::~PdfAssembler() {
    for (auto& kv : d->openDocs) CGPDFDocumentRelease(kv.second);
    if (d->ctx) CGContextRelease(d->ctx);
    delete d;
}

bool PdfAssembler::begin(const std::string& path, std::string* error) {
    CFURLRef url = makeUrl(path);
    if (!url) {
        if (error) *error = "invalid output path";
        return false;
    }
    d->ctx = CGPDFContextCreateWithURL(url, nullptr, nullptr);
    CFRelease(url);
    if (!d->ctx) {
        if (error) *error = "cannot create PDF at: " + path;
        return false;
    }
    return true;
}

bool PdfAssembler::appendImage(const QImage& image, int rotationDeg) {
    if (!d->ctx || image.isNull()) return false;
    int rotation = normalizeRotation(rotationDeg);
    int dpi = dpiOf(image);

    double wPt = image.width() * 72.0 / dpi;
    double hPt = image.height() * 72.0 / dpi;
    bool swap = (rotation == 90 || rotation == 270);
    CGRect pageBox = CGRectMake(0, 0, CGFloat(swap ? hPt : wPt), CGFloat(swap ? wPt : hPt));
    if (!d->beginPage(pageBox)) return false;

    applyRotation(d->ctx, rotation, wPt, hPt);
    CGImageRef cg = cgImageFromQImage(image);
    if (!cg) {
        CGPDFContextEndPage(d->ctx);
        return false;
    }
    CGContextDrawImage(d->ctx, CGRectMake(0, 0, CGFloat(wPt), CGFloat(hPt)), cg);
    CGImageRelease(cg);
    CGPDFContextEndPage(d->ctx);
    return true;
}

bool PdfAssembler::appendPdfPage(const std::string& pdfPath, int pageIndex, int rotationDeg) {
    if (!d->ctx) return false;
    CGPDFDocumentRef doc = d->docFor(pdfPath);
    if (!doc) return false;
    if (pageIndex < 0 || pageIndex >= CGPDFDocumentGetNumberOfPages(doc)) return false;

    CGPDFPageRef page = CGPDFDocumentGetPage(doc, pageIndex + 1);
    CGRect srcBox = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
    if (CGRectIsEmpty(srcBox)) srcBox = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
    if (CGRectIsEmpty(srcBox)) return false;

    int rotation = normalizeRotation(rotationDeg);
    bool swap = (rotation == 90 || rotation == 270);
    double w = srcBox.size.width, h = srcBox.size.height;
    CGRect pageBox = CGRectMake(0, 0, CGFloat(swap ? h : w), CGFloat(swap ? w : h));
    if (!d->beginPage(pageBox)) return false;

    applyRotation(d->ctx, rotation, w, h);
    CGContextDrawPDFPage(d->ctx, page);
    CGPDFContextEndPage(d->ctx);
    return true;
}

bool PdfAssembler::end(std::string* error) {
    if (!d->ctx) {
        if (error) *error = "PDF not started";
        return false;
    }
    CGContextRelease(d->ctx);
    d->ctx = nullptr;
    for (auto& kv : d->openDocs) CGPDFDocumentRelease(kv.second);
    d->openDocs.clear();
    return true;
}

int PdfAssembler::pdfPageCount(const std::string& pdfPath) {
    CFURLRef url = makeUrl(pdfPath);
    if (!url) return 0;
    CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(url);
    CFRelease(url);
    if (!doc) return 0;
    int n = int(CGPDFDocumentGetNumberOfPages(doc));
    CGPDFDocumentRelease(doc);
    return n;
}

QImage PdfAssembler::renderPdfPage(const std::string& pdfPath, int pageIndex, int maxDim) {
    CFURLRef url = makeUrl(pdfPath);
    if (!url) return QImage();
    CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(url);
    CFRelease(url);
    if (!doc) return QImage();
    if (pageIndex < 0 || pageIndex >= CGPDFDocumentGetNumberOfPages(doc)) {
        CGPDFDocumentRelease(doc);
        return QImage();
    }
    CGPDFPageRef page = CGPDFDocumentGetPage(doc, pageIndex + 1);
    CGRect box = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
    if (CGRectIsEmpty(box)) box = CGPDFPageGetBoxRect(page, kCGPDFCropBox);

    double scale = 1.0;
    if (box.size.width > box.size.height)
        scale = maxDim / box.size.width;
    else
        scale = maxDim / box.size.height;
    if (scale <= 0) scale = 1.0;

    int w = int(std::lround(box.size.width * scale));
    int h = int(std::lround(box.size.height * scale));
    if (w <= 0 || h <= 0) {
        CGPDFDocumentRelease(doc);
        return QImage();
    }

    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::white);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(out.bits(), size_t(w), size_t(h), 8,
                                             size_t(out.bytesPerLine()), cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        CGPDFDocumentRelease(doc);
        return QImage();
    }
    CGContextScaleCTM(ctx, CGFloat(scale), CGFloat(scale));
    CGContextDrawPDFPage(ctx, page);
    CGContextRelease(ctx);
    CGPDFDocumentRelease(doc);
    return out;
}
