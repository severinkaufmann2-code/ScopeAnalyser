set(STYLE_HEADERS
    include/scope/style/Theme.h
    include/scope/style/StyleKit.h
)

set(STYLE_SOURCES
    src/Theme.cpp
    src/StyleKit.cpp
)

scope_add_library(scope_style
    SOURCES ${STYLE_SOURCES}
    HEADERS ${STYLE_HEADERS}
    PUBLIC_INCLUDES
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PUBLIC_LIBS
        Qt6::Widgets
        Qt6::Gui
    FOLDER scope/style
)
