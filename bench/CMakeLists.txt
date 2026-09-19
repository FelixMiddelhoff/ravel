# The soak test checks ravel's own promises (replay, determinism) over many
# seeds and doubles as a throughput bench. It reuses the showcase system.
add_executable(ravel_soak soak.cpp)
target_link_libraries(ravel_soak PRIVATE ravel::ravel)
target_include_directories(ravel_soak PRIVATE ${PROJECT_SOURCE_DIR}/examples)

# A short soak run as part of ctest; run ravel_soak by hand with a big seed
# count (say 100000; Raft makes it slow) before a release.
add_test(NAME ravel_soak COMMAND ravel_soak 300)
