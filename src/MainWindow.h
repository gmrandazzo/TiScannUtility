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
#pragma once

#include "ScannerBackend.h"

#include <QMainWindow>

#include <memory>
#include <vector>

class QListWidgetItem;
class QProgressBar;
namespace Ui { class MainWindow; }

struct PageItem {
    QImage image;            // scanned page at full resolution
    bool isPdf = false;      // page imported from an existing PDF
    QString pdfPath;         //   source file (isPdf)
    int pdfPage = 0;         //   zero-based page number in source file (isPdf)
    int rotation = 0;        // 0/90/180/270 clockwise
    QString label;           // display name ("Scan 3", "PDF 2 · p.5")
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

private slots:
    void onScan();
    void onCancel();
    void onAddPdf();
    void onDeleteSelected();
    void onRotateLeft();
    void onRotateRight();
    void onMoveLeft();
    void onMoveRight();
    void onSave();
    void onAbout();
    void onClose();

private:
    void rebuildDeviceCombo();
    void updateDuplexAvailability();
    void addScannedPage(const QImage& img);
    void addPdfPageItem(const QString& path, int pageIndex, int total);
    void refreshThumb(QListWidgetItem* item, PageItem* page);
    void setBusy(bool busy, const QString& message);
    void moveCurrentRow(int delta);
    PageItem* pageOf(QListWidgetItem* item) const;
    QList<QListWidgetItem*> selectedPageItems() const;
    static QImage makeThumb(const PageItem& page);

    Ui::MainWindow* ui;
    ScannerBackend backend;
    std::vector<ScannerInfo> devices;
    std::vector<std::unique_ptr<PageItem>> pages; // owns the PageItems; list order = output order

    QProgressBar* busyBar = nullptr;
    bool scanning = false;
    int scanCounter = 0;
    int importCounter = 0;
};
