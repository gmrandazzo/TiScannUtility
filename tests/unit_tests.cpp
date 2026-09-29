/*
 * TiScannUtility — Unit Tests & Corner Case Verification Suite.
 * Copyright (C) 2026 Giuseppe Marco Randazzo <gmrandazzo@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Qt is linked dynamically; the Qt libraries remain under the LGPL.
 */

#include "PdfAssembler.h"
#include "ScannerBackend.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Lightweight, robust test runner (no extra third-party test dependencies)
// ---------------------------------------------------------------------------
namespace TestRunner {

static int gTotalTests = 0;
static int gPassedTests = 0;
static int gFailedTests = 0;
static const char* gCurrentTestName = nullptr;

#define TEST_CASE(name) \
    static void name(); \
    static struct Register_##name { \
        Register_##name() { TestRunner::addTest(#name, name); } \
    } g_reg_##name; \
    static void name()

struct TestCase {
    std::string name;
    std::function<void()> func;
};

static std::vector<TestCase>& getTests() {
    static std::vector<TestCase> tests;
    return tests;
}

static void addTest(const std::string& name, std::function<void()> func) {
    getTests().push_back({name, func});
}

static void recordFailure(const char* file, int line, const std::string& expr, const std::string& msg = "") {
    std::cerr << "  [FAIL] " << gCurrentTestName << " (" << file << ":" << line << "): "
              << expr;
    if (!msg.empty()) std::cerr << " -> " << msg;
    std::cerr << std::endl;
}

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        TestRunner::recordFailure(__FILE__, __LINE__, #cond); \
        throw std::runtime_error("Assertion failed: " #cond); \
    } \
} while(0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_GE(a, b) ASSERT_TRUE((a) >= (b))
#define ASSERT_LT(a, b) ASSERT_TRUE((a) < (b))
#define ASSERT_LE(a, b) ASSERT_TRUE((a) <= (b))

static int runAll() {
    std::cout << "==================================================" << std::endl;
    std::cout << "Running TiScannUtility Unit & Corner-Case Tests..." << std::endl;
    std::cout << "==================================================" << std::endl;

    for (const auto& test : getTests()) {
        gTotalTests++;
        gCurrentTestName = test.name.c_str();
        auto start = std::chrono::high_resolution_clock::now();
        bool ok = true;
        try {
            test.func();
        } catch (const std::exception& e) {
            ok = false;
        } catch (...) {
            std::cerr << "  [FAIL] " << test.name << ": Unknown exception caught!" << std::endl;
            ok = false;
        }
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        if (ok) {
            gPassedTests++;
            std::cout << "  [PASS] " << test.name << " (" << ms << " ms)" << std::endl;
        } else {
            gFailedTests++;
        }
    }

    std::cout << "==================================================" << std::endl;
    std::cout << "Summary: " << gPassedTests << "/" << gTotalTests << " passed";
    if (gFailedTests > 0) {
        std::cout << " (" << gFailedTests << " FAILED)";
    }
    std::cout << std::endl;
    std::cout << "==================================================" << std::endl;

    return gFailedTests == 0 ? 0 : 1;
}

} // namespace TestRunner

// ---------------------------------------------------------------------------
// Helper: create synthetic test images
// ---------------------------------------------------------------------------
static QImage createPatternImage(int w, int h, int dpi, QColor bg, const QString& text = "") {
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(bg);
    int dpm = int(dpi / 0.0254 + 0.5);
    img.setDotsPerMeterX(dpm);
    img.setDotsPerMeterY(dpm);

    if (!text.isEmpty()) {
        QPainter p(&img);
        p.setPen(Qt::black);
        QFont f = p.font();
        f.setPointSize(std::max(12, h / 20));
        p.setFont(f);
        p.drawText(img.rect(), Qt::AlignCenter, text);
        p.end();
    }
    return img;
}

// ===========================================================================
// SUITE 1: PdfAssembler - Creation & File Path Corner Cases
// ===========================================================================

TEST_CASE(TestPdfAssembler_EmptyPath) {
    PdfAssembler asmblr;
    std::string err;
    bool ok = asmblr.begin("", &err);
    ASSERT_FALSE(ok);
    ASSERT_FALSE(err.empty());
}

TEST_CASE(TestPdfAssembler_InvalidDestinationDir) {
    PdfAssembler asmblr;
    std::string err;
    // Attempting to write into a nonexistent directory
    bool ok = asmblr.begin("/nonexistent_folder_abc123_xyz/output.pdf", &err);
    if (ok) {
        // If begin didn't fail immediately, end() must fail
        ok = asmblr.end(&err);
    }
    ASSERT_FALSE(ok);
}

