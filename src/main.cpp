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
#include "MainWindow.h"
#include "ScannerBackend.h"
#include "PdfAssembler.h"

#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QTimer>
#include <QTemporaryDir>

#include <cstdio>

// Builds a couple of test images and exercises the PDF pipeline:
// images with rotations + importing pages from an existing PDF.
static int runPdfTest() {
    auto makeImage = [](const char* text, QColor bg) {
        QImage img(1240, 1754, QImage::Format_RGB32); // ~150dpi A4
        int dpm = int(150 / 0.0254);
        img.setDotsPerMeterX(dpm);
        img.setDotsPerMeterY(dpm);
        img.fill(bg);
        QPainter p(&img);
        p.setPen(Qt::black);
        QFont f = p.font();
        f.setPointSize(64);
        p.setFont(f);
        p.drawText(img.rect(), Qt::AlignCenter, QString::fromLatin1(text));
        p.end();
        return img;
    };

    QImage a = makeImage("PAGE A", Qt::white);
    QImage b = makeImage("PAGE B rot90", Qt::lightGray);
    QImage c = makeImage("PAGE C rot180", Qt::white);

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) { std::puts("tempdir failed"); return 1; }

    const std::string part1 = tempDir.filePath("pdftest_part1.pdf").toStdString();
    const std::string final = tempDir.filePath("pdftest_final.pdf").toStdString();

    PdfAssembler p1;
    std::string err;
    if (!p1.begin(part1, &err)) { std::printf("begin1 failed: %s\n", err.c_str()); return 1; }
    if (!p1.appendImage(a, 0)) { std::printf("append A failed\n"); return 1; }
    if (!p1.appendImage(b, 90)) { std::printf("append B failed\n"); return 1; }
    if (!p1.end(&err)) { std::printf("end1 failed: %s\n", err.c_str()); return 1; }

    PdfAssembler pf;
    if (!pf.begin(final, &err)) { std::printf("begin final failed: %s\n", err.c_str()); return 1; }
    if (!pf.appendImage(c, 180)) { std::printf("append C failed\n"); return 1; }
    if (!pf.appendPdfPage(part1, 0, 0)) { std::printf("import page1 failed\n"); return 1; }
    if (!pf.appendPdfPage(part1, 1, 270)) { std::printf("import page2 failed\n"); return 1; }
    if (!pf.end(&err)) { std::printf("end final failed: %s\n", err.c_str()); return 1; }

    int n = PdfAssembler::pdfPageCount(final);
    std::printf("part1 pages=%d, final pages=%d (expect 2 and 3)\n",
                PdfAssembler::pdfPageCount(part1), n);
    for (int i = 0; i < n; ++i) {
        QString png = tempDir.filePath(QString("pdftest_page%1.png").arg(i));
        PdfAssembler::renderPdfPage(final, i, 500).save(png, "PNG");
    }
    std::puts("page previews written to temp dir");
    std::printf("PDFTEST %s\n", n == 3 ? "PASS" : "FAIL");
    return n == 3 ? 0 : 1;
}

// Headless check: discover scanners, open a session, print capabilities.
static int runSelfTest(QApplication& app) {
    ScannerBackend backend;
    bool sawDevice = false;
    bool sawReadyDevice = false;

    backend.setDeviceListCallback([&](const std::vector<ScannerInfo>& infos) {
        for (const ScannerInfo& info : infos) {
            std::printf("device: '%s' uuid=%s ready=%d feeder=%d duplex=%d\n",
                        info.name.c_str(), info.uuid.c_str(), int(info.ready),
                        int(info.hasDocumentFeeder), int(info.supportsDuplex));
            sawDevice = true;
            if (info.ready) sawReadyDevice = true;
        }
    });
    backend.setStatusCallback([&](const std::string& msg) {
        std::printf("status: %s\n", msg.c_str());
    });

    backend.startBrowsing();
    QTimer::singleShot(15000, &app, &QCoreApplication::quit);
    QApplication::exec();
    backend.stopBrowsing();

    std::printf("SELFTEST %s\n", (sawDevice && sawReadyDevice) ? "PASS" : "FAIL");
    return (sawDevice && sawReadyDevice) ? 0 : 1;
}

int main(int argc, char** argv) {
    bool selfTest = argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--selftest");
    bool pdfTest = argc > 1 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--pdftest");
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/app_icon.png"));
    if (pdfTest)
        return runPdfTest();
    if (selfTest)
        return runSelfTest(app);

    MainWindow window;
    window.show();
    return QApplication::exec();
}
