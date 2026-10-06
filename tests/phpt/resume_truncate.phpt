--TEST--
saveMediaDataPart(): continuing truncates unrecorded bytes; impossible offsets are rejected with WECOM_ERR_RESUME
--EXTENSIONS--
wecomarchive
--SKIPIF--
<?php require __DIR__ . '/skipif.inc'; ?>
--FILE--
<?php
require __DIR__ . '/mock.inc';
$archive = mock_archive();
$dir = mock_dir();

$size = 6 * MOCK_CHUNK + 5;
$src = "{$dir}/media.src";
$md5 = mock_source($src, $size);
$part = "{$dir}/media.part";

// First call: two chunks, then time runs out
$a = $archive->saveMediaDataPart(mock_spec($src, ['delay_ms' => 150]), $part, ['max_seconds' => 0.2]);
check('first call stops unfinished', $a['finished'] === false && $a['bytes'] === 2 * MOCK_CHUNK && $a['indexbuf'] === 'Range:bytes=1048576-1572863', json_encode($a));

// "Crash": another call writes more, but its result is never recorded; junk is appended as well
$archive->saveMediaDataPart(mock_spec($src, ['delay_ms' => 150]), $part, ['max_seconds' => 0.2, 'indexbuf' => $a['indexbuf'], 'offset' => $a['bytes']]);
file_put_contents($part, 'junk written after the crash', FILE_APPEND);
clearstatcache();
check('the file is now longer than the recorded offset', filesize($part) > $a['bytes']);

// Continue from the recorded state: the file is cut back to the offset and completed
$state = ['indexbuf' => $a['indexbuf'], 'offset' => $a['bytes']];
$calls = 0;
do {
    $r = $archive->saveMediaDataPart(mock_spec($src, ['delay_ms' => 50]), $part, $state + ['max_seconds' => 0.12]);
    $calls++;
    $state = ['indexbuf' => $r['indexbuf'], 'offset' => $r['bytes']];
} while (!$r['finished'] && $calls < 20);
check('continuing from the recorded offset completes the file', $r['finished'] && $r['bytes'] === $size);
check('the completed file matches the source', md5_file($part) === $md5);
check('several calls were needed', $calls >= 2, (string)$calls);

// offset larger than the file
file_put_contents($part, str_repeat('a', 1000));
$e = caught(function () use ($archive, $src, $part) {
    $archive->saveMediaDataPart(mock_spec($src), $part, ['offset' => 2000, 'indexbuf' => 'Range:bytes=2000-526287']);
});
check('offset beyond the file size throws WECOM_ERR_RESUME', $e && $e->getCode() === WECOM_ERR_RESUME && strpos($e->getMessage(), 'only 1000 bytes') !== false, describe($e));
check('the file is untouched', file_get_contents($part) === str_repeat('a', 1000));

// offset > 0 but the file does not exist
$e = caught(function () use ($archive, $src, $dir) {
    $archive->saveMediaDataPart(mock_spec($src), "{$dir}/missing.part", ['offset' => 10, 'indexbuf' => 'Range:bytes=10-20']);
});
check('offset > 0 without a file throws WECOM_ERR_RESUME', $e && $e->getCode() === WECOM_ERR_RESUME && strpos($e->getMessage(), 'does not exist') !== false, describe($e));
check('no file was created', !file_exists("{$dir}/missing.part"));

// offset and token disagree
$e = caught(function () use ($archive, $src, $part) {
    $archive->saveMediaDataPart(mock_spec($src), $part, ['offset' => 100, 'indexbuf' => 'Range:bytes=200-300']);
});
check('an indexbuf for another offset throws WECOM_ERR_RESUME', $e && $e->getCode() === WECOM_ERR_RESUME && strpos($e->getMessage(), 'byte 200') !== false, describe($e));
check('the file is still untouched', file_get_contents($part) === str_repeat('a', 1000));

// offset > 0 needs a token
$e = caught(function () use ($archive, $src, $part) {
    $archive->saveMediaDataPart(mock_spec($src), $part, ['offset' => 100]);
});
check('offset > 0 without indexbuf throws WECOM_ERR_PARAM', $e && $e->getCode() === WECOM_ERR_PARAM && strpos($e->getMessage(), "'indexbuf'") !== false, describe($e));

// Invalid option types
foreach ([['offset' => -1], ['offset' => '5'], ['indexbuf' => 5], ['indexbuf' => []]] as $bad) {
    $e = caught(function () use ($archive, $src, $part, $bad) {
        $archive->saveMediaDataPart(mock_spec($src), $part, $bad);
    });
    check(json_encode($bad) . ' throws WECOM_ERR_PARAM', $e && $e->getCode() === WECOM_ERR_PARAM, describe($e));
}

// A token the extension does not understand is passed to the SDK as is (the mock rejects it with 10000)
file_put_contents($part, str_repeat('a', 1000));
$e = caught(function () use ($archive, $src, $part) {
    $archive->saveMediaDataPart(mock_spec($src), $part, ['offset' => 1000, 'indexbuf' => 'opaque-token']);
});
check('an opaque token reaches the SDK unchanged', $e && $e->getCode() === 10000 && strpos($e->getMessage(), 'Failed to get media data') !== false, describe($e));
check('the file was truncated to the offset but otherwise kept', file_get_contents($part) === str_repeat('a', 1000));

// Invalid paths
$e = caught(function () use ($archive, $src, $dir) {
    $archive->saveMediaDataPart(mock_spec($src), "{$dir}/no-such-dir/x.part");
});
check('a missing directory throws WECOM_ERR_PATH', $e && $e->getCode() === WECOM_ERR_PATH, describe($e));
$e = caught(function () use ($archive, $src, $dir) {
    $archive->saveMediaDataPart(mock_spec($src), $dir);
});
check('a directory as part path throws WECOM_ERR_PATH', $e && $e->getCode() === WECOM_ERR_PATH, describe($e));
?>
--EXPECT--
ok - first call stops unfinished
ok - the file is now longer than the recorded offset
ok - continuing from the recorded offset completes the file
ok - the completed file matches the source
ok - several calls were needed
ok - offset beyond the file size throws WECOM_ERR_RESUME
ok - the file is untouched
ok - offset > 0 without a file throws WECOM_ERR_RESUME
ok - no file was created
ok - an indexbuf for another offset throws WECOM_ERR_RESUME
ok - the file is still untouched
ok - offset > 0 without indexbuf throws WECOM_ERR_PARAM
ok - {"offset":-1} throws WECOM_ERR_PARAM
ok - {"offset":"5"} throws WECOM_ERR_PARAM
ok - {"indexbuf":5} throws WECOM_ERR_PARAM
ok - {"indexbuf":[]} throws WECOM_ERR_PARAM
ok - an opaque token reaches the SDK unchanged
ok - the file was truncated to the offset but otherwise kept
ok - a missing directory throws WECOM_ERR_PATH
ok - a directory as part path throws WECOM_ERR_PATH
