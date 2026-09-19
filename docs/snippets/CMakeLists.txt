# Programs the documentation quotes. They are built and run so the code shown
# in the docs compiles and the output shown is what really comes out;
# tools/check_docs.py verifies both.

function(ravel_snippet name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ravel::ravel)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endfunction()

# Each also runs as a test, in its own scratch directory (some write files).
function(ravel_snippet_test name)
  ravel_snippet(${name})
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/work/${name})
  add_test(NAME snippet_${name} COMMAND ${name}
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/work/${name})
endfunction()

ravel_snippet_test(tutorial_first_test)
ravel_snippet_test(tutorial_deposit)
ravel_snippet_test(tutorial_deposit_fixed)
ravel_snippet_test(tutorial_regression)
ravel_snippet_test(porting_after)
ravel_snippet_test(porting_determinism)
ravel_snippet_test(debugging_examples)

# Shown next to porting_after as what code looks like before; only compiled.
ravel_snippet(porting_before)
