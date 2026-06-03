cmake_minimum_required(VERSION 3.24)

project(ScopeAnalyser
    VERSION 0.1.0
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
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(SCOPE_BUILD_TESTS "Build unit tests" ON)
option(SCOPE_BUILD_APP "Build integrated shell application" ON)
option(SCOPE_BUILD_STANDALONE_EXES "Build standalone Recorder/Analyser/Converter executables" ON)

# ---- System packages (from apt on Linux, from vcpkg or installers on Windows) -
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui PrintSupport Concurrent Test)
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
FetchContent_Declare(HighFive
    GIT_REPOSITORY https://github.com/BlueBrain/HighFive.git
    GIT_TAG        v2.10.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(HighFive)

# exprtk — single-header expression parser
FetchContent_Declare(exprtk
    GIT_REPOSITORY https://github.com/ArashPartow/exprtk.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)
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
target_compile_definitions(qcustomplot PUBLIC QCUSTOMPLOT_USE_LIBRARY)
add_library(qcustomplot::qcustomplot ALIAS qcustomplot)

# QXlsx — converter dependency
set(QXlsx_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(qxlsx
    GIT_REPOSITORY https://github.com/QtExcel/QXlsx.git
    GIT_TAG        v1.4.7
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  QXlsx)
FetchContent_MakeAvailable(qxlsx)

# Beckhoff/ADS — open-source ADS-over-TCP client (Linux + Windows).
FetchContent_Declare(beckhoff_ads
    GIT_REPOSITORY https://github.com/Beckhoff/ADS.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(beckhoff_ads)

include(cmake/scope_helpers.cmake)

add_subdirectory(core)
add_subdirectory(ads)
add_subdirectory(recorder)
add_subdirectory(analyser)
add_subdirectory(converter)

if(SCOPE_BUILD_APP)
    add_subdirectory(app)
endif()

if(SCOPE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
