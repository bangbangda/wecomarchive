<?php
/**
 * Helpers for the phpt tests that run against the mock SDK (tests/mock-sdk/mock_sdk.c).
 * The extension keeps the first SDK library it loads for the whole process, so each test
 * (its own process) only ever constructs instances with the mock.
 */

const MOCK_CHUNK = 524288;

function mock_lib(): string {
    return getenv('WECOM_MOCK_SDK') ?: '/usr/local/lib/libMockWeComSdk.so';
}

function mock_archive(): WeComArchive {
    return new WeComArchive(['corpid' => 'mock', 'secret' => 'mock', 'lib_path' => mock_lib()]);
}

/** A fresh temporary directory, removed when the test ends. */
function mock_dir(): string {
    $dir = sys_get_temp_dir() . '/wecomarchive-phpt-' . bin2hex(random_bytes(4));
    mkdir($dir, 0777, true);
    register_shutdown_function(function () use ($dir) { mock_remove_dir($dir); });
    return $dir;
}

function mock_remove_dir(string $dir): void {
    if (!is_dir($dir)) {
        return;
    }
    foreach (scandir($dir) as $entry) {
        if ($entry === '.' || $entry === '..') {
            continue;
        }
        $path = "{$dir}/{$entry}";
        is_dir($path) && !is_link($path) ? mock_remove_dir($path) : unlink($path);
    }
    rmdir($dir);
}

/** Write $size pseudo-random bytes to $path and return their md5. */
function mock_source(string $path, int $size): string {
    $fh = fopen($path, 'wb');
    $ctx = hash_init('md5');
    $block = $size > 0 ? random_bytes(min(1 << 20, $size)) : '';
    for ($left = $size; $left > 0; $left -= $n) {
        $n = min(1 << 20, $left);
        $piece = $n === strlen($block) ? $block : substr($block, 0, $n);
        fwrite($fh, $piece);
        hash_update($ctx, $piece);
        $block[0] = chr(random_int(0, 255));  // every block differs
    }
    fclose($fh);
    return hash_final($ctx);
}

/** Build a mock sdkfileid; "n" keeps the mock's per-call failure counter apart between downloads. */
function mock_spec(string $file, array $extra = []): string {
    static $n = 0;
    $parts = ['file=' . $file, 'n=' . ++$n];
    foreach ($extra as $key => $value) {
        $parts[] = "{$key}={$value}";
    }
    return implode(';', $parts);
}

/** Calls recorded by the mock's log= option: [[ms, timeout, indexbuf], ...]. */
function mock_log(string $log): array {
    $calls = [];
    foreach (file($log, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) ?: [] as $line) {
        [$ms, $timeout, $indexbuf] = array_pad(explode(' ', $line, 3), 3, '');
        $calls[] = [(int)$ms, (int)$timeout, $indexbuf];
    }
    return $calls;
}

/** Leftover "<target>.part-*" temporary files of saveMediaData() next to $target. */
function mock_parts(string $target): array {
    return glob($target . '.part-*') ?: [];
}

/** Peak RSS of this process in bytes (Linux), or null. */
function vm_hwm(): ?int {
    $status = @file_get_contents('/proc/self/status');
    return $status && preg_match('/^VmHWM:\s+(\d+) kB/m', $status, $m) ? (int)$m[1] * 1024 : null;
}

/** CPU time (user + system) used by this process so far, in seconds. */
function cpu_seconds(): float {
    $u = getrusage();
    return $u['ru_utime.tv_sec'] + $u['ru_stime.tv_sec'] + ($u['ru_utime.tv_usec'] + $u['ru_stime.tv_usec']) / 1e6;
}

function check(string $label, bool $ok, string $detail = ''): void {
    echo $ok ? "ok - {$label}\n" : "not ok - {$label}" . ($detail !== '' ? ": {$detail}" : '') . "\n";
}

function describe(?Throwable $e): string {
    return $e ? sprintf('%s(%d): %s', get_class($e), $e->getCode(), $e->getMessage()) : 'no exception';
}

/** Run $fn and return the exception it throws, or null. */
function caught(callable $fn): ?Throwable {
    try {
        $fn();
    } catch (Throwable $e) {
        return $e;
    }
    return null;
}

/** Command prefix for a child PHP process that has the extension loaded, whatever the ini setup. */
function mock_php_cmd(): string {
    static $cmd = null;
    if ($cmd === null) {
        $cmd = escapeshellarg(PHP_BINARY);
        $loaded = trim((string)shell_exec($cmd . ' -r ' . escapeshellarg('echo (int)extension_loaded("wecomarchive");') . ' 2>/dev/null'));
        if ($loaded !== '1') {
            $cmd .= ' -d extension=' . escapeshellarg(ini_get('extension_dir') . '/wecomarchive.so');
        }
    }
    return $cmd;
}
