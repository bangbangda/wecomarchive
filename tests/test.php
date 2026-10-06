<?php
/**
 * WeComArchive Extension Test Suite
 */

require __DIR__ . '/helpers.php';

echo "=== WeComArchive Extension Test Suite ===\n\n";

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
$methods = ['__construct', 'getChatData', 'decryptData', 'decryptChatItem', 'getMediaData', 'saveMediaData', 'saveMediaDataPart', 'getSdkVersion'];
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
    'WECOM_ERR_WRITE' => 20001,
    'WECOM_ERR_MD5' => 20002,
    'WECOM_ERR_PATH' => 20003,
    'WECOM_ERR_TIMEOUT' => 20004,
    'WECOM_ERR_EXEC_TIME' => 20005,
    'WECOM_ERR_INDEXBUF' => 20006,
    'WECOM_ERR_NOT_INIT' => 20007,
    'WECOM_ERR_RESUME' => 20008,
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

// Test 14: extension version
test(
    "Extension version is 1.4.0",
    phpversion('wecomarchive') === '1.4.0',
    "Got: " . var_export(phpversion('wecomarchive'), true)
);

// Test 15: saveMediaData() signature
$method = new ReflectionMethod('WeComArchive', 'saveMediaData');
$params = $method->getParameters();
$signature = array_map(function (ReflectionParameter $p) {
    return $p->getType() . ' $' . $p->getName()
        . ($p->isDefaultValueAvailable() ? ' = ' . var_export($p->getDefaultValue(), true) : '');
}, $params);
test(
    "saveMediaData() signature",
    $method->isPublic() && !$method->isStatic()
        && $method->getNumberOfRequiredParameters() === 2
        && $signature === ['string $sdkFileId', 'string $path', 'array $options = array (' . "\n" . ')']
        && (string)$method->getReturnType() === 'int',
    "Got: (" . implode(', ', $signature) . "): " . $method->getReturnType()
);

// Test 15b: saveMediaDataPart() signature
$method = new ReflectionMethod('WeComArchive', 'saveMediaDataPart');
$signature = array_map(function (ReflectionParameter $p) {
    return $p->getType() . ' $' . $p->getName()
        . ($p->isDefaultValueAvailable() ? ' = ' . var_export($p->getDefaultValue(), true) : '');
}, $method->getParameters());
test(
    "saveMediaDataPart() signature",
    $method->isPublic() && !$method->isStatic()
        && $method->getNumberOfRequiredParameters() === 2
        && $signature === ['string $sdkFileId', 'string $partPath', 'array $options = array (' . "\n" . ')']
        && (string)$method->getReturnType() === 'array',
    "Got: (" . implode(', ', $signature) . "): " . $method->getReturnType()
);

// Tests 16+: saveMediaData() / getMediaData() without network access to real data.
// Uses the real SDK with fake credentials; Init() does not validate them.
$archive = new WeComArchive(['corpid' => 'fake_corpid', 'secret' => 'fake_secret']);
$dir = make_temp_dir('wecomarchive-test');

$e = catch_exception(function () use ($archive, $dir) {
    $archive->saveMediaData('fake', "{$dir}/missing-dir/out.bin");
});
test(
    "saveMediaData() rejects a missing directory with WECOM_ERR_PATH",
    $e && $e->getCode() === WECOM_ERR_PATH && !is_dir("{$dir}/missing-dir"),
    describe_exception($e)
);

$e = catch_exception(function () use ($archive, $dir) {
    $archive->saveMediaData('fake', $dir);
});
test(
    "saveMediaData() rejects a directory as target with WECOM_ERR_PATH",
    $e && $e->getCode() === WECOM_ERR_PATH && leftover_parts($dir) === [],
    describe_exception($e)
);

if (function_exists('posix_mkfifo')) {
    posix_mkfifo("{$dir}/fifo", 0600);
    $e = catch_exception(function () use ($archive, $dir) {
        $archive->saveMediaData('fake', "{$dir}/fifo");
    });
    test(
        "saveMediaData() refuses to replace a non-regular file with WECOM_ERR_PATH",
        $e && $e->getCode() === WECOM_ERR_PATH && filetype("{$dir}/fifo") === 'fifo' && leftover_parts("{$dir}/fifo") === [],
        describe_exception($e)
    );
} else {
    skip("saveMediaData() refuses to replace a non-regular file", "posix extension not loaded");
}

foreach (['php://memory', 'ftp://example.com/out.bin', 'data://text/plain,abc', ''] as $badPath) {
    $e = catch_exception(function () use ($archive, $badPath) {
        $archive->saveMediaData('fake', $badPath);
    });
    test(
        "saveMediaData() rejects non-local path " . var_export($badPath, true),
        $e && $e->getCode() === WECOM_ERR_PATH,
        describe_exception($e)
    );
}

