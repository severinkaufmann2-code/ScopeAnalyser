add_executable(scope_tests
    test_signal_store.cpp
    test_hdf5_session.cpp
    test_mdf4_session.cpp
    test_formula_engine.cpp
    test_converter_profile.cpp
    test_csv_converter.cpp
    test_csv_writer.cpp
    test_mock_pipeline.cpp
    test_scope_plot.cpp
    test_analyser_layout.cpp
    test_svdx_reader.cpp
)

# Real TwinCAT Scope export used as a decode fixture (tests/data/).
target_compile_definitions(scope_tests PRIVATE
    SVDX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

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
