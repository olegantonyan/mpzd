add_executable(unit_tests unit_tests.c)
target_link_libraries(unit_tests PRIVATE mpzdlinux)
add_test(NAME unit_tests COMMAND unit_tests)

add_executable(engine_tests engine_tests.c ${PROJECT_SOURCE_DIR}/src/linux/wake.c)
target_link_libraries(engine_tests PRIVATE mpzdcore)
add_test(NAME engine_tests COMMAND engine_tests)
