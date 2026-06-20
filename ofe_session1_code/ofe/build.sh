#!/usr/bin/env bash
# ============================================================
#  OFE Build & Test Commands — run sections individually
#  Working directory: ofe_session1_code/ofe/
#
#  Build directories:
#    build/       Debug   (ASan + UBSan — catches memory bugs)
#    build_rel/   Release (O3, LTO, stripped — perf testing)
#    build_test/  Debug   (same as build/ — dedicated test run dir)
# ============================================================

set -euo pipefail
OFE_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── 1. CONFIGURE ─────────────────────────────────────────────
# Run once after a clean checkout or after changing CMakeLists.txt.
# Downloads googletest and yaml-cpp via FetchContent (needs internet first time).

configure_debug() {
    cmake -S "$OFE_DIR" -B "$OFE_DIR/build" \
          -DCMAKE_BUILD_TYPE=Debug
}

configure_release() {
    cmake -S "$OFE_DIR" -B "$OFE_DIR/build_rel" \
          -DCMAKE_BUILD_TYPE=Release
}

configure_test() {
    cmake -S "$OFE_DIR" -B "$OFE_DIR/build_test" \
          -DCMAKE_BUILD_TYPE=Debug
}

# ── 2. BUILD ──────────────────────────────────────────────────
# Incremental — only recompiles changed files.

build_debug() {
    cmake --build "$OFE_DIR/build" -j"$(nproc)"
}

build_release() {
    cmake --build "$OFE_DIR/build_rel" -j"$(nproc)"
}

build_test() {
    cmake --build "$OFE_DIR/build_test" -j"$(nproc)"
}

# ── 3. CLEAN BUILD ────────────────────────────────────────────
# Full rebuild from scratch (keeps downloaded deps).

clean_build_debug() {
    cmake --build "$OFE_DIR/build" --target clean
    cmake --build "$OFE_DIR/build" -j"$(nproc)"
}

clean_build_test() {
    cmake --build "$OFE_DIR/build_test" --target clean
    cmake --build "$OFE_DIR/build_test" -j"$(nproc)"
}

# ── 4. RUN ALL TESTS ─────────────────────────────────────────
# Prints PASSED/FAILED summary.  --output-on-failure shows the
# full gtest output only for failing tests.

run_tests() {
    mkdir -p "$OFE_DIR/test_logs"
    ctest --test-dir "$OFE_DIR/build_test" \
          --output-on-failure \
          2>&1 | tee "$OFE_DIR/test_logs/last_run.log"
}

# ── 5. RUN TEST BINARY DIRECTLY ──────────────────────────────
# Gives full gtest output with colour for every test.

run_tests_verbose() {
    mkdir -p "$OFE_DIR/test_logs"
    "$OFE_DIR/build_test/ofe_tests" \
          --gtest_color=yes \
          2>&1 | tee "$OFE_DIR/test_logs/last_run_verbose.log"
}

# ── 6. RUN A SINGLE TEST SUITE ───────────────────────────────
# Pass the suite name as argument, e.g.:
#   ./build.sh run_suite TickRecord
#   ./build.sh run_suite RingBuffer
#   ./build.sh run_suite LeeReady

run_suite() {
    local suite="${1:?Usage: run_suite <SuiteName>}"
    mkdir -p "$OFE_DIR/test_logs"
    "$OFE_DIR/build_test/ofe_tests" \
          --gtest_filter="${suite}*" \
          --gtest_color=yes \
          2>&1 | tee "$OFE_DIR/test_logs/suite_${suite}.log"
}

# ── 7. RUN A SINGLE TEST CASE ────────────────────────────────
# Pass full test name, e.g.:
#   ./build.sh run_one TickRecord.SizeIs64Bytes

run_one() {
    local test="${1:?Usage: run_one <Suite.TestName>}"
    "$OFE_DIR/build_test/ofe_tests" \
          --gtest_filter="$test" \
          --gtest_color=yes
}

# ── 8. BUILD + TEST (one-shot workflow) ───────────────────────
build_and_test() {
    build_test
    run_tests_verbose
}

# ── 9. WIPE AND RECONFIGURE (nuclear option) ─────────────────
# Use when CMake cache is stale or you change toolchain.

nuke_and_reconfigure() {
    rm -rf "$OFE_DIR/build" "$OFE_DIR/build_rel" "$OFE_DIR/build_test"
    configure_debug
    configure_release
    configure_test
}

# ── DISPATCH ─────────────────────────────────────────────────
# Allows:  ./build.sh build_test
#          ./build.sh run_tests
#          ./build.sh run_suite LeeReady
#          ./build.sh build_and_test

if [[ $# -gt 0 ]]; then
    "$@"
else
    echo "Usage: $0 <command> [args]"
    echo ""
    echo "Commands:"
    echo "  configure_debug         cmake configure (Debug, ASan+UBSan)"
    echo "  configure_release       cmake configure (Release, O3+LTO)"
    echo "  configure_test          cmake configure (Debug, test dir)"
    echo "  build_debug             incremental debug build"
    echo "  build_release           incremental release build"
    echo "  build_test              incremental test build"
    echo "  clean_build_debug       clean then rebuild debug"
    echo "  clean_build_test        clean then rebuild tests"
    echo "  run_tests               ctest --output-on-failure (logs to test_logs/)"
    echo "  run_tests_verbose       ./ofe_tests --gtest_color (full output)"
    echo "  run_suite  <Name>       run one gtest suite  e.g. LeeReady"
    echo "  run_one    <Suite.Test> run one gtest case"
    echo "  build_and_test          build_test + run_tests_verbose"
    echo "  nuke_and_reconfigure    wipe all build dirs and reconfigure"
fi
