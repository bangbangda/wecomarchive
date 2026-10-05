<?php
/**
 * Streaming tests for getMediaData() / saveMediaData() against the mock SDK
 * (tests/mock-sdk/mock_sdk.c), which serves local files chunk by chunk.
 *
 * Env:
 *   WECOM_MOCK_SDK       path to the mock library (default /usr/local/lib/libMockWeComSdk.so)
 *   WECOM_MOCK_LARGE_MB  size of the large-file memory test (default 300, 0 to skip)
 *   WECOM_SMALL_FS       directory on a tiny filesystem, for the disk-full test (optional)
 */

require __DIR__ . '/helpers.php';

echo "=== WeComArchive Streaming Tests (mock SDK) ===\n\n";

$mockLib = getenv('WECOM_MOCK_SDK') ?: '/usr/local/lib/libMockWeComSdk.so';
if (!is_file($mockLib)) {
    skip('Mock SDK tests', "{$mockLib} not found");
    finish();
}

// The extension keeps the first SDK library it loads for the whole process,
// so this script must not construct an instance with the real SDK.
$archive = new WeComArchive(['corpid' => 'mock', 'secret' => 'mock', 'lib_path' => $mockLib]);
$dir = make_temp_dir('wecomarchive-mock');
const CHUNK = 524288;

/** Write $size random bytes to $path and return their md5. */
function make_source(string $path, int $size): string {
    $fh = fopen($path, 'wb');
    $ctx = hash_init('md5');
    for ($left = $size; $left > 0; $left -= $n) {
        $n = min(1 << 20, $left);
        $block = random_bytes($n);
        fwrite($fh, $block);
        hash_update($ctx, $block);
    }
    fclose($fh);
    return hash_final($ctx);
}

/** Build a mock sdkfileid; "n" keeps the mock's per-call failure counter apart between tests. */
function spec(string $file, array $extra = []): string {
    static $n = 0;
    $parts = ['file=' . $file, 'n=' . ++$n];
    foreach ($extra as $key => $value) {
        $parts[] = "{$key}={$value}";
    }
    return implode(';', $parts);
}

/** Peak RSS of this process in bytes (Linux), or null. */
function vm_hwm(): ?int {
    $status = @file_get_contents('/proc/self/status');
    return $status && preg_match('/^VmHWM:\s+(\d+) kB/m', $status, $m) ? (int)$m[1] * 1024 : null;
}

// --- Multi-chunk download, odd tail ---
$src = "{$dir}/multi.src";
$size = 3 * CHUNK + 123;
$md5 = make_source($src, $size);
$target = "{$dir}/multi.bin";

$written = $archive->saveMediaData(spec($src), $target);
test(
    "saveMediaData() writes a multi-chunk file",
    $written === $size && filesize($target) === $size && md5_file($target) === $md5 && leftover_parts($target) === [],
    "written={$written}"
);

$data = $archive->getMediaData(spec($src));
test("getMediaData() returns the same multi-chunk content", is_string($data) && md5($data) === $md5);
unset($data);

// --- Exact multiple of the chunk size ---
$src = "{$dir}/exact.src";
$md5 = make_source($src, 2 * CHUNK);
$written = $archive->saveMediaData(spec($src), "{$dir}/exact.bin");
test(
    "saveMediaData() handles a size that is a multiple of the chunk size",
    $written === 2 * CHUNK && md5_file("{$dir}/exact.bin") === $md5
);

// --- Many tiny chunks (indexbuf handling, leak check under valgrind) ---
$src = "{$dir}/tiny.src";
$md5 = make_source($src, 200000);
$written = $archive->saveMediaData(spec($src, ['chunk' => 100]), "{$dir}/tiny.bin");
test("saveMediaData() handles 2000 small chunks", $written === 200000 && md5_file("{$dir}/tiny.bin") === $md5);
test("getMediaData() handles 2000 small chunks", md5($archive->getMediaData(spec($src, ['chunk' => 100]))) === $md5);

