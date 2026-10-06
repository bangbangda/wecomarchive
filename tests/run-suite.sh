#!/bin/sh
#
# Runs all test scripts inside the test container (the image's default command).
#
# Env:
#   VALGRIND=1            run the mock-SDK streaming tests and the phpt tests under valgrind
#                         (image built with WITH_VALGRIND=1)
#   WECOM_MOCK_LARGE_MB   size of the large-file memory tests (default 300 for test_mock.php and
#                         1024 for tests/phpt/large_file_memory.phpt; 8 under valgrind; 0 skips)
#

cd "$(dirname "$0")" || exit 1

rc=0

php test.php || rc=1
echo ""

# run-tests.php is copied next to the sources by phpize; fall back to PHP's own copy
RUN_TESTS="../run-tests.php"
if [ ! -f "$RUN_TESTS" ]; then
    RUN_TESTS="$(php-config --prefix)/lib/php/build/run-tests.php"
fi
PHP_BIN="$(command -v php)"
run_phpt() {
    php -n -d open_basedir= -d output_buffering=0 -d memory_limit=-1 "$RUN_TESTS" -q -p "$PHP_BIN" --show-diff "$@" phpt
}

if [ "${VALGRIND:-0}" = "1" ]; then
    echo "Running streaming tests under valgrind (USE_ZEND_ALLOC=0)..."
    USE_ZEND_ALLOC=0 ZEND_DONT_UNLOAD_MODULES=1 WECOM_MOCK_LARGE_MB="${WECOM_MOCK_LARGE_MB:-8}" \
        valgrind --leak-check=full --show-leak-kinds=definite,indirect \
        --errors-for-leak-kinds=definite,indirect --error-exitcode=99 \
        --log-file=/tmp/valgrind.log php test_mock.php
    vg_rc=$?
    echo ""
    echo "--- valgrind summary ---"
    grep -E 'ERROR SUMMARY|definitely lost|indirectly lost|possibly lost' /tmp/valgrind.log
    if grep -q 'wecomarchive' /tmp/valgrind.log; then
        echo "--- valgrind entries mentioning wecomarchive ---"
        grep -B2 -A12 'wecomarchive' /tmp/valgrind.log | head -200
    fi
    [ "$vg_rc" = "0" ] || rc=1
    echo ""

    echo "Running phpt tests under valgrind (run-tests.php -m)..."
    WECOM_MOCK_LARGE_MB="${WECOM_MOCK_LARGE_MB:-8}" run_phpt -m || rc=1
else
    php test_mock.php || rc=1
    echo ""

    echo "Running phpt tests..."
    run_phpt || rc=1
fi
echo ""

php test_integration.php || rc=1

exit $rc
