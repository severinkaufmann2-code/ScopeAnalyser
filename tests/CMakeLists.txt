add_executable(scope_tests
    test_signal_store.cpp
    test_hdf5_session.cpp
    test_formula_engine.cpp
    test_converter_profile.cpp
)

target_link_libraries(scope_tests PRIVATE
    scope::scope_core
    scope::scope_analyser
    scope::scope_converter
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(scope_tests)
