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

#include "ui_mainwindow.h"

#include "PdfAssembler.h"
#include "PreviewDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>
#include <QKeySequence>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QShortcut>
#include <QStatusBar>
#include <QUrl>
#include <QVariant>

#include <algorithm>

namespace {
constexpr int kThumbWidth = 180;

QString rotationSuffix(int rotation) {
    return rotation ? QString(" (%1°)").arg(rotation) : QString();
}
} // namespace

MainWindow::MainWindow()
    : ui(new Ui::MainWindow) {
    ui->setupUi(this);

    ui->duplexCheck->setToolTip("Scan both sides (needs a duplex document feeder)");

    busyBar = new QProgressBar(this);
    busyBar->setRange(0, 0); // busy indicator
    busyBar->setMaximumWidth(180);
    busyBar->setVisible(false);
    statusBar()->addPermanentWidget(busyBar);
    statusBar()->showMessage("Looking for scanners...");

    // --- backend wiring (all callbacks arrive on the main thread) ---
    backend.setDeviceListCallback([this](const std::vector<ScannerInfo>& infos) {
        devices = infos;
        rebuildDeviceCombo();
    });
    backend.setStatusCallback([this](const std::string& msg) {
        statusBar()->showMessage(QString::fromStdString(msg));
    });
    backend.setPageCallback([this](const QImage& img) { addScannedPage(img); });
    backend.setScanFinishedCallback([this](bool ok, const std::string& err) {
        scanning = false;
        setBusy(false, ok ? "Scan finished" : QString::fromStdString(err));
        if (!ok && err != "cancelled")
            QMessageBox::warning(this, "Scan", QString::fromStdString(err));
    });
    backend.startBrowsing();

    // --- connections that the .ui file cannot express ---
    connect(ui->refreshBtn, &QToolButton::clicked, this, [this] {
        rebuildDeviceCombo();
        statusBar()->showMessage("Looking for scanners...");
    });
    connect(ui->deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { updateDuplexAvailability(); });

    auto* delShortcut = new QShortcut(QKeySequence::Delete, this);
    connect(delShortcut, &QShortcut::activated, this, &MainWindow::onDeleteSelected);

    // double-click a thumbnail to preview it full-size
    connect(ui->list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        PageItem* page = pageOf(item);
        if (!page) return;
        QImage full = page->isPdf
            ? PdfAssembler::renderPdfPage(page->pdfPath.toStdString(), page->pdfPage, 2400)
            : page->image;
        if (full.isNull()) return;
        if (page->rotation)
            full = full.transformed(QTransform().rotate(page->rotation), Qt::SmoothTransformation);
        PreviewDialog dialog(full, page->label, this);
        dialog.exec();
    });
}

MainWindow::~MainWindow() {
    delete ui;
}

void MainWindow::rebuildDeviceCombo() {
    QString previous = ui->deviceCombo->currentData().toString();
    ui->deviceCombo->clear();
    int selectIndex = -1;
    for (const ScannerInfo& info : devices) {
        QString label = QString::fromStdString(info.name);
        if (!info.ready)
            label += " (connecting...)";
        else if (info.hasDocumentFeeder)
            label += info.supportsDuplex ? " (feeder, duplex)" : " (feeder)";
        else
            label += " (flatbed)";
        ui->deviceCombo->addItem(label, QString::fromStdString(info.uuid));
        if (QString::fromStdString(info.uuid) == previous)
            selectIndex = ui->deviceCombo->count() - 1;
    }
    if (selectIndex >= 0)
        ui->deviceCombo->setCurrentIndex(selectIndex);
    else if (ui->deviceCombo->count() > 0)
        ui->deviceCombo->setCurrentIndex(0);
    updateDuplexAvailability();
}

void MainWindow::updateDuplexAvailability() {
    QString uuid = ui->deviceCombo->currentData().toString();
    const ScannerInfo* current = nullptr;
    for (const ScannerInfo& info : devices) {
        if (QString::fromStdString(info.uuid) == uuid) {
            current = &info;
            break;
        }
    }
    bool ready = current && current->ready;
    ui->duplexCheck->setEnabled(ready && current->supportsDuplex);
    if (!ui->duplexCheck->isEnabled()) ui->duplexCheck->setChecked(false);
    ui->scanBtn->setEnabled(ready && !scanning);
}

