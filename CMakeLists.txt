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

if(APPLE)
    project(TiScannUtility VERSION 1.2.0 LANGUAGES CXX OBJCXX)
else()
    project(TiScannUtility VERSION 1.2.0 LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0")

# Support both Qt 6 and Qt 5 dynamically linked
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Gui Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Gui Core)

# macOS 26+ SDK removed the deprecated AGL framework still referenced by older Qt6 CMake configs.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_wrap_opengl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(_wrap_opengl_libs)
        list(REMOVE_ITEM _wrap_opengl_libs "-framework AGL" "AGL")
        string(REPLACE "-framework AGL" "" _wrap_opengl_libs "${_wrap_opengl_libs}")
        set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_wrap_opengl_libs}")
    endif()
endif()

if(COMMAND qt_standard_project_setup)
    qt_standard_project_setup()
endif()

set(COMMON_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/mainwindow.ui
    src/PreviewDialog.h
    src/PreviewDialog.cpp
    src/ScannerBackend.h
    src/PdfAssembler.h
    src/PdfAssembler.cpp
    src/resources.qrc
)

if(APPLE)
    set(PLATFORM_SOURCES
        src/ScannerBackend.mm
        src/ImageConversion.h
        src/ImageConversion.cpp
        cmake/icon.icns
    )
    set_source_files_properties(src/ScannerBackend.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set_source_files_properties(cmake/icon.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
elseif(WIN32)
    set(PLATFORM_SOURCES
        src/ScannerBackend_Win.cpp
    )
else()
    set(PLATFORM_SOURCES
        src/ScannerBackend_Sane.cpp
    )
endif()

set(SOURCES ${COMMON_SOURCES} ${PLATFORM_SOURCES})

if(COMMAND qt_add_executable)
    qt_add_executable(TiScannUtility MACOSX_BUNDLE ${SOURCES})
else()
    add_executable(TiScannUtility MACOSX_BUNDLE ${SOURCES})
endif()

target_include_directories(TiScannUtility PRIVATE src)
target_compile_definitions(TiScannUtility PRIVATE APP_VERSION="${PROJECT_VERSION}")

target_link_libraries(TiScannUtility PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Core
)

if(APPLE)
    target_link_libraries(TiScannUtility PRIVATE
        "-framework ImageCaptureCore"
        "-framework CoreGraphics"
    )

    set_target_properties(TiScannUtility PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "TiScannUtility"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_ICON_FILE "icon.icns"
    )
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(TiScannUtility PRIVATE
        ${CMAKE_DL_LIBS}
        Threads::Threads
    )

    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(POPPLER_QT QUIET poppler-qt${QT_VERSION_MAJOR})
        if(POPPLER_QT_FOUND)
            target_include_directories(TiScannUtility PRIVATE ${POPPLER_QT_INCLUDE_DIRS})
            target_link_libraries(TiScannUtility PRIVATE ${POPPLER_QT_LIBRARIES})
        endif()

        pkg_check_modules(QPDF QUIET libqpdf)
        if(QPDF_FOUND)
            target_include_directories(TiScannUtility PRIVATE ${QPDF_INCLUDE_DIRS})
            target_link_libraries(TiScannUtility PRIVATE ${QPDF_LIBRARIES})
        endif()
    endif()
endif()

include(GNUInstallDirs)

install(TARGETS TiScannUtility
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(UNIX AND NOT APPLE)
    install(FILES cmake/TiScannUtility.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES cmake/TiScannUtility.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

# ============================================================================
# Unit Tests & CTest Integration
# ============================================================================
enable_testing()

set(TEST_SOURCES
    tests/unit_tests.cpp
    src/PdfAssembler.cpp
    src/PdfAssembler.h
    src/ScannerBackend.h
    ${PLATFORM_SOURCES}
)

add_executable(unit_tests ${TEST_SOURCES})
target_include_directories(unit_tests PRIVATE src)
target_link_libraries(unit_tests PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Core
)

if(APPLE)
    target_link_libraries(unit_tests PRIVATE
        "-framework ImageCaptureCore"
        "-framework CoreGraphics"
    )
elseif(UNIX)
    target_link_libraries(unit_tests PRIVATE
        ${CMAKE_DL_LIBS}
        Threads::Threads
    )
    if(PKG_CONFIG_FOUND)
        if(POPPLER_QT_FOUND)
            target_include_directories(unit_tests PRIVATE ${POPPLER_QT_INCLUDE_DIRS})
            target_link_libraries(unit_tests PRIVATE ${POPPLER_QT_LIBRARIES})
        endif()
        if(QPDF_FOUND)
            target_include_directories(unit_tests PRIVATE ${QPDF_INCLUDE_DIRS})
            target_link_libraries(unit_tests PRIVATE ${QPDF_LIBRARIES})
        endif()
    endif()
endif()

add_test(NAME TiScannUtility_UnitTests COMMAND unit_tests)


