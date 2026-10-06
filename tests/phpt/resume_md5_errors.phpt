--TEST--
saveMediaDataPart(): md5 is checked when finished; errors keep the part file for the caller
--EXTENSIONS--
wecomarchive
--SKIPIF--
<?php require __DIR__ . '/skipif.inc'; ?>
--FILE--
<?php
require __DIR__ . '/mock.inc';
$archive = mock_archive();
$dir = mock_dir();

$size = 3 * MOCK_CHUNK + 9;
$src = "{$dir}/media.src";
$md5 = mock_source($src, $size);
$part = "{$dir}/media.part";

// md5 only matters once the download is finished
$a = $archive->saveMediaDataPart(mock_spec($src, ['delay_ms' => 150]), $part, ['max_seconds' => 0.2, 'md5' => str_repeat('0', 32)]);
check('a wrong md5 is ignored while unfinished', $a['finished'] === false && $a['bytes'] === 2 * MOCK_CHUNK, json_encode($a));

$e = caught(function () use ($archive, $src, $part, $a) {
    $archive->saveMediaDataPart(mock_spec($src), $part, ['indexbuf' => $a['indexbuf'], 'offset' => $a['bytes'], 'md5' => str_repeat('0', 32)]);
});
check('a wrong md5 throws WECOM_ERR_MD5 when finished', $e && $e->getCode() === WECOM_ERR_MD5 && strpos($e->getMessage(), $md5) !== false, describe($e));
clearstatcache();
check('the complete part file is kept', filesize($part) === $size && md5_file($part) === $md5);

$r = $archive->saveMediaDataPart(mock_spec($src), $part, ['indexbuf' => $a['indexbuf'], 'offset' => $a['bytes'], 'md5' => strtoupper($md5)]);
check('a matching md5 (any case) passes', $r['finished'] && md5_file($part) === $md5);

$e = caught(function () use ($archive, $src, $part) {
    $archive->saveMediaDataPart(mock_spec($src), $part, ['md5' => 'nope']);
});
check('an invalid md5 option throws WECOM_ERR_PARAM', $e && $e->getCode() === WECOM_ERR_PARAM, describe($e));

// SDK errors: the exception carries the SDK code, the file keeps what was written
$e = caught(function () use ($archive, $src, $part) {
    $archive->saveMediaDataPart(mock_spec($src, ['fail_at' => 2, 'fail_times' => 5]), $part);
});
check('exhausted retries throw the SDK code', $e && $e->getCode() === 10001 && strpos($e->getMessage(), 'after 2 retries') !== false, describe($e));
clearstatcache();
check('the chunks before the failure stay in the part file', filesize($part) === 2 * MOCK_CHUNK && md5_file($part) === md5(file_get_contents($src, false, null, 0, 2 * MOCK_CHUNK)));

$e = caught(function () use ($archive, $src, $part) {
    $archive->saveMediaDataPart(mock_spec($src, ['error' => 10010]), $part, ['offset' => 1024, 'indexbuf' => 'Range:bytes=1024-2047']);
});
check('a non-retriable SDK error is passed through', $e && $e->getCode() === 10010, describe($e));
clearstatcache();
check('the file was truncated to the offset and kept', filesize($part) === 1024);

// After an error the caller resumes from its last recorded state
$r = $archive->saveMediaDataPart(mock_spec($src), $part, ['offset' => 1024, 'indexbuf' => 'Range:bytes=1024-525311', 'md5' => $md5]);
check('resuming after the error completes the file', $r['finished'] && $r['bytes'] === $size && md5_file($part) === $md5);
?>
--EXPECT--
ok - a wrong md5 is ignored while unfinished
ok - a wrong md5 throws WECOM_ERR_MD5 when finished
ok - the complete part file is kept
ok - a matching md5 (any case) passes
ok - an invalid md5 option throws WECOM_ERR_PARAM
ok - exhausted retries throw the SDK code
ok - the chunks before the failure stay in the part file
ok - a non-retriable SDK error is passed through
ok - the file was truncated to the offset and kept
ok - resuming after the error completes the file