// --- Zero-byte file ---
$src = "{$dir}/empty.src";
touch($src);
$target = "{$dir}/empty.bin";
$written = $archive->saveMediaData(spec($src), $target, ['md5' => md5('')]);
test(
    "saveMediaData() creates an empty file for a zero-byte media",
    $written === 0 && is_file($target) && filesize($target) === 0 && leftover_parts($target) === []
);
test("getMediaData() returns '' for a zero-byte media", $archive->getMediaData(spec($src)) === '');

// --- Overwrite, relative path and file:// ---
$src = "{$dir}/small.src";
$md5 = make_source($src, 4096);
$target = "{$dir}/overwrite.bin";
file_put_contents($target, 'old content');
$archive->saveMediaData(spec($src), $target);
test("saveMediaData() replaces an existing target", md5_file($target) === $md5 && leftover_parts($target) === []);

$cwd = getcwd();
chdir($dir);
$written = $archive->saveMediaData(spec($src), 'relative.bin');
chdir($cwd);
test("saveMediaData() accepts a relative path", $written === 4096 && md5_file("{$dir}/relative.bin") === $md5);

$written = $archive->saveMediaData(spec($src), "file://{$dir}/wrapper.bin");
test("saveMediaData() accepts a file:// path", $written === 4096 && md5_file("{$dir}/wrapper.bin") === $md5);

// --- Permissions: a replaced file keeps its mode, a new file follows the umask ---
$oldUmask = umask(022);
$target = "{$dir}/perm.bin";
file_put_contents($target, 'secret');
chmod($target, 0600);
$archive->saveMediaData(spec($src), $target);
clearstatcache();
test("saveMediaData() keeps the mode of a replaced file", (fileperms($target) & 0777) === 0600, sprintf('%o', fileperms($target) & 0777));
$archive->saveMediaData(spec($src), "{$dir}/perm-new.bin");
clearstatcache();
test(
    "saveMediaData() creates a new file according to the umask",
    (fileperms("{$dir}/perm-new.bin") & 0777) === 0644,
    sprintf('%o', fileperms("{$dir}/perm-new.bin") & 0777)
);
umask($oldUmask);

// --- A target that exists but cannot be inspected is never treated as missing ---
symlink('loop.bin', "{$dir}/loop.bin");  // stat() fails with ELOOP, not ENOENT
$e = catch_exception(function () use ($archive, $src, $dir) {
    $archive->saveMediaData(spec($src), "{$dir}/loop.bin");
});
test(
    "saveMediaData() refuses a target whose stat() fails with anything but ENOENT",
    $e && $e->getCode() === WECOM_ERR_PATH && is_link("{$dir}/loop.bin") && leftover_parts("{$dir}/loop.bin") === [],
    describe_exception($e)
);

// --- Owner and group of a replaced file (handing files to other users needs root) ---
if (function_exists('posix_geteuid') && posix_geteuid() === 0) {
    $target = "{$dir}/owned.bin";
    file_put_contents($target, 'secret');
    chown($target, 1234);
    chgrp($target, 2345);
    chmod($target, 0640);
    $archive->saveMediaData(spec($src), $target);
    clearstatcache();
    test(
        "saveMediaData() keeps the owner, group and mode of a replaced file",
        fileowner($target) === 1234 && filegroup($target) === 2345 && (fileperms($target) & 0777) === 0640 && md5_file($target) === $md5,
        sprintf('%d:%d %o', fileowner($target), filegroup($target), fileperms($target) & 0777)
    );

    // An unprivileged process cannot hand the new file to the replaced file's owner, so it must give up
    $shared = "{$dir}/shared";
    mkdir($shared);
    chmod($shared, 0777);
    chmod($src, 0644);
    $target = "{$shared}/root-owned.bin";
    file_put_contents($target, 'keep me');
    $child = <<<'PHP'
posix_setgid(65534);
posix_setuid(65534);
$archive = new WeComArchive(['corpid' => 'mock', 'secret' => 'mock', 'lib_path' => $argv[1]]);
try {
    $written = $archive->saveMediaData($argv[2], $argv[3]);
    echo 'written ', $written;
} catch (Exception $e) {
    echo $e->getCode(), ' ', $e->getMessage();
}
PHP;
    $cmd = escapeshellarg(PHP_BINARY) . ' -r ' . escapeshellarg($child) . ' ' . escapeshellarg($mockLib) . ' '
        . escapeshellarg(spec($src)) . ' ' . escapeshellarg($target) . ' 2>&1';
    $output = trim((string)shell_exec($cmd));
    clearstatcache();
    test(
        "saveMediaData() refuses to replace a file whose owner it cannot keep",
        strpos($output, WECOM_ERR_WRITE . ' ') === 0 && file_get_contents($target) === 'keep me' && fileowner($target) === 0
            && leftover_parts($target) === [],
        $output . ' | parts: ' . implode(', ', leftover_parts($target))
    );
} else {
    skip("Owner and group of a replaced file", "needs root and the posix extension");
}