void MainWindow::setBusy(bool busy, const QString& message) {
    busyBar->setVisible(busy);
    ui->scanBtn->setEnabled(!busy && ui->deviceCombo->count() > 0);
    ui->cancelBtn->setVisible(busy);
    ui->scanBtn->setVisible(!busy);
    ui->duplexCheck->setEnabled(!busy && ui->duplexCheck->isEnabled());
    if (!message.isEmpty()) statusBar()->showMessage(message);
}

void MainWindow::onScan() {
    if (scanning) return;
    QString uuid = ui->deviceCombo->currentData().toString();
    if (uuid.isEmpty()) return;

    ScanOptions options;
    options.dpi = ui->dpiCombo->currentText().toInt();
    options.colorMode = ColorMode(ui->modeCombo->currentIndex());
    options.duplex = ui->duplexCheck->isChecked();
    if (options.colorMode == ColorMode::BlackWhite && options.dpi > 300)
        statusBar()->showMessage("Note: 600 dpi B&W produces very large pages; 300 dpi is usually enough.");

    scanning = true;
    setBusy(true, "Scanning — load your document and wait...");
    backend.scan(uuid.toStdString(), options);
}

void MainWindow::onCancel() {
    backend.cancelScan();
}

void MainWindow::addScannedPage(const QImage& img) {
    if (img.isNull()) return;
    auto page = std::make_unique<PageItem>();
    page->image = img;
    page->label = QString("Scan %1").arg(++scanCounter);
    pages.push_back(std::move(page));
    auto* item = new QListWidgetItem(ui->list);
    item->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(pages.back().get())));
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
    item->setTextAlignment(Qt::AlignHCenter);
    refreshThumb(item, pages.back().get());
    ui->list->scrollToItem(item);
    statusBar()->showMessage(QString("Got page %1 (%2x%3)").arg(scanCounter).arg(img.width()).arg(img.height()));
}

void MainWindow::addPdfPageItem(const QString& path, int pageIndex, int total) {
    auto page = std::make_unique<PageItem>();
    page->isPdf = true;
    page->pdfPath = path;
    page->pdfPage = pageIndex;
    page->label = QString("PDF %1 · p.%2").arg(++importCounter).arg(pageIndex + 1);
    PageItem* raw = page.get();
    pages.push_back(std::move(page));
    auto* item = new QListWidgetItem(ui->list);
    item->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(raw)));
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
    item->setTextAlignment(Qt::AlignHCenter);
    refreshThumb(item, raw);
    statusBar()->showMessage(QString("Importing PDF: page %1/%2").arg(pageIndex + 1).arg(total));
}

QImage MainWindow::makeThumb(const PageItem& page) {
    QImage src = page.isPdf
        ? PdfAssembler::renderPdfPage(page.pdfPath.toStdString(), page.pdfPage, 480)
        : page.image;
    if (src.isNull()) return QImage();
    QImage small = src.scaledToWidth(kThumbWidth, Qt::SmoothTransformation);
    if (page.rotation)
        small = small.transformed(QTransform().rotate(page.rotation), Qt::SmoothTransformation);
    return small;
}

void MainWindow::refreshThumb(QListWidgetItem* item, PageItem* page) {
    QImage thumb = makeThumb(*page);
    if (!thumb.isNull()) {
        item->setIcon(QIcon(QPixmap::fromImage(thumb)));
        item->setSizeHint(QSize(kThumbWidth + 30, thumb.height() + 44));
    }
    item->setText(page->label + rotationSuffix(page->rotation));
}

PageItem* MainWindow::pageOf(QListWidgetItem* item) const {
    if (!item) return nullptr;
    return reinterpret_cast<PageItem*>(item->data(Qt::UserRole).value<void*>());
}

QList<QListWidgetItem*> MainWindow::selectedPageItems() const {
    return ui->list->selectedItems();
}

