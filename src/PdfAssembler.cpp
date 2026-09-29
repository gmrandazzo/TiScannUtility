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

#if defined(__APPLE__)

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

#else // Cross-platform (Linux / Windows)

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPdfWriter>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <cmath>
#include <map>
#include <memory>
#include <vector>

#if __has_include(<qpdf/QPDF.hh>)
#define HAVE_QPDF_CPP 1
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>
#endif

#if __has_include(<poppler-qt6.h>)
#define HAVE_POPPLER_CPP 1
#include <poppler-qt6.h>
#elif __has_include(<poppler/qt6/poppler-qt6.h>)
#define HAVE_POPPLER_CPP 1
#include <poppler/qt6/poppler-qt6.h>
#elif __has_include(<poppler-qt5.h>)
#define HAVE_POPPLER_CPP 1
#include <poppler-qt5.h>
#elif __has_include(<poppler/qt5/poppler-qt5.h>)
#define HAVE_POPPLER_CPP 1
#include <poppler/qt5/poppler-qt5.h>
#endif

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

struct PageSource {
    bool isPdf = false;
    QString path;
    int pageIndex = 0;
    int rotation = 0;
};

} // namespace

struct PdfAssembler::Impl {
    std::string targetPath;
    std::vector<PageSource> pages;
    std::vector<QString> tempFiles;
    std::unique_ptr<QTemporaryDir> tempDir;

    void cleanup() {
        for (const QString& f : tempFiles) {
            QFile::remove(f);
        }
        tempFiles.clear();
        pages.clear();
        tempDir.reset();
        targetPath.clear();
    }
};

PdfAssembler::PdfAssembler() : d(new Impl) {}

PdfAssembler::~PdfAssembler() {
    d->cleanup();
    delete d;
}

bool PdfAssembler::begin(const std::string& path, std::string* error) {
    d->cleanup();
    if (path.empty()) {
        if (error) *error = "Invalid output path";
        return false;
    }
    d->targetPath = path;
    d->tempDir = std::make_unique<QTemporaryDir>();
    return true;
}

bool PdfAssembler::appendImage(const QImage& image, int rotationDeg) {
    if (d->targetPath.empty() || image.isNull()) return false;

    int rotation = normalizeRotation(rotationDeg);
    int dpi = dpiOf(image);

    int w = (rotation == 90 || rotation == 270) ? image.height() : image.width();
    int h = (rotation == 90 || rotation == 270) ? image.width() : image.height();
    qreal ptW = w * 72.0 / dpi;
    qreal ptH = h * 72.0 / dpi;

    QString tmpPath = d->tempDir->filePath(QString("img_page_%1.pdf").arg(d->tempFiles.size() + 1));

    QPdfWriter writer(tmpPath);
    writer.setResolution(dpi);
    writer.setPageSize(QPageSize(QSizeF(ptW, ptH), QPageSize::Point));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));

    QPainter painter(&writer);
    qreal origPtW = image.width() * 72.0 / dpi;
    qreal origPtH = image.height() * 72.0 / dpi;

    if (rotation == 90) {
        painter.translate(ptW, 0);
        painter.rotate(90);
    } else if (rotation == 180) {
        painter.translate(ptW, ptH);
        painter.rotate(180);
    } else if (rotation == 270) {
        painter.translate(0, ptH);
        painter.rotate(270);
    }

    painter.drawImage(QRectF(0, 0, origPtW, origPtH), image);
    painter.end();

    d->tempFiles.push_back(tmpPath);
    d->pages.push_back({false, tmpPath, 0, 0});
    return true;
}

bool PdfAssembler::appendPdfPage(const std::string& pdfPath, int pageIndex, int rotationDeg) {
    if (d->targetPath.empty()) return false;
    if (pageIndex < 0) return false;
    d->pages.push_back({true, QString::fromStdString(pdfPath), pageIndex, normalizeRotation(rotationDeg)});
    return true;
}

