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

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui PrintSupport Concurrent)
find_package(HDF5 CONFIG REQUIRED COMPONENTS CXX)
find_package(HighFive CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-concurrentqueue CONFIG REQUIRED)
find_package(exprtk CONFIG REQUIRED)
find_package(QXlsx CONFIG REQUIRED)

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
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()
