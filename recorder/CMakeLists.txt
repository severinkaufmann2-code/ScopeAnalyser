set(RECORDER_HEADERS
    include/scope/recorder/RecordingSession.h
    include/scope/recorder/NotifyChannel.h
    include/scope/recorder/OversampledChannel.h
    include/scope/recorder/RecorderWidget.h
    ui/SymbolBrowserWidget.h
    ui/ChannelTableWidget.h
    ui/LivePreviewPlot.h
)

set(RECORDER_SOURCES
    src/RecordingSession.cpp
    src/NotifyChannel.cpp
    src/OversampledChannel.cpp
    ui/RecorderWidget.cpp
    ui/SymbolBrowserWidget.cpp
    ui/ChannelTableWidget.cpp
    ui/LivePreviewPlot.cpp
)

scope_add_library(scope_recorder
    SOURCES ${RECORDER_SOURCES}
    HEADERS ${RECORDER_HEADERS}
    PUBLIC_INCLUDES
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/ui
    PUBLIC_LIBS
        scope::scope_core
        scope::scope_ads
        scope::scope_plot
        Qt6::Widgets
        Qt6::Gui
        Qt6::PrintSupport
        qcustomplot::qcustomplot
    PRIVATE_LIBS
        scope::scope_style
        spdlog::spdlog
    FOLDER scope/recorder
)

if(SCOPE_BUILD_STANDALONE_EXES)
    add_executable(scope_recorder_app ui/standalone_main.cpp)
    target_link_libraries(scope_recorder_app PRIVATE scope::scope_recorder scope::scope_style Qt6::Widgets)
    set_target_properties(scope_recorder_app PROPERTIES
        OUTPUT_NAME ScopeRecorder
        WIN32_EXECUTABLE TRUE)
endif()