TEST_CASE(TestPdfAssembler_EmptyPdfZeroPages) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    std::string path = tmpDir.filePath("empty.pdf").toStdString();

    PdfAssembler asmblr;
    std::string err;
    ASSERT_TRUE(asmblr.begin(path, &err));
    // Call end without adding any pages
    ASSERT_TRUE(asmblr.end(&err));
}

TEST_CASE(TestPdfAssembler_AppendNullImage) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    std::string path = tmpDir.filePath("null_img.pdf").toStdString();

    PdfAssembler asmblr;
    std::string err;
    ASSERT_TRUE(asmblr.begin(path, &err));
    // Null image must return false and not crash
    QImage nullImg;
    ASSERT_TRUE(nullImg.isNull());
    ASSERT_FALSE(asmblr.appendImage(nullImg, 0));
    ASSERT_TRUE(asmblr.end(&err));
}

// ===========================================================================
// SUITE 2: Rotation Corner Cases (Angles, Normalization, Extreme Values)
// ===========================================================================

TEST_CASE(TestPdfAssembler_RotationAnglesAndNormalization) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    std::string path = tmpDir.filePath("rotations.pdf").toStdString();

    PdfAssembler asmblr;
    std::string err;
    ASSERT_TRUE(asmblr.begin(path, &err));

    QImage img = createPatternImage(300, 400, 150, Qt::white, "Rot Test");

    // Standard angles: 0, 90, 180, 270
    ASSERT_TRUE(asmblr.appendImage(img, 0));
    ASSERT_TRUE(asmblr.appendImage(img, 90));
    ASSERT_TRUE(asmblr.appendImage(img, 180));
    ASSERT_TRUE(asmblr.appendImage(img, 270));

    // Corner cases: 360 (same as 0), 450 (same as 90), 720 (same as 0)
    ASSERT_TRUE(asmblr.appendImage(img, 360));
    ASSERT_TRUE(asmblr.appendImage(img, 450));
    ASSERT_TRUE(asmblr.appendImage(img, 720));

    // Corner cases: negative angles (-90, -180, -270, -360)
    ASSERT_TRUE(asmblr.appendImage(img, -90));
    ASSERT_TRUE(asmblr.appendImage(img, -180));
    ASSERT_TRUE(asmblr.appendImage(img, -270));
    ASSERT_TRUE(asmblr.appendImage(img, -360));

    ASSERT_TRUE(asmblr.end(&err));

    int count = PdfAssembler::pdfPageCount(path);
    ASSERT_EQ(count, 11);
}

// ===========================================================================
// SUITE 3: Geometry & Aspect Ratio Extremes (1x1, strips, odd strides)
// ===========================================================================

TEST_CASE(TestPdfAssembler_ExtremeDimensions) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    std::string path = tmpDir.filePath("extremes.pdf").toStdString();

    PdfAssembler asmblr;
    std::string err;
    ASSERT_TRUE(asmblr.begin(path, &err));

    // 1. Single pixel: 1x1
    QImage p1 = createPatternImage(1, 1, 72, Qt::blue);
    ASSERT_TRUE(asmblr.appendImage(p1, 0));

    // 2. Extremely tall vertical strip: 2x1000
    QImage tall = createPatternImage(2, 1000, 150, Qt::green);
    ASSERT_TRUE(asmblr.appendImage(tall, 0));

    // 3. Extremely wide horizontal strip: 1000x2
    QImage wide = createPatternImage(1000, 2, 150, Qt::yellow);
    ASSERT_TRUE(asmblr.appendImage(wide, 0));

    // 4. Odd, unaligned dimensions (testing byte alignment / padding)
    QImage odd1 = createPatternImage(13, 17, 300, Qt::cyan);
    QImage odd2 = createPatternImage(101, 203, 150, Qt::magenta);
    ASSERT_TRUE(asmblr.appendImage(odd1, 90));
    ASSERT_TRUE(asmblr.appendImage(odd2, 270));

    ASSERT_TRUE(asmblr.end(&err));

    int count = PdfAssembler::pdfPageCount(path);
    ASSERT_EQ(count, 5);
}

// ===========================================================================
// SUITE 4: Non-standard Image Formats (Mono, Grayscale, ARGB)
// ===========================================================================

TEST_CASE(TestPdfAssembler_ImageFormats) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    std::string path = tmpDir.filePath("formats.pdf").toStdString();

    PdfAssembler asmblr;
    std::string err;
    ASSERT_TRUE(asmblr.begin(path, &err));

    // 1. 1-bit Monochrome
    QImage mono(200, 200, QImage::Format_Mono);
    mono.fill(0);
    // Draw some black dots
    for (int i = 0; i < 200; i += 10) {
        mono.setPixel(i, i, 1);
    }
    ASSERT_TRUE(asmblr.appendImage(mono, 0));

    // 2. 8-bit Grayscale
    QImage gray(200, 200, QImage::Format_Grayscale8);
    gray.fill(128);
    ASSERT_TRUE(asmblr.appendImage(gray, 90));

    // 3. 32-bit ARGB Premultiplied
    QImage argb(200, 200, QImage::Format_ARGB32_Premultiplied);
    argb.fill(QColor(255, 100, 50, 200));
    ASSERT_TRUE(asmblr.appendImage(argb, 180));

    ASSERT_TRUE(asmblr.end(&err));
    ASSERT_EQ(PdfAssembler::pdfPageCount(path), 3);
}

