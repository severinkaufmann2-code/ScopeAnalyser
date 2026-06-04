add_executable(scope_tests
    test_signal_store.cpp
    test_hdf5_session.cpp
    test_formula_engine.cpp
    test_converter_profile.cpp
    test_csv_converter.cpp
    test_csv_writer.cpp
    test_mock_pipeline.cpp
    test_scope_plot.cpp
)

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
