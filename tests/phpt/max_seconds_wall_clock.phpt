--TEST--
saveMediaData(): max_seconds is a wall-clock limit that works while chunks block without using CPU
--EXTENSIONS--
wecomarchive
--SKIPIF--
<?php require __DIR__ . '/skipif.inc'; ?>
--FILE--
<?php
require __DIR__ . '/mock.inc';
$archive = mock_archive();
$dir = mock_dir();

$src = "{$dir}/slow.src";
mock_source($src, 20 * 1024);
$target = "{$dir}/slow.bin";
file_put_contents($target, 'keep me');

// Every chunk sleeps 300 ms in the SDK: no CPU time, so max_execution_time would never fire
$cpu0 = cpu_seconds();
$t0 = microtime(true);
$e = caught(function () use ($archive, $src, $target) {
    $archive->saveMediaData(mock_spec($src, ['chunk' => 1024, 'delay_ms' => 300]), $target, ['max_seconds' => 1]);
});
$elapsed = microtime(true) - $t0;
$cpu = cpu_seconds() - $cpu0;

check('throws WECOM_ERR_TIMEOUT', $e && $e->getCode() === WECOM_ERR_TIMEOUT && $e->getCode() === 20004, describe($e));
check('message names the limit and the progress', $e && preg_match('/exceeded max_seconds \(1 s\) after \d+ bytes/', $e->getMessage()) === 1, describe($e));
check('stops right after the limit (1 s + one 300 ms chunk)', $elapsed >= 1.0 && $elapsed < 2.0, sprintf('%.2fs', $elapsed));
check('barely any CPU time was used meanwhile', $cpu < 0.5, sprintf('%.2fs', $cpu));
check('target file is untouched', file_get_contents($target) === 'keep me');
check('no temporary file is left', mock_parts($target) === [], implode(', ', mock_parts($target)));

// Float limit, and a download that fits in the budget
$bytes = $archive->saveMediaData(mock_spec($src, ['chunk' => 4096, 'delay_ms' => 20]), $target, ['max_seconds' => 5.5]);
check('a download within max_seconds completes', $bytes === 20 * 1024 && filesize($target) === 20 * 1024);

// getMediaData() honours the same option
$e = caught(function () use ($archive, $src) {
    $archive->getMediaData(mock_spec($src, ['chunk' => 1024, 'delay_ms' => 300]), ['max_seconds' => 0.5]);
});
check('getMediaData() throws WECOM_ERR_TIMEOUT as well', $e && $e->getCode() === WECOM_ERR_TIMEOUT, describe($e));

// The per-chunk timeout handed to the SDK is cut down to the remaining time (at least 1 s)
$log = "{$dir}/calls.log";
$e = caught(function () use ($archive, $src, $target, $log) {
    $archive->saveMediaData(mock_spec($src, ['chunk' => 1024, 'delay_ms' => 300, 'log' => $log]), $target, ['timeout' => 10, 'max_seconds' => 1.5]);
});
$timeouts = array_column(mock_log($log), 1);
check('per-chunk timeout starts at ceil(remaining) = 2', $timeouts[0] === 2, implode(',', $timeouts));
check('per-chunk timeout never exceeds the remaining time', max($timeouts) <= 2 && min($timeouts) === 1, implode(',', $timeouts));
check('per-chunk timeout ends at the 1 s floor', end($timeouts) === 1, implode(',', $timeouts));
check('about five 300 ms chunks fit into 1.5 s', count($timeouts) >= 4 && count($timeouts) <= 6, (string)count($timeouts));

unlink($log);
$archive->saveMediaData(mock_spec($src, ['chunk' => 4096, 'log' => $log]), $target, ['timeout' => 10]);
check('without max_seconds the timeout option is passed verbatim', array_unique(array_column(mock_log($log), 1)) === [10]);
?>
--EXPECT--
ok - throws WECOM_ERR_TIMEOUT
ok - message names the limit and the progress
ok - stops right after the limit (1 s + one 300 ms chunk)
ok - barely any CPU time was used meanwhile
ok - target file is untouched
ok - no temporary file is left
ok - a download within max_seconds completes
ok - getMediaData() throws WECOM_ERR_TIMEOUT as well
ok - per-chunk timeout starts at ceil(remaining) = 2
ok - per-chunk timeout never exceeds the remaining time
ok - per-chunk timeout ends at the 1 s floor
ok - about five 300 ms chunks fit into 1.5 s
ok - without max_seconds the timeout option is passed verbatim
