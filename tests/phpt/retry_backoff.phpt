--TEST--
Retries of 10001-10003 pause 200 ms x attempt between calls, never longer than the remaining max_seconds
--EXTENSIONS--
wecomarchive
--SKIPIF--
<?php require __DIR__ . '/skipif.inc'; ?>
--FILE--
<?php
require __DIR__ . '/mock.inc';
$archive = mock_archive();
$dir = mock_dir();
$src = "{$dir}/retry.src";
$md5 = mock_source($src, 3 * 1024);
$target = "{$dir}/retry.bin";
$log = "{$dir}/calls.log";

// Chunk 1 fails twice, the default retries = 2 cover it
$bytes = $archive->saveMediaData(mock_spec($src, ['chunk' => 1024, 'fail_at' => 1, 'fail_times' => 2, 'log' => $log]), $target);
check('download succeeds after two retries', $bytes === 3 * 1024 && md5_file($target) === $md5);
$calls = mock_log($log);
check('five calls were made (3 chunks + 2 retries)', count($calls) === 5, (string)count($calls));
check('retries repeat the same indexbuf', $calls[1][2] === $calls[2][2] && $calls[2][2] === $calls[3][2], json_encode(array_column($calls, 2)));
$gap1 = $calls[2][0] - $calls[1][0];
$gap2 = $calls[3][0] - $calls[2][0];
check('first retry waits about 200 ms', $gap1 >= 190 && $gap1 < 600, "{$gap1} ms");
check('second retry waits about 400 ms', $gap2 >= 390 && $gap2 < 800, "{$gap2} ms");
check('successful chunks do not wait', $calls[1][0] - $calls[0][0] < 100 && $calls[4][0] - $calls[3][0] < 100);

// The pause is capped by the remaining time; what is left of max_seconds then triggers WECOM_ERR_TIMEOUT
@unlink($log);
$t0 = microtime(true);
$e = caught(function () use ($archive, $src, $target, $log) {
    $archive->saveMediaData(mock_spec($src, ['chunk' => 1024, 'fail_at' => 1, 'fail_times' => 9, 'log' => $log]), $target,
        ['retries' => 9, 'max_seconds' => 0.5]);
});
$elapsed = microtime(true) - $t0;
check('backoff cannot push past max_seconds', $e && $e->getCode() === WECOM_ERR_TIMEOUT, describe($e));
check('it gives up about when max_seconds passes', $elapsed >= 0.5 && $elapsed < 1.0, sprintf('%.2fs', $elapsed));
check('only the retries that fit were attempted', count(mock_log($log)) <= 4, (string)count(mock_log($log)));
check('no temporary file is left', mock_parts($target) === []);
?>
--EXPECT--
ok - download succeeds after two retries
ok - five calls were made (3 chunks + 2 retries)
ok - retries repeat the same indexbuf
ok - first retry waits about 200 ms
ok - second retry waits about 400 ms
ok - successful chunks do not wait
ok - backoff cannot push past max_seconds
ok - it gives up about when max_seconds passes
ok - only the retries that fit were attempted
ok - no temporary file is left