// --- POSIX ACLs of a replaced file: named users and groups neither gain nor lose access ---
$getfacl = function (string $file): string {
    return trim((string)shell_exec('getfacl --omit-header --absolute-names ' . escapeshellarg($file) . ' 2>&1'));
};
$aclProbe = "{$dir}/acl-probe";
touch($aclProbe);
shell_exec('setfacl -m u:3456:r ' . escapeshellarg($aclProbe) . ' 2>/dev/null');
if (function_exists('posix_geteuid') && posix_geteuid() === 0 && strpos($getfacl($aclProbe), 'user:3456:r--') !== false) {
    // The replaced file's ACL narrows its group below the mode bits (mode shows 0640, group has no access)
    $target = "{$dir}/acl.bin";
    file_put_contents($target, 'secret');
    shell_exec('setfacl -m u:3456:r,g::-,m::r ' . escapeshellarg($target));
    $before = $getfacl($target);
    $archive->saveMediaData(spec($src), $target);
    test(
        "saveMediaData() keeps the ACL of a replaced file",
        strpos($before, 'user:3456:r--') !== false && $getfacl($target) === $before && md5_file($target) === $md5,
        str_replace("\n", ' ', $getfacl($target))
    );

    // The directory has a default ACL, the replaced file has none: the new file must not inherit it
    $aclDir = "{$dir}/acl-dir";
    mkdir($aclDir);
    shell_exec('setfacl -d -m u:4567:r ' . escapeshellarg($aclDir));
    $target = "{$aclDir}/plain.bin";
    file_put_contents($target, 'secret');
    shell_exec('setfacl -b ' . escapeshellarg($target));
    chmod($target, 0600);
    $before = $getfacl($target);
    $archive->saveMediaData(spec($src), $target);
    test(
        "a replaced file does not inherit the directory's default ACL",
        strpos($before, '4567') === false && $getfacl($target) === $before,
        str_replace("\n", ' ', $getfacl($target))
    );

    // A new file inherits the default ACL, like any file created there
    $archive->saveMediaData(spec($src), "{$aclDir}/new.bin");
    test(
        "a new file inherits the directory's default ACL",
        strpos($getfacl("{$aclDir}/new.bin"), 'user:4567:r--') !== false,
        str_replace("\n", ' ', $getfacl("{$aclDir}/new.bin"))
    );
} else {
    skip("ACL tests", "needs root, setfacl and a file system with POSIX ACLs");
}

// --- PHP's stat and realpath caches see the new file in the same request ---
$target = "{$dir}/statcache.bin";
file_put_contents($target, 'old');
$before = filesize($target);  // primes the stat cache
$archive->saveMediaData(spec($src), $target);
test("filesize() sees the new file right after saveMediaData()", $before === 3 && filesize($target) === 4096, "after: " . filesize($target));

