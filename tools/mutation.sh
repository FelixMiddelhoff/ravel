#!/usr/bin/env bash
# Mutation testing with Mull (https://mull.readthedocs.io). Needs clang and the
# Mull package for the same LLVM major version; MULL_ROOT is where its usr/ tree is.
#   MULL_ROOT=$HOME/mull/usr tools/mutation.sh [build-dir]
# Optional: SANITIZE=address|thread builds and runs the tests under that sanitizer;
# WORKERS=n sets the number of Mull workers (default: all cores).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
mull="${MULL_ROOT:-/usr}"
build="${1:-$HOME/ravel-mutation-build}"
san="${SANITIZE:+-fsanitize=$SANITIZE -fno-omit-frame-pointer}"
llvm="$(clang --version | sed -n 's/.*version \([0-9]*\)\..*/\1/p')"
export LD_LIBRARY_PATH="$mull/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

cmake -S "$root" -B "$build" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DRAVEL_BUILD_BENCH=OFF -DRAVEL_BUILD_EXAMPLES=OFF -DRAVEL_BUILD_FUZZ=OFF \
  -DCMAKE_CXX_FLAGS="-O0 -g -grecord-command-line -fpass-plugin=$mull/lib/mull-ir-frontend-$llvm $san" \
  -DCMAKE_EXE_LINKER_FLAGS="$san" >/dev/null
mkdir -p "$build"
cp "$root/mull.yml" "$build/mull.yml"  # the compiler plugin reads it from the build dir
cmake --build "$build" -j"$(nproc)" --target ravel_tests
cd "$build"
"$mull/bin/mull-runner-$llvm" --workers "${WORKERS:-$(nproc)}" --allow-surviving \
  --reporters IDE --reporters Elements --report-dir "$build/mutation" \
  --ld-search-path "$mull/lib" ./tests/ravel_tests "${@:2}"
