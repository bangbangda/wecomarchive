#!/bin/sh
#
# Runs all test scripts inside the test container (the image's default command).
#
# Env:
#   VALGRIND=1   run the mock-SDK streaming tests under valgrind (image built with WITH_VALGRIND=1)
#

cd "$(dirname "$0")" || exit 1

rc=0

php test.php || rc=1
echo ""

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
else
    php test_mock.php || rc=1
fi
echo ""

php test_integration.php || rc=1

exit $rc
