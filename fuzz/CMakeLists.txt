# Fuzz targets. With RAVEL_FUZZ=ON (Clang) they link libFuzzer and the address
# and undefined-behavior sanitizers, and you run them by hand:
#   build/fuzz/fuzz_read_choices -max_total_time=600 fuzz/corpus/read_choices
# Otherwise they link a small driver that just replays the committed corpus, so
# every compiler and every ctest run still exercises the inputs that once
# mattered.
set(RAVEL_FUZZ_TARGETS read_choices sweep_args)

foreach(target IN LISTS RAVEL_FUZZ_TARGETS)
  add_executable(fuzz_${target} fuzz_${target}.cpp)
  target_link_libraries(fuzz_${target} PRIVATE ravel::ravel)
  if(RAVEL_FUZZ)
    target_compile_options(fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
    target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
  else()
    target_sources(fuzz_${target} PRIVATE driver.cpp)
    add_test(NAME fuzz_corpus_${target}
             COMMAND fuzz_${target} ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${target})
  endif()
endforeach()
