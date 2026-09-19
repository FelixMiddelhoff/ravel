add_executable(ravel_tests
  test_disk.cpp
  test_disk_names.cpp
  test_main.cpp
  test_network.cpp
  test_raft.cpp
  test_rng.cpp
  test_runner.cpp
  test_scheduler.cpp
  test_shrink.cpp
  test_shrink_passes.cpp
  test_simulation.cpp
  test_timeouts.cpp
  test_trace.cpp
)
target_link_libraries(ravel_tests PRIVATE ravel::ravel)
target_include_directories(ravel_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples)
add_test(NAME ravel_tests COMMAND ravel_tests)
