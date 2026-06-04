set(PLOT_HEADERS
    include/scope/plot/ScopePlot.h
    include/scope/plot/PlotLayout.h
)

set(PLOT_SOURCES
    src/ScopePlot.cpp
    src/PlotLayout.cpp
)

scope_add_library(scope_plot
    SOURCES ${PLOT_SOURCES}
    HEADERS ${PLOT_HEADERS}
    PUBLIC_INCLUDES
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PUBLIC_LIBS
        Qt6::Widgets
        Qt6::Gui
        Qt6::PrintSupport
        qcustomplot::qcustomplot
    PRIVATE_LIBS
        nlohmann_json::nlohmann_json
    FOLDER scope/plot
)