void MainWindow::onAddPdf() {
    QString path = QFileDialog::getOpenFileName(this, "Add PDF pages", QDir::homePath(), "PDF files (*.pdf)");
    if (path.isEmpty()) return;
    int n = PdfAssembler::pdfPageCount(path.toStdString());
    if (n <= 0) {
        QMessageBox::warning(this, "Add PDF", "Could not read this PDF file.");
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (int i = 0; i < n; ++i) {
        addPdfPageItem(path, i, n);
        if (i % 10 == 0) QCoreApplication::processEvents();
    }
    QApplication::restoreOverrideCursor();
    statusBar()->showMessage(QString("Imported %1 page(s) from %2").arg(n).arg(QFileInfo(path).fileName()));
}

void MainWindow::onDeleteSelected() {
    auto sel = selectedPageItems();
    if (sel.isEmpty()) return;
    for (QListWidgetItem* item : sel) {
        PageItem* page = pageOf(item);
        delete item;
        auto it = std::find_if(pages.begin(), pages.end(), [&](const std::unique_ptr<PageItem>& p) { return p.get() == page; });
        if (it != pages.end()) pages.erase(it);
    }
    statusBar()->showMessage(QString("Deleted %1 page(s)").arg(sel.size()));
}

void MainWindow::onRotateLeft() {
    auto sel = selectedPageItems();
    for (QListWidgetItem* item : sel) {
        PageItem* page = pageOf(item);
        if (!page) continue;
        page->rotation = (page->rotation + 270) % 360;
        refreshThumb(item, page);
    }
}

void MainWindow::onRotateRight() {
    auto sel = selectedPageItems();
    for (QListWidgetItem* item : sel) {
        PageItem* page = pageOf(item);
        if (!page) continue;
        page->rotation = (page->rotation + 90) % 360;
        refreshThumb(item, page);
    }
}

void MainWindow::onMoveLeft() {
    moveCurrentRow(-1);
}

void MainWindow::onMoveRight() {
    moveCurrentRow(+1);
}

void MainWindow::moveCurrentRow(int delta) {
    int row = ui->list->currentRow();
    int target = row + delta;
    if (row < 0 || target < 0 || target >= ui->list->count()) return;
    QListWidgetItem* item = ui->list->takeItem(row);
    ui->list->insertItem(target, item);
    ui->list->setCurrentItem(item);
}

void MainWindow::onSave() {
    if (ui->list->count() == 0) {
        QMessageBox::information(this, "Save", "No pages to save yet. Scan or add a PDF first.");
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, "Save final PDF", QDir::home().filePath("scanned.pdf"), "PDF files (*.pdf)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".pdf", Qt::CaseInsensitive)) path += ".pdf";

    QApplication::setOverrideCursor(Qt::WaitCursor);
    PdfAssembler assembler;
    std::string err;
    bool ok = assembler.begin(path.toStdString(), &err);
    for (int i = 0; ok && i < ui->list->count(); ++i) {
        PageItem* page = pageOf(ui->list->item(i));
        if (!page) continue;
        if (page->isPdf)
            ok = assembler.appendPdfPage(page->pdfPath.toStdString(), page->pdfPage, page->rotation);
        else
            ok = assembler.appendImage(page->image, page->rotation);
        if (i % 5 == 0) QCoreApplication::processEvents();
    }
    std::string endErr;
    bool endOk = assembler.end(&endErr);
    if (!ok) {
        if (err.empty()) err = "Failed to append all pages to the PDF.";
    } else if (!endOk) {
        ok = false;
        err = endErr;
    }
    QApplication::restoreOverrideCursor();

    if (!ok) {
        QMessageBox::critical(this, "Save", QString("Could not save PDF:\n%1").arg(QString::fromStdString(err)));
        return;
    }
    statusBar()->showMessage(QString("Saved %1 page(s) to %2").arg(ui->list->count()).arg(path));
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::onAbout() {
    QMessageBox::about(this, "About TiScannUtility",
        QString("<h3>TiScannUtility %1</h3>"
                "<p>Scan, rearrange, rotate and merge pages into a final PDF.</p>"
                "<p>Author: <b>Giuseppe Marco Randazzo</b> &lt;gmrandazzo@gmail.com&gt;</p>"
                "<p>Licensed under the GNU AGPL v3. Qt is linked dynamically and remains under the LGPL.</p>"
                "<p>Built with Qt %2 and macOS ImageCaptureCore.</p>")
            .arg(QApplication::applicationVersion(), QLatin1String(qVersion())));
}

void MainWindow::onClose() {
    QApplication::quit();
}