file_put_contents("{$dir}/link-dest.bin", 'old destination');
symlink("{$dir}/link-dest.bin", "{$dir}/link.bin");
$old = file_get_contents("{$dir}/link.bin");  // primes the realpath cache
$archive->saveMediaData(spec($src), "{$dir}/link.bin");
test(
    "a replaced symlink reads back as the new file in the same request",
    $old === 'old destination' && !is_link("{$dir}/link.bin") && md5(file_get_contents("{$dir}/link.bin")) === $md5
        && file_get_contents("{$dir}/link-dest.bin") === 'old destination'
);

// --- Symlink followed by ".." in the target: create, rename and clean up in the same directory
// (ZTS builds used to rename/unlink lexically while the temporary file followed the symlink) ---
mkdir("{$dir}/sl/sub/deep", 0777, true);
symlink("{$dir}/sl/sub/deep", "{$dir}/sl/link");
$written = $archive->saveMediaData(spec($src), "{$dir}/sl/link/../via-link.bin");
test(
    "saveMediaData() resolves a symlink/.. target like the kernel does",
    $written === 4096 && md5_file("{$dir}/sl/sub/via-link.bin") === $md5 && !file_exists("{$dir}/sl/via-link.bin")
        && leftover_parts("{$dir}/sl/sub/via-link.bin") === [] && leftover_parts("{$dir}/sl/via-link.bin") === []
);
$e = catch_exception(function () use ($archive, $src, $dir) {
    $archive->saveMediaData(spec($src, ['error' => 10005]), "{$dir}/sl/link/../failed.bin");
});
test(
    "a failed symlink/.. download leaves no temporary file",
    $e && $e->getCode() === 10005 && leftover_parts("{$dir}/sl/sub/failed.bin") === [] && leftover_parts("{$dir}/sl/failed.bin") === [],
    describe_exception($e)
);

// --- md5 option ---
$src = "{$dir}/md5.src";
$md5 = make_source($src, CHUNK + 1);
$target = "{$dir}/md5.bin";
$written = $archive->saveMediaData(spec($src), $target, ['md5' => strtoupper($md5)]);
test("saveMediaData() accepts a matching md5 (any case)", $written === CHUNK + 1 && md5_file($target) === $md5);

file_put_contents($target, 'keep me');
$e = catch_exception(function () use ($archive, $src, $target) {
    $archive->saveMediaData(spec($src), $target, ['md5' => str_repeat('0', 32)]);
});
test(
    "saveMediaData() rejects an md5 mismatch with WECOM_ERR_MD5",
    $e && $e->getCode() === WECOM_ERR_MD5 && strpos($e->getMessage(), $md5) !== false,
    describe_exception($e)
);
test(
    "md5 mismatch keeps the existing target and leaves no temporary file",
    file_get_contents($target) === 'keep me' && leftover_parts($target) === []
);

// --- Retries ---
$src = "{$dir}/retry.src";
$md5 = make_source($src, 3 * CHUNK);
$target = "{$dir}/retry.bin";

foreach ([10001, 10002, 10003] as $code) {
    $written = $archive->saveMediaData(spec($src, ['fail_at' => 1, 'fail_code' => $code, 'fail_times' => 2]), $target);
    test(
        "saveMediaData() retries a chunk failing twice with {$code} (default retries = 2)",
        $written === 3 * CHUNK && md5_file($target) === $md5
    );
}

$written = $archive->saveMediaData(spec($src, ['fail_at' => 0, 'fail_times' => 1]), $target);
test("saveMediaData() retries the first chunk", $written === 3 * CHUNK && md5_file($target) === $md5);

$written = $archive->saveMediaData(spec($src, ['fail_at' => 2, 'fail_times' => 5]), $target, ['retries' => 5]);
test("saveMediaData() honours a custom retries value", $written === 3 * CHUNK && md5_file($target) === $md5);

file_put_contents($target, 'keep me');
$e = catch_exception(function () use ($archive, $src, $target) {
    $archive->saveMediaData(spec($src, ['fail_at' => 1, 'fail_times' => 3]), $target);
});
test(
    "saveMediaData() gives up after the retries with the SDK error code",
    $e && $e->getCode() === 10001 && strpos($e->getMessage(), 'after 2 retries') !== false,
    describe_exception($e)
);
test(
    "exhausted retries keep the existing target and leave no temporary file",
    file_get_contents($target) === 'keep me' && leftover_parts($target) === []
);

