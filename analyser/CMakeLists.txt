set(ANALYSER_HEADERS
    include/scope/analyser/FormulaEngine.h
    include/scope/analyser/FunctionRegistry.h
    include/scope/analyser/AnalyserWidget.h
    ui/AnalyserPlot.h
)

set(ANALYSER_SOURCES
    src/FormulaEngine.cpp
    src/FunctionRegistry.cpp
    ui/AnalyserWidget.cpp
    ui/AnalyserPlot.cpp
)

scope_add_library(scope_analyser
    SOURCES ${ANALYSER_SOURCES}
    HEADERS ${ANALYSER_HEADERS}
    PUBLIC_INCLUDES
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/ui
    PUBLIC_LIBS
        scope::scope_core
        scope::scope_plot
        Qt6::Widgets
        Qt6::Gui
        Qt6::PrintSupport
        qcustomplot::qcustomplot
    PRIVATE_LIBS
        spdlog::spdlog
    FOLDER scope/analyser
)

if(SCOPE_BUILD_STANDALONE_EXES)
    add_executable(scope_analyser_app ui/standalone_main.cpp)
    target_link_libraries(scope_analyser_app PRIVATE scope::scope_analyser Qt6::Widgets)
    set_target_properties(scope_analyser_app PROPERTIES
        OUTPUT_NAME ScopeAnalyserStandalone
        WIN32_EXECUTABLE TRUE)
endif()
