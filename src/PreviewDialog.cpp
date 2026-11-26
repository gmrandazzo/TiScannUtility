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
#include "PreviewDialog.h"

#include <QGuiApplication>
#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

PreviewDialog::PreviewDialog(const QImage& image, const QString& title, QWidget* parent)
    : QDialog(parent)
    , image_(image) {
    setWindowTitle(QString("Preview — %1").arg(title));

    auto* vbox = new QVBoxLayout(this);

    scroll_ = new QScrollArea(this);
    imageLabel_ = new QLabel;
    imageLabel_->setAlignment(Qt::AlignCenter);
    scroll_->setWidget(imageLabel_);
    scroll_->setAlignment(Qt::AlignCenter);
    vbox->addWidget(scroll_, 1);

    auto* buttons = new QHBoxLayout;
    auto* zoomOutBtn = new QPushButton("Zoom Out", this);
    auto* zoomInBtn = new QPushButton("Zoom In", this);
    auto* fitBtn = new QPushButton("Fit Window", this);
    auto* actualBtn = new QPushButton("Actual Size", this);
    auto* closeBtn = new QPushButton("Close", this);
    buttons->addStretch(1);
    buttons->addWidget(zoomOutBtn);
    buttons->addWidget(zoomInBtn);
    buttons->addWidget(fitBtn);
    buttons->addWidget(actualBtn);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn);
    vbox->addLayout(buttons);

    connect(zoomInBtn, &QPushButton::clicked, this, &PreviewDialog::zoomIn);
    connect(zoomOutBtn, &QPushButton::clicked, this, &PreviewDialog::zoomOut);
    connect(fitBtn, &QPushButton::clicked, this, &PreviewDialog::fitToWindow);
    connect(actualBtn, &QPushButton::clicked, this, &PreviewDialog::actualSize);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    // open at ~70% of the screen, image fitted
    if (QScreen* screen = QGuiApplication::screenAt(QCursor::pos()))
        resize(screen->availableSize() * 7 / 10);
    else
        resize(1000, 800);
}

void PreviewDialog::updatePixmap() {
    if (image_.isNull()) return;
    QSize target(std::max(1, int(image_.width() * scale_)),
                 std::max(1, int(image_.height() * scale_)));
    QImage scaled = image_.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    imageLabel_->setPixmap(QPixmap::fromImage(scaled));
    imageLabel_->resize(scaled.size());
}

void PreviewDialog::zoomIn() {
    fitMode_ = false;
    scale_ = std::min(scale_ * 1.25, 16.0);
    updatePixmap();
}

void PreviewDialog::zoomOut() {
    fitMode_ = false;
    scale_ = std::max(scale_ / 1.25, 0.05);
    updatePixmap();
}

void PreviewDialog::fitToWindow() {
    fitMode_ = true;
    QSize vp = scroll_->viewport()->size();
    if (vp.width() <= 8 || vp.height() <= 8 || image_.isNull()) return;
    double sx = double(vp.width() - 4) / image_.width();
    double sy = double(vp.height() - 4) / image_.height();
    scale_ = std::max(0.05, std::min(sx, sy));
    updatePixmap();
}

void PreviewDialog::actualSize() {
    fitMode_ = false;
    scale_ = 1.0;
    updatePixmap();
}

void PreviewDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (fitMode_) {
        fitToWindow();
        QTimer::singleShot(0, this, [this] {
            if (fitMode_) fitToWindow();
        });
    }
}

void PreviewDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (fitMode_) {
        fitToWindow();
        QTimer::singleShot(0, this, [this] {
            if (fitMode_) fitToWindow();
        });
    }
}

void PreviewDialog::wheelEvent(QWheelEvent* event) {
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->angleDelta().y() > 0)
            zoomIn();
        else
            zoomOut();
        event->accept();
    } else {
        QDialog::wheelEvent(event);
    }
}
