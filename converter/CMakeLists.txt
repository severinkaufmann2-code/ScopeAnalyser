set(CONVERTER_HEADERS
    include/scope/converter/IConverterSource.h
    include/scope/converter/ConverterProfile.h
    include/scope/converter/ExcelSource.h
    include/scope/converter/CsvSource.h
    include/scope/converter/CsvWriter.h
    include/scope/converter/SignalIO.h
    include/scope/converter/SaveChartDialog.h
    include/scope/converter/BusyRunner.h
    include/scope/converter/ConverterWidget.h
    ui/MappingPanel.h
    ui/ChannelEditDialog.h
)

set(CONVERTER_SOURCES
    src/ConverterProfile.cpp
    src/ExcelSource.cpp
    src/CsvSource.cpp
    src/CsvWriter.cpp
    src/SignalIO.cpp
    ui/ConverterWidget.cpp
    ui/MappingPanel.cpp
    ui/ChannelEditDialog.cpp
    ui/SaveChartDialog.cpp
)

scope_add_library(scope_converter
    SOURCES ${CONVERTER_SOURCES}
    HEADERS ${CONVERTER_HEADERS}
    PUBLIC_INCLUDES
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/ui
    PUBLIC_LIBS
        scope::scope_core
        Qt6::Widgets
        Qt6::Gui
        Qt6::Concurrent
    PRIVATE_LIBS
        scope::scope_style
        spdlog::spdlog
        nlohmann_json::nlohmann_json
    FOLDER scope/converter
)

if(SCOPE_BUILD_STANDALONE_EXES)
    add_executable(scope_converter_app ui/standalone_main.cpp)
    target_link_libraries(scope_converter_app PRIVATE scope::scope_converter scope::scope_style Qt6::Widgets)
    set_target_properties(scope_converter_app PROPERTIES
        OUTPUT_NAME ScopeConverter
        WIN32_EXECUTABLE TRUE)
endif()