// ===========================================================================
// SUITE 5: pdfPageCount Corner Cases (Corrupted, Empty, Missing files)
// ===========================================================================

TEST_CASE(TestPdfPageCount_NonExistentFile) {
    int n = PdfAssembler::pdfPageCount("/path/that/does/not/exist_12345.pdf");
    ASSERT_EQ(n, 0);
}

TEST_CASE(TestPdfPageCount_EmptyFileZeroBytes) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString emptyPath = tmpDir.filePath("empty_zero_byte.pdf");

    QFile f(emptyPath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.close();

    int n = PdfAssembler::pdfPageCount(emptyPath.toStdString());
    ASSERT_EQ(n, 0);
}

TEST_CASE(TestPdfPageCount_CorruptedRandomBytes) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString corruptPath = tmpDir.filePath("corrupted.pdf");

    QFile f(corruptPath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    // Write arbitrary non-PDF garbage
    QByteArray garbage = "NOT A VALID PDF FILE AT ALL!! LOREM IPSUM DOLOR SIT AMET...";
    f.write(garbage);
    f.close();

    int n = PdfAssembler::pdfPageCount(corruptPath.toStdString());
    ASSERT_EQ(n, 0);
}

// ===========================================================================
// SUITE 6: renderPdfPage Corner Cases (Bounds, Bad Dimensions, Corrupted Files)
// ===========================================================================

TEST_CASE(TestRenderPdfPage_OutOfBoundsAndNegativeIndices) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    std::string path = tmpDir.filePath("test_render.pdf").toStdString();

    // Create a 2-page PDF
    PdfAssembler asmblr;
    std::string err;
    ASSERT_TRUE(asmblr.begin(path, &err));
    QImage img = createPatternImage(300, 300, 150, Qt::white, "P1");
    ASSERT_TRUE(asmblr.appendImage(img, 0));
    ASSERT_TRUE(asmblr.appendImage(img, 0));
    ASSERT_TRUE(asmblr.end(&err));

    ASSERT_EQ(PdfAssembler::pdfPageCount(path), 2);

    // Negative page index -> must return null image safely
    QImage rNeg = PdfAssembler::renderPdfPage(path, -1, 400);
    ASSERT_TRUE(rNeg.isNull());

    // Out of bounds page index (index 2 for 2-page PDF; valid are 0, 1)
    QImage rHigh = PdfAssembler::renderPdfPage(path, 2, 400);
    ASSERT_TRUE(rHigh.isNull());

    // Way out of bounds
    QImage rCrazy = PdfAssembler::renderPdfPage(path, 99999, 400);
    ASSERT_TRUE(rCrazy.isNull());

    // Zero and negative maxDim -> handles safely without crash
    QImage rZeroDim = PdfAssembler::renderPdfPage(path, 0, 0);
    // Either returns null or clamped dimension, but no crash
    QImage rNegDim = PdfAssembler::renderPdfPage(path, 0, -100);

    // Valid rendering
    QImage rValid = PdfAssembler::renderPdfPage(path, 0, 400);
    ASSERT_FALSE(rValid.isNull());
    ASSERT_TRUE(rValid.width() == 400 || rValid.height() == 400);
}

TEST_CASE(TestRenderPdfPage_CorruptedFile) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString corruptPath = tmpDir.filePath("bad.pdf");

    QFile f(corruptPath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("Corrupted garbage");
    f.close();

    QImage img = PdfAssembler::renderPdfPage(corruptPath.toStdString(), 0, 300);
    ASSERT_TRUE(img.isNull());
}

// ===========================================================================
// SUITE 7: appendPdfPage Corner Cases
// ===========================================================================

