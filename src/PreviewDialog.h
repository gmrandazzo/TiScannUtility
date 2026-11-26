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

#include <QDialog>
#include <QImage>

class QLabel;
class QScrollArea;

// Simple image viewer: zoom in/out, fit to window, 100% size.
// The image passed in is shown exactly as given (rotation is applied
// by the caller before opening the dialog).
class PreviewDialog : public QDialog {
    Q_OBJECT
public:
    PreviewDialog(const QImage& image, const QString& title, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private slots:
    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void actualSize();

private:
    void updatePixmap();

    QImage image_;
    QLabel* imageLabel_ = nullptr;
    QScrollArea* scroll_ = nullptr;
    double scale_ = 1.0;
    bool fitMode_ = true;
};
