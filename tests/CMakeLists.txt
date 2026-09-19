add_executable(ravel_tests
  test_main.cpp
  test_network.cpp
  test_rng.cpp
  test_scheduler.cpp
  test_simulation.cpp
)
target_link_libraries(ravel_tests PRIVATE ravel::ravel)
add_test(NAME ravel_tests COMMAND ravel_tests)
