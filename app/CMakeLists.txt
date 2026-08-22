add_executable(ScopeAnalyser
    main.cpp
    ShellWindow.cpp
    ShellWindow.h
    ${SCOPE_APP_ICON_RC}
)

target_link_libraries(ScopeAnalyser PRIVATE
    scope::scope_core
    scope::scope_style
    scope::scope_recorder
    scope::scope_analyser
    scope::scope_converter
    Qt6::Widgets
    Qt6::Gui
    spdlog::spdlog
)

set_target_properties(ScopeAnalyser PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE)
