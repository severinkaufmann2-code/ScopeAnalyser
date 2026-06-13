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
    test_undo_stack.cpp
)

# test_analyser_layout.cpp drives AnalyserPlot directly; its header lives in
# the analyser target's private ui/ dir, so expose it to the test target.
target_include_directories(scope_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/analyser/ui)

target_link_libraries(scope_tests PRIVATE
    scope::scope_core
    scope::scope_ads
    scope::scope_plot
    scope::scope_recorder
    scope::scope_analyser
    scope::scope_converter
    Qt6::Test
    Qt6::Core
    Qt6::Widgets
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(scope_tests)
