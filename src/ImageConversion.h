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

#if defined(__APPLE__)

#include <QImage>

#include <CoreGraphics/CGImage.h>

// Converts a CGImage to a QImage (8-bit ARGB), tagging it with the given DPI
// so PDF pages come out at real-world size. dpi <= 0 leaves the tag unset.
QImage qImageFromCGImage(CGImageRef image, int dpi);

// Converts a QImage to a CGImage. Supports RGB(A) and monochrome formats.
// The caller takes ownership of the returned CGImage (CGImageRelease).
CGImageRef cgImageFromQImage(const QImage& image);

#endif // __APPLE__

