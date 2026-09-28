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

#include <string>

// Builds a PDF document from scanned images and/or pages of existing PDF files.
// Usage: begin() -> appendImage()/appendPdfPage() in page order -> end().
class PdfAssembler {
public:
    PdfAssembler();
    ~PdfAssembler();

    bool begin(const std::string& path, std::string* error);
    // rotationDeg: 0, 90, 180 or 270 (clockwise).
    bool appendImage(const QImage& image, int rotationDeg);
    // pageIndex is zero-based. Pages are drawn, not rasterized, so vector
    // content of the source PDF is preserved.
    bool appendPdfPage(const std::string& pdfPath, int pageIndex, int rotationDeg);
    bool end(std::string* error);

    static int pdfPageCount(const std::string& pdfPath);
    // Renders a page to a small bitmap (maxDim on the longest side) for thumbnails.
    static QImage renderPdfPage(const std::string& pdfPath, int pageIndex, int maxDim);

private:
    struct Impl;
    Impl* d;
};
