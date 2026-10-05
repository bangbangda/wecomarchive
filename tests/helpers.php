<?php
/**
 * Shared helpers for the WeComArchive test scripts
 */

$passed = 0;
$failed = 0;
$skipped = 0;

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

function skip($name, $reason) {
    global $skipped;
    echo "[SKIP] {$name} - {$reason}\n";
    $skipped++;
}

/**
 * Run $fn and return the exception it throws, or null if it returns normally.
 */
function catch_exception(callable $fn): ?Throwable {
    try {
        $fn();
    } catch (Throwable $e) {
        return $e;
    }
    return null;
}

function describe_exception(?Throwable $e): string {
    return $e ? sprintf('%s(%d): %s', get_class($e), $e->getCode(), $e->getMessage()) : 'no exception';
}

/**
 * Leftover "<target>.part-*" temporary files of saveMediaData() next to $target.
 */
function leftover_parts(string $target): array {
    return glob($target . '.part-*') ?: [];
}

function make_temp_dir(string $prefix): string {
    $dir = sys_get_temp_dir() . '/' . $prefix . '-' . bin2hex(random_bytes(4));
    mkdir($dir, 0777, true);
    return $dir;
}

function remove_dir(string $dir): void {
    if (!is_dir($dir)) {
        return;
    }
    foreach (scandir($dir) as $entry) {
        if ($entry === '.' || $entry === '..') {
            continue;
        }
        $path = "{$dir}/{$entry}";
        is_dir($path) && !is_link($path) ? remove_dir($path) : unlink($path);
    }
    rmdir($dir);
}

function finish(): void {
    global $passed, $failed, $skipped;
    echo "\n=== Test Summary ===\n";
    echo "Passed:  {$passed}\n";
    echo "Failed:  {$failed}\n";
    echo "Skipped: {$skipped}\n";
    echo "Total:   " . ($passed + $failed + $skipped) . "\n";
    exit($failed > 0 ? 1 : 0);
}