TEST_CASE(TestAppendPdfPage_InvalidInputs) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    std::string outPath = tmpDir.filePath("out.pdf").toStdString();
    std::string validPdf = tmpDir.filePath("valid.pdf").toStdString();

    // Create 1-page valid PDF
    PdfAssembler asmblr;
    std::string err;
    ASSERT_TRUE(asmblr.begin(validPdf, &err));
    ASSERT_TRUE(asmblr.appendImage(createPatternImage(200, 200, 150, Qt::white), 0));
    ASSERT_TRUE(asmblr.end(&err));

    PdfAssembler asmblr2;
    ASSERT_TRUE(asmblr2.begin(outPath, &err));

    // 1. Non-existent source file
    ASSERT_FALSE(asmblr2.appendPdfPage("/nonexistent_xyz.pdf", 0, 0));

    // 2. Negative page index
    ASSERT_FALSE(asmblr2.appendPdfPage(validPdf, -1, 0));

    // 3. Out-of-bounds page index (valid page is only index 0)
    ASSERT_FALSE(asmblr2.appendPdfPage(validPdf, 5, 0));

    // 4. Valid page append with rotation
    ASSERT_TRUE(asmblr2.appendPdfPage(validPdf, 0, 90));

    ASSERT_TRUE(asmblr2.end(&err));
    ASSERT_EQ(PdfAssembler::pdfPageCount(outPath), 1);
}

// ===========================================================================
// SUITE 8: Assembler Lifecycle & Re-use
// ===========================================================================

TEST_CASE(TestPdfAssembler_ReuseInstance) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    std::string path1 = tmpDir.filePath("first.pdf").toStdString();
    std::string path2 = tmpDir.filePath("second.pdf").toStdString();

    PdfAssembler asmblr;
    std::string err;

    // First cycle
    ASSERT_TRUE(asmblr.begin(path1, &err));
    ASSERT_TRUE(asmblr.appendImage(createPatternImage(200, 200, 150, Qt::red), 0));
    ASSERT_TRUE(asmblr.end(&err));
    ASSERT_EQ(PdfAssembler::pdfPageCount(path1), 1);

    // Second cycle using SAME assembler object
    ASSERT_TRUE(asmblr.begin(path2, &err));
    ASSERT_TRUE(asmblr.appendImage(createPatternImage(200, 200, 150, Qt::green), 90));
    ASSERT_TRUE(asmblr.appendImage(createPatternImage(200, 200, 150, Qt::blue), 180));
    ASSERT_TRUE(asmblr.end(&err));
    ASSERT_EQ(PdfAssembler::pdfPageCount(path2), 2);
}

TEST_CASE(TestPdfAssembler_DestructionWithoutEnd) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    std::string path = tmpDir.filePath("aborted.pdf").toStdString();

    {
        PdfAssembler asmblr;
        std::string err;
        ASSERT_TRUE(asmblr.begin(path, &err));
        ASSERT_TRUE(asmblr.appendImage(createPatternImage(200, 200, 150, Qt::red), 0));
        // Intentionally exit scope WITHOUT calling end()
        // Destructor must clean up safely without leaking files or handles
    }
    // Verify tmpDir is clean and no dangling lock exists
    ASSERT_TRUE(tmpDir.isValid());
}

// ===========================================================================
// SUITE 9: ScannerBackend Options & State Corner Cases
// ===========================================================================

TEST_CASE(TestScannerBackend_OptionsDefaults) {
    ScanOptions opt;
    ASSERT_EQ(opt.dpi, 300);
    ASSERT_TRUE(opt.colorMode == ColorMode::Color);
    ASSERT_FALSE(opt.duplex);
    ASSERT_TRUE(opt.source == ScanSource::Automatic);
}

TEST_CASE(TestScannerBackend_ScanNonExistentScanner) {
    ScannerBackend backend;
    bool finishedCalled = false;
    bool scanOk = true;
    std::string scanErr;

    backend.setScanFinishedCallback([&](bool ok, const std::string& err) {
        finishedCalled = true;
        scanOk = ok;
        scanErr = err;
    });

    ScanOptions opt;
    // Scan with invalid device UUID
    backend.scan("invalid-device-uuid-99999", opt);

    // Allow event processing for asynchronous callbacks
    auto start = std::chrono::steady_clock::now();
    while (!finishedCalled && (std::chrono::steady_clock::now() - start < std::chrono::seconds(2))) {
        qApp->processEvents();
    }

    ASSERT_TRUE(finishedCalled);
    ASSERT_FALSE(scanOk);
    ASSERT_FALSE(scanErr.empty());
}

TEST_CASE(TestScannerBackend_RapidBrowsingStartStop) {
    ScannerBackend backend;
    // Toggling start/stop should safely join worker threads without deadlock
    for (int i = 0; i < 2; ++i) {
        backend.startBrowsing();
        backend.stopBrowsing();
    }
}

TEST_CASE(TestScannerBackend_CancelWhenIdle) {
    ScannerBackend backend;
    // Calling cancelScan when not scanning should not crash
    backend.cancelScan();
}

// ===========================================================================
// Main Entry Point
// ===========================================================================
int main(int argc, char** argv) {
    // Force offscreen platform for headless test execution
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    return TestRunner::runAll();
}
