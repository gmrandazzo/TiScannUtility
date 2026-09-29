# TiScannUtility

![TiScannUtility screenshot](tiscannutility.png)

A fast, cross-compatible macOS scanning application (C++17 / Qt 6 Widgets + macOS ImageCaptureCore) that allows you to scan over the network from any supported macOS scanner (including the **Canon MF660C Series**), rearrange pages, merge existing PDFs, and save a final PDF document without losing vector quality.

No proprietary drivers needed — macOS talks to the scanner natively via AirScan (eSCL over IPP), the same technology Apple's Image Capture app uses.

### 1. Install dependencies

Requires macOS 14+, Xcode Command Line Tools, CMake, and Qt 6 from Homebrew.

```bash
brew install qt cmake
```

### 2. Build the application

Configure the project and build it directly on your machine:

```bash
cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The app is built at `build/TiScannUtility.app`. Because it dynamically links to Qt via Homebrew, you should run `macdeployqt` to bundle the libraries and make the application completely standalone:

```bash
$(brew --prefix qt)/bin/macdeployqt build/TiScannUtility.app
```

### 3. Use the application

1. Make sure your Mac and the printer/scanner are on the same local network.
2. Launch **TiScannUtility** from the `build` directory. Wait a few seconds for scanners on your network to be automatically discovered.
3. Choose the **Source** (Automatic, Flatbed, or Feeder), set options like DPI and Color Mode, load your document, and click **Scan**.
4. Reorder pages using the **Move Left/Right** buttons (or drag and drop). Double-click a thumbnail for a high-res preview.
5. Use **Add PDF...** to merge pages from an existing PDF losslessly. 
6. Click **Save Final PDF...** to finish and open the document in Preview.

### 4. Check that it is working via Self Tests

If you run into issues, you can run headless tests from the terminal to diagnose the problem:

```bash
# Tests device discovery and opens a network session
build/TiScannUtility.app/Contents/MacOS/TiScannUtility --selftest   

# Tests the CoreGraphics PDF assembly and rotation pipeline
build/TiScannUtility.app/Contents/MacOS/TiScannUtility --pdftest    
```

## Features

- **Scan Options**: Resolution (150/300/600 dpi), Color Modes (Color / Grayscale / Black & White via software thresholding to keep sizes small).
- **Multiple Sources**: Smart automatic switching between the Automatic Document Feeder (ADF) and the Flatbed glass.
- **Add PDF**: Imports every page of an existing PDF as draggable thumbnails. Imported pages are re-drawn natively via CoreGraphics (not rasterized), so vector text stays perfectly sharp.
- **Organize & Rotate**: Move pages, rotate in 90° steps losslessly, or delete mistakes.

## Project structure

```
TiScannUtility/
├── src/
│   ├── main.cpp          # Entry point and self-tests
│   ├── MainWindow.*      # UI behavior, device handling, and thumbnails
│   ├── mainwindow.ui     # Layout & widgets (editable via Qt Designer)
│   ├── ScannerBackend.*  # ICDeviceBrowser / ICScannerDevice bridge (Objective-C++)
│   ├── PdfAssembler.*    # CoreGraphics PDF context writer and compiler
│   ├── ImageConversion.* # QImage ↔ CGImage conversion helpers
│   └── PreviewDialog.*   # Zoomable page preview UI
├── tests/                # Test utilities
├── cmake/                # Application property lists and assets
├── CMakeLists.txt        # CMake build configuration
├── .github/
│   └── workflows/
│       └── release.yml   # GitHub Actions Pipeline for automated releases
├── .gitignore            # Git ignore file
└── README.md             # This documentation
```

## Troubleshooting

- **Scanner never appears**: Ensure your printer is awake and on the same network. Also, verify that macOS allows the app to find local devices (`System Settings` → `Privacy & Security` → `Local Network`).
- **"No pages received"**: Ensure your selected source actually has paper loaded. If the feeder is empty and you force the Feeder source, it will return nothing. Change the Source dropdown to `Automatic` or `Flatbed`.
- **Duplex is disabled**: Many scanners (like the MF660C) advertise their document feeders as *simplex* over the network via AirScan. For double-sided documents, scan the stack face-up, flip it, scan again, and reorder the pages in the app.

## AI Full Disclosure

This software is developed with strong assistance from Kimi 2.8 and Gemini 3.1 Pro (High) and with humans leading the ideas, testing, and debugging. We say this openly because it shaped how the project was built. If you are not happy with AI-developed code, this software is not for you.

## License

This project is licensed under the **GNU Affero General Public License v3.0 or later** (AGPLv3). See `LICENSE`.
Copyright (C) 2026 Giuseppe Marco Randazzo <gmrandazzo@gmail.com>.

**Qt linking:** this program links against Qt **dynamically** (shared libraries). The Qt libraries remain under their own license, the **GNU Lesser General Public License (LGPL) v3**, and are not covered by the AGPL. Dynamic linking is deliberate — it keeps the Qt framework under LGPL as required for closed/AGPL combinations. Static linking of Qt is not supported by this build.
