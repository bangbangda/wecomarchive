--TEST--
saveMediaDataPart(): a file downloaded in several calls equals a one-shot download
--EXTENSIONS--
wecomarchive
--SKIPIF--
<?php require __DIR__ . '/skipif.inc'; ?>
--FILE--
<?php
require __DIR__ . '/mock.inc';
$archive = mock_archive();
$dir = mock_dir();

$size = 5 * MOCK_CHUNK + 777;
$src = "{$dir}/media.src";
$md5 = mock_source($src, $size);

$once = "{$dir}/once.bin";
$archive->saveMediaData(mock_spec($src), $once);

// Each chunk takes 120 ms and every call may use 0.25 s: three chunks per call, so the six chunks need two calls
$part = "{$dir}/media.part";
$state = ['indexbuf' => '', 'offset' => 0];
$calls = 0;
$shapeOk = $tokenOk = $bytesOk = true;
do {
    $r = $archive->saveMediaDataPart(mock_spec($src, ['delay_ms' => 120]), $part, $state + ['max_seconds' => 0.25]);
    $calls++;
    clearstatcache();
    $shapeOk = $shapeOk && is_array($r) && array_keys($r) === ['finished', 'indexbuf', 'bytes']
        && is_bool($r['finished']) && is_string($r['indexbuf']) && is_int($r['bytes']);
    $bytesOk = $bytesOk && $r['bytes'] === filesize($part);
    if (!$r['finished']) {
        // The token continues exactly where the file ends
        $tokenOk = $tokenOk && preg_match('/^Range:bytes=(\d+)-(\d+)$/', $r['indexbuf'], $m) === 1 && (int)$m[1] === $r['bytes'];
    }
    $state = ['indexbuf' => $r['indexbuf'], 'offset' => $r['bytes']];
} while (!$r['finished'] && $calls < 20);

check('return value is [finished, indexbuf, bytes]', $shapeOk);
check('bytes always equals the file size', $bytesOk);
check('indexbuf of an unfinished call continues at bytes', $tokenOk);
check('the download needed more than one call', $calls >= 2 && $calls < 20, (string)$calls);
check('the final call reports finished with an empty indexbuf', $r['finished'] === true && $r['indexbuf'] === '' && $r['bytes'] === $size);
check('the part file equals the one-shot download', md5_file($part) === $md5 && md5_file($once) === $md5);
check('no temporary file was used', mock_parts($part) === [] && !file_exists("{$part}.part"));

// A download that fits in one call
$r = $archive->saveMediaDataPart(mock_spec($src), "{$dir}/single.part", ['md5' => $md5]);
check('one call is enough without a time limit', $r === ['finished' => true, 'indexbuf' => '', 'bytes' => $size] && md5_file("{$dir}/single.part") === $md5);

// Zero-byte media
touch("{$dir}/empty.src");
$r = $archive->saveMediaDataPart(mock_spec("{$dir}/empty.src"), "{$dir}/empty.part");
check('a zero-byte media finishes at once with an empty file', $r === ['finished' => true, 'indexbuf' => '', 'bytes' => 0] && is_file("{$dir}/empty.part") && filesize("{$dir}/empty.part") === 0);

// offset 0 starts over an existing file
file_put_contents("{$dir}/again.part", str_repeat('x', 100));
$r = $archive->saveMediaDataPart(mock_spec($src), "{$dir}/again.part");
check('offset 0 replaces the content of an existing part file', $r['bytes'] === $size && md5_file("{$dir}/again.part") === $md5);

// Relative and file:// paths, like saveMediaData()
$cwd = getcwd();
chdir($dir);
$r = $archive->saveMediaDataPart(mock_spec($src), 'relative.part');
chdir($cwd);
check('a relative path works', $r['finished'] && md5_file("{$dir}/relative.part") === $md5);
$r = $archive->saveMediaDataPart(mock_spec($src), "file://{$dir}/wrapper.part");
check('a file:// path works', $r['finished'] && md5_file("{$dir}/wrapper.part") === $md5);

// New files follow the umask
umask(022);
$archive->saveMediaDataPart(mock_spec($src), "{$dir}/mode.part", ['max_seconds' => 60]);
clearstatcache();
check('a new part file is created according to the umask', (fileperms("{$dir}/mode.part") & 0777) === 0644, sprintf('%o', fileperms("{$dir}/mode.part") & 0777));
?>
--EXPECT--
ok - return value is [finished, indexbuf, bytes]
ok - bytes always equals the file size
ok - indexbuf of an unfinished call continues at bytes
ok - the download needed more than one call
ok - the final call reports finished with an empty indexbuf
ok - the part file equals the one-shot download
ok - no temporary file was used
ok - one call is enough without a time limit
ok - a zero-byte media finishes at once with an empty file
ok - offset 0 replaces the content of an existing part file
ok - a relative path works
ok - a file:// path works
ok - a new part file is created according to the umask
