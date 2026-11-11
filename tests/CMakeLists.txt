add_executable(unit_tests unit_tests.c)
target_link_libraries(unit_tests PRIVATE mpzdcore)
add_test(NAME unit_tests COMMAND unit_tests)
