<?php
/**
 * WeComArchive Extension Test Suite
 */

echo "=== WeComArchive Extension Test Suite ===\n\n";

$passed = 0;
$failed = 0;

function test($name, $condition, $message = '') {
    global $passed, $failed;
    if ($condition) {
        echo "[PASS] {$name}\n";
        $passed++;
    } else {
        echo "[FAIL] {$name}" . ($message ? " - {$message}" : "") . "\n";
        $failed++;
    }
}

// Test 1: Extension loaded
test(
    "Extension loaded",
    extension_loaded('wecomarchive'),
    "wecomarchive extension not loaded"
);

// Test 2: Class exists
test(
    "WeComArchive class exists",
    class_exists('WeComArchive'),
    "WeComArchive class not found"
);

// Test 3: Check methods exist
$methods = ['__construct', 'getChatData', 'decryptData', 'decryptChatItem', 'getMediaData', 'getSdkVersion'];
foreach ($methods as $method) {
    test(
        "Method {$method}() exists",
        method_exists('WeComArchive', $method),
        "Method {$method} not found"
    );
}

// Test 4: Static method getSdkVersion
test(
    "getSdkVersion() returns string",
    is_string(WeComArchive::getSdkVersion()),
    "getSdkVersion did not return a string"
);

$version = WeComArchive::getSdkVersion();
test(
    "SDK version format valid",
    preg_match('/^v\d+_\d+$/', $version) === 1,
    "Got: {$version}"
);

// Test 5: Error constants defined
$constants = [
    'WECOM_ERR_PARAM' => 10000,
    'WECOM_ERR_NETWORK' => 10001,
    'WECOM_ERR_PARSE' => 10002,
    'WECOM_ERR_SYSTEM' => 10003,
    'WECOM_ERR_ENCRYPT' => 10004,
    'WECOM_ERR_FILEID' => 10005,
    'WECOM_ERR_DECRYPT' => 10006,
    'WECOM_ERR_PRIKEY' => 10007,
    'WECOM_ERR_ENCKEY' => 10008,
    'WECOM_ERR_IP' => 10009,
    'WECOM_ERR_EXPIRED' => 10010,
    'WECOM_ERR_CERT' => 10011,
];

foreach ($constants as $name => $expected) {
    test(
        "Constant {$name} defined",
        defined($name) && constant($name) === $expected,
        defined($name) ? "Expected {$expected}, got " . constant($name) : "Not defined"
    );
}

// Test 6: Constructor requires array
try {
    new WeComArchive("invalid");
    test("Constructor rejects non-array", false, "Should have thrown exception");
} catch (TypeError $e) {
    test("Constructor rejects non-array", true);
} catch (Exception $e) {
    test("Constructor rejects non-array", false, $e->getMessage());
}

// Test 7: Constructor requires corpid
try {
    new WeComArchive([]);
    test("Constructor requires corpid", false, "Should have thrown exception");
} catch (Exception $e) {
    test(
        "Constructor requires corpid",
        strpos($e->getMessage(), 'corpid') !== false,
        $e->getMessage()
    );
}

// Test 8: Constructor requires secret
try {
    new WeComArchive(['corpid' => 'test']);
    test("Constructor requires secret", false, "Should have thrown exception");
} catch (Exception $e) {
    test(
        "Constructor requires secret",
        strpos($e->getMessage(), 'secret') !== false,
        $e->getMessage()
    );
}

// Test 9: INI setting exists
$ini = ini_get('wecomarchive.sdk_lib_path');
test(
    "INI setting wecomarchive.sdk_lib_path exists",
    $ini !== false && strlen($ini) > 0,
    "Got: " . var_export($ini, true)
);

// Test 10: private_keys validation — wrong type
try {
    new WeComArchive([
        'corpid' => 'test', 'secret' => 'test',
        'private_keys' => 'not-an-array',
    ]);
    test("private_keys type check rejects string", false, "Should have thrown");
} catch (Exception $e) {
    test(
        "private_keys type check rejects string",
        strpos($e->getMessage(), 'private_keys') !== false,
        $e->getMessage()
    );
}

// Test 11: private_keys validation — empty array
try {
    new WeComArchive([
        'corpid' => 'test', 'secret' => 'test',
        'private_keys' => [],
    ]);
    test("private_keys rejects empty array", false, "Should have thrown");
} catch (Exception $e) {
    test(
        "private_keys rejects empty array",
        strpos($e->getMessage(), 'empty') !== false,
        $e->getMessage()
    );
}

// Test 12: private_key as nonexistent file path → clear error
try {
    new WeComArchive([
        'corpid' => 'test', 'secret' => 'test',
        'private_key' => '/nonexistent/path/to/key.pem',
    ]);
    test("private_key nonexistent path rejected", false, "Should have thrown");
} catch (Exception $e) {
    test(
        "private_key nonexistent path rejected",
        strpos($e->getMessage(), 'private_key') !== false,
        $e->getMessage()
    );
}

// Test 13: private_keys[ver] with bad path → error mentions version
try {
    new WeComArchive([
        'corpid' => 'test', 'secret' => 'test',
        'private_keys' => [42 => '/nonexistent/key.pem'],
    ]);
    test("private_keys bad path mentions version", false, "Should have thrown");
} catch (Exception $e) {
    test(
        "private_keys bad path mentions version",
        strpos($e->getMessage(), '42') !== false,
        $e->getMessage()
    );
}

// Summary
echo "\n=== Test Summary ===\n";
echo "Passed: {$passed}\n";
echo "Failed: {$failed}\n";
echo "Total:  " . ($passed + $failed) . "\n";

exit($failed > 0 ? 1 : 0);
