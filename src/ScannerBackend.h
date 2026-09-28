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

#include <QImage>

#include <functional>
#include <string>
#include <vector>

enum class ColorMode { Color, Grayscale, BlackWhite };

struct ScanOptions {
    int dpi = 300;
    ColorMode colorMode = ColorMode::Color;
    bool duplex = false;
};

struct ScannerInfo {
    std::string uuid;
    std::string name;
    bool ready = false;             // session open, capabilities known
    bool hasDocumentFeeder = false;
    bool supportsDuplex = false;
};

// Drives scanners through macOS ImageCaptureCore.
// All callbacks are invoked on the main thread (ICA guarantees this),
// so they can safely touch Qt GUI objects.
class ScannerBackend {
public:
    ScannerBackend();
    ~ScannerBackend();

    void setDeviceListCallback(std::function<void(const std::vector<ScannerInfo>&)> cb);
    void setPageCallback(std::function<void(const QImage&)> cb);
    void setScanFinishedCallback(std::function<void(bool ok, const std::string& error)> cb);
    void setStatusCallback(std::function<void(const std::string& message)> cb);

    void startBrowsing();
    void stopBrowsing();

    void scan(const std::string& deviceUuid, const ScanOptions& options);
    void cancelScan();

    struct Impl;

private:
    Impl* d;
};
