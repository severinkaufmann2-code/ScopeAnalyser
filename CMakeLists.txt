cmake_minimum_required(VERSION 3.24)

project(ScopeAnalyser
    VERSION 0.5.3
    DESCRIPTION "Signal recording, analysis, and data conversion for TwinCAT ADS"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
    # <windows.h> (pulled in by the Beckhoff ADS lib) defines min/max macros
    # that clobber std::min/std::max; NOMINMAX disables them. WIN32_LEAN_AND_MEAN
    # trims the header and avoids the winsock.h/winsock2.h clash.
    # _USE_MATH_DEFINES makes <cmath> expose M_PI etc. (MSVC omits them by
    # default, unlike glibc).
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(SCOPE_BUILD_TESTS "Build unit tests" ON)
option(SCOPE_BUILD_APP "Build integrated shell application" ON)
option(SCOPE_BUILD_STANDALONE_EXES "Build standalone Recorder/Analyser/Converter executables" ON)

# ---- System packages (from apt on Linux, from vcpkg or installers on Windows) -
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui PrintSupport Concurrent Test)
# SVG chart export is optional at build time (CI's Qt always has it, so
# releases do too). Locally: apt install qt6-svg-dev to enable.
find_package(Qt6 COMPONENTS Svg QUIET)
if(Qt6Svg_FOUND)
    message(STATUS "Qt6Svg found — SVG chart export enabled")
else()
    message(STATUS "Qt6Svg not found — building without SVG chart export")
endif()
find_package(HDF5 REQUIRED COMPONENTS CXX)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

# ---- Small / header-only deps fetched at configure time --------------------
include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

