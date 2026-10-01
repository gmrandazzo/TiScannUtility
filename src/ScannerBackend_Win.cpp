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
#include "ScannerBackend.h"

#if defined(Q_OS_WIN)

#include <QCoreApplication>
#include <QMetaObject>

// Windows WIA / TWAIN scaffold implementation
struct ScannerBackend::Impl {
    std::function<void(const std::vector<ScannerInfo>&)> deviceListCb;
    std::function<void(const QImage&)> pageCb;
    std::function<void(bool, const std::string&)> finishedCb;
    std::function<void(const std::string&)> statusCb;
    bool scanning = false;
};

ScannerBackend::ScannerBackend() : d(std::make_unique<Impl>()) {}
ScannerBackend::~ScannerBackend() = default;

void ScannerBackend::setDeviceListCallback(std::function<void(const std::vector<ScannerInfo>&)> cb) { d->deviceListCb = std::move(cb); }
void ScannerBackend::setPageCallback(std::function<void(const QImage&)> cb) { d->pageCb = std::move(cb); }
void ScannerBackend::setScanFinishedCallback(std::function<void(bool, const std::string&)> cb) { d->finishedCb = std::move(cb); }
void ScannerBackend::setStatusCallback(std::function<void(const std::string&)> cb) { d->statusCb = std::move(cb); }

void ScannerBackend::startBrowsing() {
    if (d->statusCb) d->statusCb("Windows WIA scanner support initialized.");
    if (d->deviceListCb) d->deviceListCb({});
}

void ScannerBackend::stopBrowsing() {}

void ScannerBackend::scan(const std::string& /*deviceUuid*/, const ScanOptions& /*options*/) {
    if (d->finishedCb) d->finishedCb(false, "WIA scanning backend not yet implemented on Windows.");
}

void ScannerBackend::cancelScan() {}

#endif // Q_OS_WIN
