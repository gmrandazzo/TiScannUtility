# TiScannUtility

A cross-platform desktop app (C++17 / Qt 6 & 5 Widgets) that scans from your
**Canon MF660C Series** (and other network/USB scanners), lets you reorder,
delete, rotate pages, mix in pages from existing PDFs, and save one final PDF.

No proprietary drivers needed:
- **macOS**: Uses native AirScan (eSCL over IPP) via Apple's `ImageCaptureCore`.
- **Linux**: Uses `SANE` (Scanner Access Now Easy) supporting eSCL, AirScan, and local USB.
- **Windows**: WIA / TWAIN backend scaffold.

Author: **Giuseppe Marco Randazzo** <gmrandazzo@gmail.com>

## License

TiScannUtility is free software licensed under the **GNU Affero General Public
License v3.0 or later** — see [LICENSE](LICENSE). Copyright (C) 2026 Giuseppe
Marco Randazzo.

**Qt linking:** this program links against Qt **dynamically** (shared
libraries). The Qt libraries remain under their own license, the **GNU Lesser
General Public License (LGPL) v3**, and are not covered by the AGPL. Dynamic
linking is deliberate — it keeps the Qt framework under LGPL as required for
closed/AGPL combination. Static linking of Qt is not supported by this build.

## Features

- **Scan** with options: resolution (150/300 dpi, higher values are snapped to
  what the scanner supports), Color / Grayscale / Black & White (B&W is
  software-thresholded 1-bit, keeps PDFs small), simplex feeder or flatbed
  (chosen automatically).
- **Add PDF…** imports every page of an existing PDF as draggable thumbnails.
  Imported pages preserve vector content so text stays sharp.
- **Reorder** pages with the Move Left / Move Right buttons (or by dragging
  thumbnails).
- **Delete** pages (button or Delete key).
- **Rotate** pages left/right (90° steps, applied losslessly when saving).
- **Preview** any page: double-click its thumbnail to zoom and read it.
- **Save Final PDF…** assembles everything in the order shown and opens the
  result in your default PDF viewer.

Note: the MF660C's document feeder advertises itself as *simplex* over the
network (no duplex via AirScan), so the Duplex checkbox stays disabled. For
double-sided documents, scan the stack face-up, then flip it over and scan
again — the app merges everything into one document.

## Build

### macOS
Requires: macOS 14+, Xcode Command Line Tools, CMake, and Qt from Homebrew.

```sh
brew install qt cmake
cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Linux
Requires: CMake, GCC/Clang, Qt (Qt 6 or Qt 5 Widgets), SANE (`libsane1`), and Poppler tools (`poppler-utils` or `libpoppler-qt6-dev` / `libqpdf-dev`).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```


1. Make sure the Mac and the printer are on the same network.
2. Launch **TiScannUtility** — after a few seconds the scanner appears in the
   dropdown (it opens its session automatically; first connection takes ~2s).
3. Put paper in the feeder (or on the glass) and click **Scan**. Pages appear
   as thumbnails as they arrive.
4. Reorder with Move Left/Right (or drag), fix orientation with **Rotate**,
   double-click a page to preview it, drop mistakes with **Delete Page**,
   optionally **Add PDF…** to merge an existing file.
5. **Save Final PDF…** — done; the document opens in Preview.

If macOS asks to let the app find devices on the local network, allow it.

## Editing the UI

The main window layout lives in `src/mainwindow.ui` — open it with Designer:

```sh
open /opt/homebrew/opt/qt/libexec/Designer.app src/mainwindow.ui
```

Behavior (scanning, thumbnails, saving) stays in C++ (`MainWindow.cpp`).

## Self tests (no GUI)

```sh
build/TiScannUtility.app/Contents/MacOS/TiScannUtility --selftest   # discovery + session
build/TiScannUtility.app/Contents/MacOS/TiScannUtility --pdftest    # PDF assembly pipeline
```

## How it works
 
 ```
 src/mainwindow.ui          Layout & widgets — edit with Qt Designer
 src/MainWindow.cpp         Behavior: device/options handling, thumbnails, saving
 src/PreviewDialog.*        Zoomable page preview (double-click a thumbnail)
 src/ScannerBackend.h       Backend interface (PIMPL)
 src/ScannerBackend.mm      macOS: ICDeviceBrowser / ICScannerDevice bridge (Objective-C++)
 src/ScannerBackend_Sane.cpp Linux: SANE bridge (eSCL, AirScan, USB)
 src/ScannerBackend_Win.cpp Windows: WIA scaffold
 src/PdfAssembler.h         PDF Assembler interface (PIMPL)
 src/PdfAssembler.cpp       macOS CoreGraphics context / Linux & Windows QPDF, Poppler & Qt
 src/ImageConversion.*      macOS: QImage <-> CGImage helpers
 ```
 
 - **Scanning**:
   - macOS uses `ImageCaptureCore` memory transfer.
   - Linux uses `SANE` asynchronous streaming with dynamic library resolution (`libsane.so.1`).
 - **Saving**:
   - macOS uses `CoreGraphics` (`CGPDFContextDrawPDFPage`).
   - Linux & Windows use vector-preserving PDF assembly (QPDF / Poppler / `QPdfWriter`).


## Troubleshooting

- **Scanner never appears**: check the printer is awake and on the same
  network; make sure System Settings → Privacy & Security → Local Network
  allows TiScannUtility.
- **"no pages received"**: the feeder was empty (or glass empty) — load paper
  and scan again.
