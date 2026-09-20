add_executable(ravel_quickstart quickstart.cpp)
target_link_libraries(ravel_quickstart PRIVATE ravel::ravel)
ravel_warnings(ravel_quickstart)

# raft.hpp is shared with the tests, which run it as an end-to-end check.
add_executable(ravel_raft raft.cpp)
target_link_libraries(ravel_raft PRIVATE ravel::ravel)
ravel_warnings(ravel_raft)