# moodycamel ConcurrentQueue — single-header
FetchContent_Declare(concurrentqueue
    GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
    GIT_TAG        v1.0.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(concurrentqueue)

# HighFive — header-only C++ wrapper for HDF5
set(HIGHFIVE_USE_BOOST OFF CACHE BOOL "" FORCE)
set(HIGHFIVE_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(HIGHFIVE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HIGHFIVE_BUILD_DOCS OFF CACHE BOOL "" FORCE)
# The C++20-concepts code path (only for prettier errors) is broken on MSVC:
# the PropertyInterface concept calls the private Property::apply, which MSVC's
# concept evaluation rejects, so DataSetCreateProps::add() finds no overload.
# Disable it to use the SFINAE fallback, which compiles on every platform.
set(HIGHFIVE_HAS_CONCEPTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(HighFive
    GIT_REPOSITORY https://github.com/BlueBrain/HighFive.git
    GIT_TAG        v2.10.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(HighFive)

# exprtk — single-header expression parser.
# Pinned to a commit, not master: upstream rewrites history on that branch, so
# a shallow clone that already exists diverges and FetchContent's update step
# dies mid-rebase ("You will have to resolve the conflicts manually"). No
# GIT_SHALLOW — a shallow fetch of a bare commit hash isn't universally
# supported.
FetchContent_Declare(exprtk
    GIT_REPOSITORY https://github.com/ArashPartow/exprtk.git
    GIT_TAG        1e4a80b5ec9b4832ed59c6faa65f625a01b18ef0)
FetchContent_MakeAvailable(exprtk)
add_library(exprtk INTERFACE)
target_include_directories(exprtk INTERFACE ${exprtk_SOURCE_DIR})

# QCustomPlot — drop-in widget; we vendor the source via FetchContent and
# build it as a small static lib here.
FetchContent_Declare(qcustomplot
    URL https://www.qcustomplot.com/release/2.1.1/QCustomPlot-source.tar.gz)
FetchContent_MakeAvailable(qcustomplot)
add_library(qcustomplot STATIC ${qcustomplot_SOURCE_DIR}/qcustomplot.cpp)
target_include_directories(qcustomplot PUBLIC ${qcustomplot_SOURCE_DIR})
target_link_libraries(qcustomplot PUBLIC Qt6::Widgets Qt6::PrintSupport Qt6::Gui)
# Do NOT define QCUSTOMPLOT_USE_LIBRARY: that is for consuming a *shared*
# QCustomPlot and expands QCP_LIB_DECL to __declspec(dllimport), which on MSVC
# makes compiling the static lib's own .cpp fail (C2491, "definition of
# dllimport static data member"). On Linux the macro is empty, so it was a
# latent Windows-only bug. With it undefined the decl is empty and the static
# lib builds on every platform.
add_library(qcustomplot::qcustomplot ALIAS qcustomplot)

# QXlsx is added in Phase 4 when the Excel converter source is implemented.
# v1.4.x / v1.5.x require Qt6::GuiPrivate which isn't packaged on Ubuntu 25.10
# without building Qt from source. We'll re-evaluate (QXlsx fork, alternative
# xlsx lib, or Qt-from-source) when we get there.

# Beckhoff/ADS — open-source ADS-over-TCP client (Linux + Windows).
# Force a static build: ADS defaults to BUILD_SHARED_LIBS=ON but has no
# dllexport markup, so on Windows the DLL ships no import lib and our exes
# fail to link (LNK1104: cannot open ads.lib). Setting this before the
# subproject also stops its option(BUILD_SHARED_LIBS ON) from leaking out.
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(beckhoff_ads
    GIT_REPOSITORY https://github.com/Beckhoff/ADS.git
    # Pinned, not master: upstream's newer CMakeLists reads
    # ${CMAKE_SOURCE_DIR}/debian/changelog to derive its version, and under
    # FetchContent that resolves to OUR project root, so configure aborts with
    # "Unable to extract AdsLib version". Same failure mode as the mdflib pin
    # below. This commit is the one v0.3.0 shipped against. Re-evaluate when
    # upstream derives its version without reaching into the parent project.
    GIT_TAG        67886519fcec58546be316b3846b1edcb2c829f5
    # ADS unconditionally adds AdsTool / AdsLibTest / example subdirs we don't
    # use; on Windows their executables fail to link (cannot find ads.lib).
    # EXCLUDE_FROM_ALL builds only the `ads` library our code links.
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(beckhoff_ads)

# mdflib — open-source C++ library for ASAM MDF 4.x (.mf4) read/write.
# Disable everything we don't need: tool/viewer (WxWidgets), tests, docs,
# the Python module, and the .NET / C language-binding shared library
# (mdflibrary), which is a lot of extra code we never link.
set(MDF_BUILD_TOOL          OFF CACHE BOOL "" FORCE)
set(MDF_BUILD_TEST          OFF CACHE BOOL "" FORCE)
set(MDF_BUILD_DOC           OFF CACHE BOOL "" FORCE)
set(MDF_BUILD_PYTHON        OFF CACHE BOOL "" FORCE)
set(MDF_BUILD_GTEST         OFF CACHE BOOL "" FORCE)
set(MDF_BUILD_SHARED_LIB    OFF CACHE BOOL "" FORCE)
set(MDF_BUILD_SHARED_LIB_NET OFF CACHE BOOL "" FORCE)
set(MDF_BUILD_SHARED_LIB_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mdflib
    GIT_REPOSITORY https://github.com/ihedvall/mdflib.git
    # Pinned past v2.3.0 for the <cstdint> include fix in itimestamp.h, and
    # before commit 223a531 which broke FetchContent integration by switching
    # ZLIB/EXPAT discovery from find_package() to include(${CMAKE_SOURCE_DIR}
    # /script/zlib.cmake) — that path doesn't exist in our project root.
    # Re-evaluate when a tag past 223a531 lands with that issue resolved.
    GIT_TAG        110209aa154f88dde158936f5a3e90de4f4677fa)
FetchContent_MakeAvailable(mdflib)

include(cmake/scope_helpers.cmake)

# Application icon. The window/taskbar icon is set in code (StyleKit
# appIcon()), but the icon Explorer shows for the .exe FILE can only come from
# a linked Win32 resource. That needs the RC language enabled explicitly:
# every preset and both CI workflows use the Ninja generator, which — unlike
# the Visual Studio generator — does not turn RC on implicitly. Enabled here,
# before the subdirectories, so the standalone Recorder / Analyser / Converter
# executables can carry it as well. Non-Windows builds get an empty string,
# which add_executable() ignores.
if(WIN32)
    enable_language(RC)
    set(SCOPE_APP_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/resources/ScopeAnalyser.rc")
else()
    set(SCOPE_APP_ICON_RC "")
endif()

add_subdirectory(core)
add_subdirectory(style)
add_subdirectory(plot)
add_subdirectory(ads)
add_subdirectory(recorder)
add_subdirectory(analyser)
add_subdirectory(converter)

if(SCOPE_BUILD_APP)
    add_subdirectory(app)
endif()

# Read-only diagnostic tools (ads_dt_probe). Tiny, and shipping them in the
# CI artifact is what makes it possible to capture PLC ground truth from a
# machine with no toolchain.
add_subdirectory(tools)

if(SCOPE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