bool PdfAssembler::end(std::string* error) {
    if (d->targetPath.empty()) {
        if (error) *error = "PDF not started";
        return false;
    }
    if (d->pages.empty()) {
        d->cleanup();
        return true;
    }

#if defined(HAVE_QPDF_CPP)
    try {
        QPDF outPdf;
        outPdf.emptyPDF();
        QPDFPageDocumentHelper outHelper(outPdf);

        std::map<std::string, std::shared_ptr<QPDF>> openDocs;
        for (const auto& pageSrc : d->pages) {
            std::string srcPath = pageSrc.path.toStdString();
            auto it = openDocs.find(srcPath);
            if (it == openDocs.end()) {
                auto doc = std::make_shared<QPDF>();
                doc->processFile(srcPath.c_str());
                openDocs[srcPath] = doc;
                it = openDocs.find(srcPath);
            }
            std::vector<QPDFPageObjectHelper> srcPages = QPDFPageDocumentHelper(*it->second).getAllPages();
            if (pageSrc.pageIndex >= 0 && pageSrc.pageIndex < int(srcPages.size())) {
                QPDFPageObjectHelper page = srcPages[pageSrc.pageIndex];
                if (pageSrc.rotation != 0) {
                    page.rotateBy(pageSrc.rotation, false);
                }
                outHelper.addPage(page, false);
            }
        }
        QPDFWriter writer(outPdf, d->targetPath.c_str());
        writer.write();
        d->cleanup();
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string("QPDF assembly error: ") + e.what();
        d->cleanup();
        return false;
    }
#else
    QString qpdfExe = QStandardPaths::findExecutable("qpdf");
    QString pdfuniteExe = QStandardPaths::findExecutable("pdfunite");
    QString pdfseparateExe = QStandardPaths::findExecutable("pdfseparate");

    if (!qpdfExe.isEmpty()) {
        QStringList args;
        args << "--empty" << "--pages";
        for (const auto& pageSrc : d->pages) {
            args << pageSrc.path;
            QString range = QString::number(pageSrc.pageIndex + 1);
            if (pageSrc.rotation != 0) {
                range += QString(":%1").arg(pageSrc.rotation);
            }
            args << range;
        }
        args << "--" << QString::fromStdString(d->targetPath);
        QProcess proc;
        proc.start(qpdfExe, args);
        if (proc.waitForFinished(60000) && proc.exitCode() == 0) {
            d->cleanup();
            return true;
        }
    }

    if (!pdfuniteExe.isEmpty()) {
        QStringList pageFiles;
        int extractedIdx = 0;
        bool allGood = true;

        for (const auto& pageSrc : d->pages) {
            int total = pdfPageCount(pageSrc.path.toStdString());
            if (total == 1 && pageSrc.rotation == 0) {
                pageFiles << pageSrc.path;
            } else if (!pdfseparateExe.isEmpty() && pageSrc.rotation == 0) {
                QString singlePagePattern = d->tempDir->filePath(QString("part_%1_%d.pdf").arg(++extractedIdx));
                QString singlePageFile = d->tempDir->filePath(QString("part_%1_%2.pdf").arg(extractedIdx).arg(pageSrc.pageIndex + 1));
                QProcess sep;
                sep.start(pdfseparateExe, QStringList() << "-f" << QString::number(pageSrc.pageIndex + 1)
                                                       << "-l" << QString::number(pageSrc.pageIndex + 1)
                                                       << pageSrc.path << singlePagePattern);
                if (sep.waitForFinished(10000) && QFile::exists(singlePageFile)) {
                    d->tempFiles.push_back(singlePageFile);
                    pageFiles << singlePageFile;
                } else {
                    allGood = false;
                    break;
                }
            } else {
                QImage rendered = renderPdfPage(pageSrc.path.toStdString(), pageSrc.pageIndex, 2400);
                if (!rendered.isNull()) {
                    QString imgPdf = d->tempDir->filePath(QString("part_rot_%1.pdf").arg(++extractedIdx));
                    QPdfWriter pw(imgPdf);
                    pw.setResolution(300);
                    int rot = pageSrc.rotation;
                    int w = (rot == 90 || rot == 270) ? rendered.height() : rendered.width();
                    int h = (rot == 90 || rot == 270) ? rendered.width() : rendered.height();
                    qreal ptW = w * 72.0 / 300.0;
                    qreal ptH = h * 72.0 / 300.0;
                    pw.setPageSize(QPageSize(QSizeF(ptW, ptH), QPageSize::Point));
                    pw.setPageMargins(QMarginsF(0, 0, 0, 0));
                    QPainter p(&pw);
                    if (rot == 90) { p.translate(ptW, 0); p.rotate(90); }
                    else if (rot == 180) { p.translate(ptW, ptH); p.rotate(180); }
                    else if (rot == 270) { p.translate(0, ptH); p.rotate(270); }
                    p.drawImage(QRectF(0, 0, rendered.width() * 72.0 / 300.0, rendered.height() * 72.0 / 300.0), rendered);
                    p.end();
                    d->tempFiles.push_back(imgPdf);
                    pageFiles << imgPdf;
                } else {
                    allGood = false;
                    break;
                }
            }
        }

        if (allGood && !pageFiles.isEmpty()) {
            QStringList uniteArgs = pageFiles;
            uniteArgs << QString::fromStdString(d->targetPath);
            QProcess unite;
            unite.start(pdfuniteExe, uniteArgs);
            if (unite.waitForFinished(60000) && unite.exitCode() == 0) {
                d->cleanup();
                return true;
            }
        }
    }

    // Pure Qt fallback: QPdfWriter rendering each page
    QPdfWriter writer(QString::fromStdString(d->targetPath));
    writer.setResolution(300);
    QPainter painter;
    bool first = true;
    for (const auto& pageSrc : d->pages) {
        QImage pageImg = renderPdfPage(pageSrc.path.toStdString(), pageSrc.pageIndex, 2400);
        if (pageImg.isNull()) continue;
        int rot = pageSrc.rotation;
        int w = (rot == 90 || rot == 270) ? pageImg.height() : pageImg.width();
        int h = (rot == 90 || rot == 270) ? pageImg.width() : pageImg.height();
        qreal ptW = w * 72.0 / 300.0;
        qreal ptH = h * 72.0 / 300.0;
        if (!first) {
            writer.newPage();
        } else {
            painter.begin(&writer);
            first = false;
        }
        writer.setPageSize(QPageSize(QSizeF(ptW, ptH), QPageSize::Point));
        writer.setPageMargins(QMarginsF(0, 0, 0, 0));
        painter.save();
        if (rot == 90) { painter.translate(ptW, 0); painter.rotate(90); }
        else if (rot == 180) { painter.translate(ptW, ptH); painter.rotate(180); }
        else if (rot == 270) { painter.translate(0, ptH); painter.rotate(270); }
        painter.drawImage(QRectF(0, 0, pageImg.width() * 72.0 / 300.0, pageImg.height() * 72.0 / 300.0), pageImg);
        painter.restore();
    }
    if (!first) painter.end();
    d->cleanup();
    return true;
#endif
}

