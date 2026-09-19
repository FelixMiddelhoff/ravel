add_executable(ravel_quickstart quickstart.cpp)
target_link_libraries(ravel_quickstart PRIVATE ravel::ravel)

# Header shared with the tests, which run it as an end-to-end check.
add_executable(ravel_quorum_register quorum_register.cpp)
target_link_libraries(ravel_quorum_register PRIVATE ravel::ravel)
