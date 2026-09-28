# TiScannUtility — scan, rearrange, rotate and merge pages into a final PDF.
# Copyright (C) 2026 Giuseppe Marco Randazzo <gmrandazzo@gmail.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Qt is linked dynamically; the Qt libraries remain under the LGPL.

cmake_minimum_required(VERSION 3.21)

project(TiScannUtility VERSION 1.0.0 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")

# Qt 6 from Homebrew is a shared build; linking stays dynamic so the Qt
# libraries remain under the GNU LGPL. Static Qt linking is not supported here.
find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/mainwindow.ui
    src/PreviewDialog.h
    src/PreviewDialog.cpp
    src/ScannerBackend.h
    src/ScannerBackend.mm
    src/ImageConversion.h
    src/ImageConversion.cpp
    src/PdfAssembler.h
    src/PdfAssembler.cpp
)

qt_add_executable(TiScannUtility MACOSX_BUNDLE ${SOURCES})

# ScannerBackend.mm uses Objective-C with ARC
set_source_files_properties(src/ScannerBackend.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

target_include_directories(TiScannUtility PRIVATE src)

target_link_libraries(TiScannUtility PRIVATE
    Qt6::Widgets
    "-framework ImageCaptureCore"
    "-framework CoreGraphics"
)

set_target_properties(TiScannUtility PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "TiScannUtility"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
)