int PdfAssembler::pdfPageCount(const std::string& pdfPath) {
#if defined(HAVE_POPPLER_CPP)
    auto doc = Poppler::Document::load(QString::fromStdString(pdfPath));
    if (doc) return doc->numPages();
#endif

    QString pdfinfoExe = QStandardPaths::findExecutable("pdfinfo");
    if (!pdfinfoExe.isEmpty()) {
        QProcess proc;
        proc.start(pdfinfoExe, QStringList() << QString::fromStdString(pdfPath));
        if (proc.waitForFinished(5000)) {
            QString out = QString::fromLocal8Bit(proc.readAllStandardOutput());
            QRegularExpression re("Pages:\\s+(\\d+)");
            auto match = re.match(out);
            if (match.hasMatch()) {
                return match.captured(1).toInt();
            }
        }
    }

    QFile f(QString::fromStdString(pdfPath));
    if (f.open(QIODevice::ReadOnly)) {
        QByteArray data = f.read(512 * 1024);
        QRegularExpression re("/Count\\s+(\\d+)");
        auto match = re.match(QString::fromLatin1(data));
        if (match.hasMatch()) {
            return match.captured(1).toInt();
        }
    }
    return 0;
}

QImage PdfAssembler::renderPdfPage(const std::string& pdfPath, int pageIndex, int maxDim) {
#if defined(HAVE_POPPLER_CPP)
    auto doc = Poppler::Document::load(QString::fromStdString(pdfPath));
    if (doc && pageIndex >= 0 && pageIndex < doc->numPages()) {
        auto page = doc->page(pageIndex);
        if (page) {
            QSizeF sz = page->pageSizeF();
            double scale = 1.0;
            if (sz.width() > sz.height()) scale = maxDim / sz.width();
            else scale = maxDim / sz.height();
            if (scale <= 0) scale = 1.0;
            double dpi = 72.0 * scale;
            return page->renderToImage(dpi, dpi);
        }
    }
#endif

    QString pdftoppmExe = QStandardPaths::findExecutable("pdftoppm");
    if (!pdftoppmExe.isEmpty()) {
        QTemporaryDir tmpDir;
        if (tmpDir.isValid()) {
            QString prefix = tmpDir.filePath("render");
            QProcess proc;
            QStringList args;
            args << "-png" << "-f" << QString::number(pageIndex + 1)
                 << "-l" << QString::number(pageIndex + 1)
                 << "-scale-to" << QString::number(maxDim)
                 << QString::fromStdString(pdfPath) << prefix;
            proc.start(pdftoppmExe, args);
            if (proc.waitForFinished(10000)) {
                QDir dir(tmpDir.path());
                QStringList entries = dir.entryList(QStringList() << "render*.png", QDir::Files);
                if (!entries.isEmpty()) {
                    return QImage(dir.filePath(entries.first()));
                }
            }
        }
    }
    return QImage();
}

#endif // !__APPLE__
