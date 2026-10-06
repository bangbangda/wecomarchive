--TEST--
Dedicated error codes: constants, SDK not initialized, missing outindexbuf, max_execution_time
--EXTENSIONS--
wecomarchive
--SKIPIF--
<?php require __DIR__ . '/skipif.inc'; ?>
--FILE--
<?php
require __DIR__ . '/mock.inc';

foreach (['WECOM_ERR_TIMEOUT' => 20004, 'WECOM_ERR_EXEC_TIME' => 20005, 'WECOM_ERR_INDEXBUF' => 20006,
          'WECOM_ERR_NOT_INIT' => 20007, 'WECOM_ERR_RESUME' => 20008] as $name => $value) {
    check("{$name} === {$value}", defined($name) && constant($name) === $value);
}

// An object whose constructor never ran has no SDK instance
$bare = (new ReflectionClass('WeComArchive'))->newInstanceWithoutConstructor();
$dir = mock_dir();
foreach ([
    'getChatData' => fn() => $bare->getChatData(0, 10),
    'getMediaData' => fn() => $bare->getMediaData('x'),
    'saveMediaData' => fn() => $bare->saveMediaData('x', "{$dir}/x.bin"),
    'saveMediaDataPart' => fn() => $bare->saveMediaDataPart('x', "{$dir}/x.part"),
] as $method => $fn) {
    $e = caught($fn);
    check("{$method}() on an uninitialized object throws WECOM_ERR_NOT_INIT", $e && $e->getCode() === WECOM_ERR_NOT_INIT, describe($e));
}

$archive = mock_archive();
$src = "{$dir}/two.src";
mock_source($src, 2 * 1024);
$target = "{$dir}/two.bin";
file_put_contents($target, 'keep me');

$e = caught(function () use ($archive, $src, $target) {
    $archive->saveMediaData(mock_spec($src, ['chunk' => 1024, 'no_outindex' => 0]), $target);
});
check('an unfinished chunk without outindexbuf throws WECOM_ERR_INDEXBUF', $e && $e->getCode() === WECOM_ERR_INDEXBUF, describe($e));
check('target is untouched and no temporary file is left', file_get_contents($target) === 'keep me' && mock_parts($target) === []);

$part = "{$dir}/two.part";
$e = caught(function () use ($archive, $src, $part) {
    $archive->saveMediaDataPart(mock_spec($src, ['chunk' => 1024, 'no_outindex' => 0]), $part);
});
check('saveMediaDataPart() throws WECOM_ERR_INDEXBUF as well', $e && $e->getCode() === WECOM_ERR_INDEXBUF, describe($e));
check('the part file keeps the chunk that was written', filesize($part) === 1024);

// max_execution_time (CPU time on Linux): the download stops before the next chunk, the engine then
// raises its fatal error, so only the message can be observed. Child processes, so the fatal error does not end this test.
$src = "{$dir}/busy.src";
mock_source($src, 32 << 20);
$child = <<<'PHP'
$archive = new WeComArchive(['corpid' => 'mock', 'secret' => 'mock', 'lib_path' => $argv[1]]);
if ($argv[4] === 'part') { $archive->saveMediaDataPart($argv[2], $argv[3]); } else { $archive->saveMediaData($argv[2], $argv[3]); }
echo "finished";
PHP;
$run = function (string $mode, string $path) use ($child, $src) {
    $cmd = mock_php_cmd() . ' -d max_execution_time=1 -r ' . escapeshellarg($child) . ' ' . escapeshellarg(mock_lib()) . ' ' . escapeshellarg(mock_spec($src, ['chunk' => 64]))
        . ' ' . escapeshellarg($path) . ' ' . $mode . ' 2>&1';
    return (string)shell_exec($cmd);
};
$target = "{$dir}/busy.bin";
$output = $run('save', $target);
check('max_execution_time stops saveMediaData() with the engine fatal error',
    stripos($output, 'maximum execution time') !== false && strpos($output, 'finished') === false, trim($output));
check('saveMediaData() removed its temporary file first', !file_exists($target) && mock_parts($target) === []);

$part = "{$dir}/busy.part";
$output = $run('part', $part);
check('max_execution_time stops saveMediaDataPart() with the engine fatal error',
    stripos($output, 'maximum execution time') !== false && strpos($output, 'finished') === false, trim($output));
clearstatcache();
check('saveMediaDataPart() keeps the bytes written so far', is_file($part) && filesize($part) > 0 && filesize($part) % 64 === 0, (string)@filesize($part));
?>
--EXPECT--
ok - WECOM_ERR_TIMEOUT === 20004
ok - WECOM_ERR_EXEC_TIME === 20005
ok - WECOM_ERR_INDEXBUF === 20006
ok - WECOM_ERR_NOT_INIT === 20007
ok - WECOM_ERR_RESUME === 20008
ok - getChatData() on an uninitialized object throws WECOM_ERR_NOT_INIT
ok - getMediaData() on an uninitialized object throws WECOM_ERR_NOT_INIT
ok - saveMediaData() on an uninitialized object throws WECOM_ERR_NOT_INIT
ok - saveMediaDataPart() on an uninitialized object throws WECOM_ERR_NOT_INIT
ok - an unfinished chunk without outindexbuf throws WECOM_ERR_INDEXBUF
ok - target is untouched and no temporary file is left
ok - saveMediaDataPart() throws WECOM_ERR_INDEXBUF as well
ok - the part file keeps the chunk that was written
ok - max_execution_time stops saveMediaData() with the engine fatal error
ok - saveMediaData() removed its temporary file first
ok - max_execution_time stops saveMediaDataPart() with the engine fatal error
ok - saveMediaDataPart() keeps the bytes written so far
