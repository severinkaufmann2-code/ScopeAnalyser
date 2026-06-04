set(PLOT_HEADERS
    include/scope/plot/ScopePlot.h
)

set(PLOT_SOURCES
    src/ScopePlot.cpp
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
    FOLDER scope/plot
)