foreach ([['retries' => -1], ['retries' => '2'], ['md5' => 'xyz'], ['md5' => str_repeat('g', 32)]] as $badOptions) {
    $e = catch_exception(function () use ($archive, $dir, $badOptions) {
        $archive->saveMediaData('fake', "{$dir}/opt.bin", $badOptions);
    });
    test(
        "saveMediaData() rejects option " . json_encode($badOptions) . " with WECOM_ERR_PARAM",
        $e && $e->getCode() === WECOM_ERR_PARAM && !file_exists("{$dir}/opt.bin") && leftover_parts("{$dir}/opt.bin") === [],
        describe_exception($e)
    );
}

// open_basedir only tightens at runtime, so check it in a child process
$allowed = "{$dir}/allowed";
$forbidden = "{$dir}/forbidden";
mkdir($allowed);
mkdir($forbidden);

/** Run saveMediaData($target) under open_basedir=$basedir in a child process; returns the exception code or output. */
function save_under_open_basedir(string $basedir, string $target): string {
    $child = <<<'PHP'
$archive = new WeComArchive(['corpid' => 'fake_corpid', 'secret' => 'fake_secret']);
try {
    $archive->saveMediaData('fake', $argv[1], ['retries' => 0]);
    echo "no exception";
} catch (Exception $e) {
    echo $e->getCode();
}
PHP;
    $cmd = escapeshellarg(PHP_BINARY) . ' -d open_basedir=' . escapeshellarg($basedir)
        . ' -r ' . escapeshellarg($child) . ' ' . escapeshellarg($target) . ' 2>&1';
    return trim((string)shell_exec($cmd));
}

$output = save_under_open_basedir($allowed, "{$forbidden}/out.bin");
test(
    "saveMediaData() honours open_basedir with WECOM_ERR_PATH",
    $output === (string)WECOM_ERR_PATH && !file_exists("{$forbidden}/out.bin") && leftover_parts("{$forbidden}/out.bin") === [],
    "Child output: {$output}"
);

// A symlink outside open_basedir pointing inside it: the rename would replace the link outside
file_put_contents("{$allowed}/file.bin", 'keep me');
symlink("{$allowed}/file.bin", "{$forbidden}/link.bin");
$output = save_under_open_basedir($allowed, "{$forbidden}/link.bin");
test(
    "saveMediaData() rejects a symlink outside open_basedir that points inside it",
    $output === (string)WECOM_ERR_PATH && is_link("{$forbidden}/link.bin") && file_get_contents("{$allowed}/file.bin") === 'keep me'
        && leftover_parts("{$forbidden}/link.bin") === [],
    "Child output: {$output}"
);

// open_basedir allowing only the target file itself leaves no room for the temporary file
file_put_contents("{$allowed}/only.bin", 'keep me');
$output = save_under_open_basedir("{$allowed}/only.bin", "{$allowed}/only.bin");
test(
    "saveMediaData() needs open_basedir to allow the target's directory",
    $output === (string)WECOM_ERR_PATH && file_get_contents("{$allowed}/only.bin") === 'keep me' && leftover_parts("{$allowed}/only.bin") === [],
    "Child output: {$output}"
);

// Fake credentials: the SDK's own error code must come through untouched,
// the existing target must survive and no temporary file may be left behind.
$target = "{$dir}/existing.bin";
file_put_contents($target, 'keep me');
$e = catch_exception(function () use ($archive, $target) {
    $archive->saveMediaData('fake_sdkfileid', $target, ['timeout' => 5]);
});
$code = $e ? $e->getCode() : null;
test(
    "saveMediaData() with fake credentials throws the SDK error code",
    is_int($code) && $code > 0 && $code < 20000,
    describe_exception($e)
);
test(
    "saveMediaData() failure leaves the existing target and no temporary file",
    file_get_contents($target) === 'keep me' && leftover_parts($target) === [],
    implode(', ', leftover_parts($target))
);

$e = catch_exception(function () use ($archive, $dir) {
    $archive->saveMediaData('fake_sdkfileid', "{$dir}/new.bin", ['timeout' => 5, 'retries' => 0]);
});
test(
    "saveMediaData() failure creates no target file",
    $e && $e->getCode() > 0 && !file_exists("{$dir}/new.bin") && leftover_parts("{$dir}/new.bin") === [],
    describe_exception($e)
);

$e = catch_exception(function () use ($archive) {
    $archive->getMediaData('fake_sdkfileid', ['timeout' => 5]);
});
test(
    "getMediaData() with fake credentials throws the SDK error code",
    $e && $e->getCode() > 0 && $e->getCode() < 20000,
    describe_exception($e)
);

remove_dir($dir);

finish();