$e = catch_exception(function () use ($archive, $src, $target) {
    $archive->saveMediaData(spec($src, ['fail_at' => 1, 'fail_times' => 1]), $target, ['retries' => 0]);
});
test("saveMediaData() does not retry with retries = 0", $e && $e->getCode() === 10001, describe_exception($e));

$e = catch_exception(function () use ($archive, $src, $target) {
    $archive->saveMediaData(spec($src, ['fail_at' => 1, 'fail_code' => 10005, 'fail_times' => 1]), $target);
});
test(
    "saveMediaData() does not retry other SDK errors",
    $e && $e->getCode() === 10005 && strpos($e->getMessage(), 'retries') === false,
    describe_exception($e)
);

$e = catch_exception(function () use ($archive, $src) {
    $archive->getMediaData(spec($src, ['fail_at' => 1, 'fail_times' => 1]));
});
test("getMediaData() still does not retry by default", $e && $e->getCode() === 10001, describe_exception($e));

$data = $archive->getMediaData(spec($src, ['fail_at' => 1, 'fail_times' => 1]), ['retries' => 1]);
test("getMediaData() retries when asked to", md5($data) === $md5);
unset($data);

$e = catch_exception(function () use ($archive, $src) {
    $archive->getMediaData(spec($src, ['error' => 10010]));
});
test("getMediaData() passes SDK error codes through", $e && $e->getCode() === 10010, describe_exception($e));

// --- Disk full ---
$smallFs = getenv('WECOM_SMALL_FS');
if ($smallFs && is_dir($smallFs) && is_writable($smallFs)) {
    $src = "{$dir}/big.src";
    make_source($src, 3 << 20);
    $target = "{$smallFs}/full.bin";
    $e = catch_exception(function () use ($archive, $src, $target) {
        $archive->saveMediaData(spec($src), $target);
    });
    test(
        "saveMediaData() reports a full disk with WECOM_ERR_WRITE",
        $e && $e->getCode() === WECOM_ERR_WRITE,
        describe_exception($e)
    );
    test(
        "full disk leaves neither the target nor a temporary file",
        !file_exists($target) && leftover_parts($target) === [],
        implode(', ', leftover_parts($target))
    );
} else {
    skip("Disk full test", "WECOM_SMALL_FS not set");
}

// --- Write errors reported only by fsync() or close(), e.g. on NFS (child process with an LD_PRELOAD shim) ---
$failIo = getenv('WECOM_FAIL_IO_LIB') ?: '/usr/local/lib/libFailIo.so';
if (is_file($failIo)) {
    $src = "{$dir}/io.src";
    make_source($src, CHUNK + 10);
    $child = <<<'PHP'
$archive = new WeComArchive(['corpid' => 'mock', 'secret' => 'mock', 'lib_path' => $argv[1]]);
try {
    $written = $archive->saveMediaData($argv[2], $argv[3]);
    echo 'written ', $written;
} catch (Exception $e) {
    echo $e->getCode(), ' ', $e->getMessage();
}
PHP;
    foreach (['none', 'fsync', 'close'] as $mode) {
        if ($mode === 'fsync' && PHP_VERSION_ID < 80100) {
            // PHP loads extensions with RTLD_DEEPBIND, so LD_PRELOAD cannot intercept the extension's own
            // fsync() call; from PHP 8.1 it goes through php_stream_sync() in the PHP binary instead.
            skip("a failing fsync() throws WECOM_ERR_WRITE", "fsync() cannot be intercepted on PHP < 8.1");
            continue;
        }
        $target = "{$dir}/io-{$mode}.bin";
        file_put_contents($target, 'keep me');
        $cmd = 'WECOM_FAIL_IO=' . $mode . ' LD_PRELOAD=' . escapeshellarg($failIo) . ' ' . escapeshellarg(PHP_BINARY)
            . ' -r ' . escapeshellarg($child) . ' ' . escapeshellarg($mockLib) . ' ' . escapeshellarg(spec($src))
            . ' ' . escapeshellarg($target) . ' 2>&1';
        $output = trim((string)shell_exec($cmd));
        if ($mode === 'none') {
            test(
                "saveMediaData() works with the I/O shim loaded but idle",
                $output === 'written ' . (CHUNK + 10) && filesize($target) === CHUNK + 10,
                $output
            );
            continue;
        }
        test(
            "a failing {$mode}() throws WECOM_ERR_WRITE and keeps the existing target",
            strpos($output, WECOM_ERR_WRITE . ' ') === 0 && file_get_contents($target) === 'keep me' && leftover_parts($target) === [],
            $output . ' | parts: ' . implode(', ', leftover_parts($target))
        );
    }
} else {
    skip("fsync()/close() failure tests", "{$failIo} not found");
}

