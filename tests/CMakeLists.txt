add_executable(scope_tests
    test_signal_store.cpp
    test_hdf5_session.cpp
    test_mdf4_session.cpp
    test_formula_engine.cpp
    test_function_correctness.cpp
    test_converter_profile.cpp
    test_csv_converter.cpp
    test_csv_writer.cpp
    test_json_converter.cpp
    test_json_writer.cpp
    test_html_export.cpp
    test_mock_pipeline.cpp
    test_scope_plot.cpp
    test_analyser_layout.cpp
    test_ads_array_expansion.cpp
    test_ads_type_table.cpp
    test_ads_symbol_types.cpp
    test_app_icon.cpp
    test_save_chart_dialog.cpp
    test_symbol_browser.cpp
    test_undo_stack.cpp
)

# test_analyser_layout.cpp drives AnalyserPlot directly; its header lives in
# the analyser target's private ui/ dir, so expose it to the test target.
target_include_directories(scope_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/analyser/ui
    ${CMAKE_SOURCE_DIR}/recorder/ui)

# test_app_icon.cpp reads the committed resources/ScopeAnalyser.ico.
target_compile_definitions(scope_tests PRIVATE
    SCOPE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(scope_tests PRIVATE
    scope::scope_core
    scope::scope_ads
    scope::scope_plot
    scope::scope_recorder
    scope::scope_analyser
    scope::scope_converter
    scope::scope_style
    Qt6::Test
    Qt6::Core
    Qt6::Widgets
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(scope_tests)

# The exported HTML chart's zoom / pan has to match the Analyser's ScopePlot.
# That logic lives in scope_page.js, so it's checked by running the page
# script under node against a DOM/uPlot stub. Skipped where node isn't
# installed — it's the only part of the suite that needs it.
find_program(SCOPE_NODE_EXECUTABLE NAMES node nodejs)
if(SCOPE_NODE_EXECUTABLE)
    add_test(NAME scope_page_js
             COMMAND ${SCOPE_NODE_EXECUTABLE}
                     ${CMAKE_CURRENT_SOURCE_DIR}/js/scope_page_interaction.test.js)
else()
    message(STATUS "node not found — skipping the scope_page.js interaction test")
endif()
