add_executable(ravel_tests
  test_main.cpp
  test_network.cpp
  test_quorum_register.cpp
  test_rng.cpp
  test_runner.cpp
  test_scheduler.cpp
  test_shrink.cpp
  test_simulation.cpp
  test_trace.cpp
)
target_link_libraries(ravel_tests PRIVATE ravel::ravel)
target_include_directories(ravel_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples)
add_test(NAME ravel_tests COMMAND ravel_tests)