// --- max_execution_time during a download (child process, the fatal error ends it) ---
$src = "{$dir}/slow.src";
make_source($src, 32 << 20);
$target = "{$dir}/slow.bin";
$child = <<<'PHP'
$archive = new WeComArchive(['corpid' => 'mock', 'secret' => 'mock', 'lib_path' => $argv[1]]);
$archive->saveMediaData($argv[2], $argv[3]);
echo "finished";
PHP;
$cmd = escapeshellarg(PHP_BINARY) . ' -d max_execution_time=1 -r ' . escapeshellarg($child) . ' '
    . escapeshellarg($mockLib) . ' ' . escapeshellarg(spec($src, ['chunk' => 64])) . ' ' . escapeshellarg($target) . ' 2>&1';
$output = (string)shell_exec($cmd);
test(
    "max_execution_time aborts the download and removes the temporary file",
    stripos($output, 'maximum execution time') !== false && strpos($output, 'finished') === false
        && !file_exists($target) && leftover_parts($target) === [],
    trim($output) . ' | parts: ' . implode(', ', leftover_parts($target))
);
unlink($src);

// --- Large file: memory must not grow with the file size ---
$largeMb = (int)(getenv('WECOM_MOCK_LARGE_MB') === false ? 300 : getenv('WECOM_MOCK_LARGE_MB'));
if ($largeMb > 0) {
    $src = "{$dir}/large.src";
    $size = $largeMb << 20;
    $md5 = make_source($src, $size);
    $target = "{$dir}/large.bin";

    if (function_exists('memory_reset_peak_usage')) {
        memory_reset_peak_usage();
    }
    $peakBefore = memory_get_peak_usage(true);
    $hwmBefore = vm_hwm();
    $start = microtime(true);
    $written = $archive->saveMediaData(spec($src), $target, ['md5' => $md5]);
    $seconds = microtime(true) - $start;
    $peakDelta = memory_get_peak_usage(true) - $peakBefore;
    $hwmDelta = $hwmBefore === null ? null : vm_hwm() - $hwmBefore;

    printf(
        "       %d MB in %.2fs, PHP peak +%.1f MB, process peak RSS +%s MB\n",
        $largeMb, $seconds, $peakDelta / 1048576, $hwmDelta === null ? 'n/a' : sprintf('%.1f', $hwmDelta / 1048576)
    );
    test("saveMediaData() writes a {$largeMb} MB file intact", $written === $size && md5_file($target) === $md5);
    test("saveMediaData() PHP peak memory grows by < 16 MB", $peakDelta < (16 << 20), sprintf('%.1f MB', $peakDelta / 1048576));
    if ($hwmDelta !== null) {
        test("saveMediaData() process peak RSS grows by < 16 MB", $hwmDelta < (16 << 20), sprintf('%.1f MB', $hwmDelta / 1048576));
    }
    unlink($src);
    unlink($target);
} else {
    skip("Large file memory test", "WECOM_MOCK_LARGE_MB=0");
}

remove_dir($dir);

finish();
