--TEST--
saveMediaDataPart(): writes, truncates and checks the md5 of the file the path names now; filesize() sees the result
--EXTENSIONS--
wecomarchive
--SKIPIF--
<?php require __DIR__ . '/skipif.inc'; ?>
--FILE--
<?php
require __DIR__ . '/mock.inc';
$archive = mock_archive();
$dir = mock_dir();
$data = str_repeat('N', 100);
$src = "{$dir}/small.src";
file_put_contents($src, $data);
$cat = fn(string $file) => (string)shell_exec('cat ' . escapeshellarg($file));
$relink = fn(string $to, string $link) => shell_exec('ln -sfn ' . escapeshellarg($to) . ' ' . escapeshellarg($link));

// The part path is a symlink that another process points elsewhere after PHP has cached where it
// led (ZTS builds used to open the cached file through the realpath cache and read the md5 back
// from the new one, reporting a false WECOM_ERR_MD5)
foreach (['without md5' => [], 'with md5' => ['md5' => md5($data)]] as $label => $options) {
    file_put_contents("{$dir}/A", 'AAAA');
    file_put_contents("{$dir}/B", 'BBBB');
    $relink("{$dir}/A", "{$dir}/link.part");
    realpath("{$dir}/link.part");
    file_get_contents("{$dir}/link.part");
    $relink("{$dir}/B", "{$dir}/link.part");  // not through PHP, so PHP's caches are not cleared
    $e = caught(fn() => $archive->saveMediaDataPart(mock_spec($src), "{$dir}/link.part", $options));
    check("{$label}: no exception", $e === null, describe($e));
    check("{$label}: the file the link names now receives the data", $cat("{$dir}/B") === $data, $cat("{$dir}/B"));
    check("{$label}: the file it used to name is untouched", $cat("{$dir}/A") === 'AAAA', $cat("{$dir}/A"));
}

// Continuing through a symlink truncates and appends to its target
$size = 3 * MOCK_CHUNK;
$big = "{$dir}/big.src";
$md5 = mock_source($big, $size);
file_put_contents("{$dir}/real.part", file_get_contents($big, false, null, 0, MOCK_CHUNK) . 'junk');
$relink("{$dir}/real.part", "{$dir}/via-link.part");
$r = $archive->saveMediaDataPart(mock_spec($big), "{$dir}/via-link.part",
    ['offset' => MOCK_CHUNK, 'indexbuf' => 'Range:bytes=524288-1048575', 'md5' => $md5]);
check('continuing through a symlink completes its target', $r['finished'] && md5_file("{$dir}/real.part") === $md5 && is_link("{$dir}/via-link.part"));

// filesize() right after each call, with no clearstatcache() in between
$part = "{$dir}/cache.part";
file_put_contents($part, str_repeat('x', 3000));
$before = filesize($part);
$r = $archive->saveMediaDataPart(mock_spec($big, ['delay_ms' => 30]), $part, ['max_seconds' => 0.01]);
check('starting over: filesize() sees the new size', $before === 3000 && filesize($part) === $r['bytes'] && $r['bytes'] === MOCK_CHUNK,
    "before {$before}, after " . filesize($part) . ", bytes {$r['bytes']}");
$r = $archive->saveMediaDataPart(mock_spec($big), $part, ['offset' => $r['bytes'], 'indexbuf' => $r['indexbuf']]);
check('continuing: filesize() sees the new size', filesize($part) === $size && $r['bytes'] === $size, 'after ' . filesize($part));
$e = caught(fn() => $archive->saveMediaDataPart(mock_spec($big, ['error' => 10010]), $part, ['offset' => 1000, 'indexbuf' => 'Range:bytes=1000-2000']));
check('a failed call: filesize() sees the truncation', $e && $e->getCode() === 10010 && filesize($part) === 1000, describe($e) . ', size ' . filesize($part));

// An existing part file keeps its mode when the download starts over
chmod($part, 0600);
$archive->saveMediaDataPart(mock_spec($src), $part);
check('starting over keeps the mode of an existing part file', (fileperms($part) & 0777) === 0600, sprintf('%o', fileperms($part) & 0777));

// A symlink to a file outside open_basedir is refused before anything is opened (child process)
mkdir("{$dir}/allowed");
mkdir("{$dir}/outside");
file_put_contents("{$dir}/outside/secret", 'keep me');
file_put_contents("{$dir}/allowed/s.src", $data);
symlink("{$dir}/outside/secret", "{$dir}/allowed/evil.part");
$child = <<<'PHP'
$archive = new WeComArchive(['corpid' => 'mock', 'secret' => 'mock', 'lib_path' => $argv[1]]);
try {
    $archive->saveMediaDataPart('file=' . $argv[2], $argv[3]);
    echo 'no exception';
} catch (Exception $e) {
    echo $e->getCode();
}
PHP;
$cmd = mock_php_cmd() . ' -d open_basedir=' . escapeshellarg("{$dir}/allowed" . PATH_SEPARATOR . dirname(mock_lib())) . ' -r ' . escapeshellarg($child) . ' '
    . escapeshellarg(mock_lib()) . ' ' . escapeshellarg("{$dir}/allowed/s.src") . ' ' . escapeshellarg("{$dir}/allowed/evil.part") . ' 2>&1';
$output = trim((string)shell_exec($cmd));
check('a symlink out of open_basedir is refused with WECOM_ERR_PATH', $output === (string)WECOM_ERR_PATH && $cat("{$dir}/outside/secret") === 'keep me', $output);
?>
--EXPECT--
ok - without md5: no exception
ok - without md5: the file the link names now receives the data
ok - without md5: the file it used to name is untouched
ok - with md5: no exception
ok - with md5: the file the link names now receives the data
ok - with md5: the file it used to name is untouched
ok - continuing through a symlink completes its target
ok - starting over: filesize() sees the new size
ok - continuing: filesize() sees the new size
ok - a failed call: filesize() sees the truncation
ok - starting over keeps the mode of an existing part file
ok - a symlink out of open_basedir is refused with WECOM_ERR_PATH
