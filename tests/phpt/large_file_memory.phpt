--TEST--
Peak memory of saveMediaData() and saveMediaDataPart() does not grow with the file size (1 GB by default)
--EXTENSIONS--
wecomarchive
--SKIPIF--
<?php
require __DIR__ . '/skipif.inc';
if (getenv('WECOM_MOCK_LARGE_MB') !== false && (int)getenv('WECOM_MOCK_LARGE_MB') <= 0) die('skip WECOM_MOCK_LARGE_MB=0');
if (disk_free_space(sys_get_temp_dir()) < 2.5 * ((int)(getenv('WECOM_MOCK_LARGE_MB') ?: 1024) << 20)) die('skip not enough free disk space');
?>
--FILE--
<?php
require __DIR__ . '/mock.inc';
$archive = mock_archive();
$dir = mock_dir();
$mb = (int)(getenv('WECOM_MOCK_LARGE_MB') ?: 1024);
$size = $mb << 20;
$src = "{$dir}/large.src";
$md5 = mock_source($src, $size);
$limit = 16 << 20;

// Warm up: first-use allocations of the extension and the libraries
$warm = "{$dir}/warm.src";
mock_source($warm, 1 << 20);
$archive->saveMediaData(mock_spec($warm), "{$dir}/warm.bin");
$archive->saveMediaDataPart(mock_spec($warm), "{$dir}/warm.part");

$measure = function (string $label, callable $fn) use ($limit, $mb) {
    if (function_exists('memory_reset_peak_usage')) {
        memory_reset_peak_usage();
    }
    gc_collect_cycles();
    $peakBefore = memory_get_peak_usage(true);
    $hwmBefore = vm_hwm();
    $t0 = microtime(true);
    $fn();
    $seconds = microtime(true) - $t0;
    $peakDelta = memory_get_peak_usage(true) - $peakBefore;
    $hwmDelta = $hwmBefore === null ? null : vm_hwm() - $hwmBefore;
    printf("# %s %d MB: %.2fs, PHP peak +%.1f MB, process peak RSS +%s MB\n", $label, $mb, $seconds, $peakDelta / 1048576,
        $hwmDelta === null ? 'n/a' : sprintf('%.1f', $hwmDelta / 1048576));
    check("{$label}: PHP peak memory grows by < 16 MB", $peakDelta < $limit, sprintf('%.1f MB', $peakDelta / 1048576));
    check("{$label}: process peak RSS grows by < 16 MB", $hwmDelta === null || $hwmDelta < $limit, sprintf('%.1f MB', ($hwmDelta ?? 0) / 1048576));
};

$target = "{$dir}/large.bin";
$measure("saveMediaData()", function () use ($archive, $src, $target, $md5, $size) {
    $written = $archive->saveMediaData(mock_spec($src), $target, ['md5' => $md5]);
    check('saveMediaData() wrote the whole file', $written === $size && filesize($target) === $size);
});
unlink($target);

$part = "{$dir}/large.part";
$calls = 0;
$measure("saveMediaDataPart()", function () use ($archive, $src, $part, $md5, $size, &$calls) {
    $state = ['indexbuf' => '', 'offset' => 0];
    do {
        $r = $archive->saveMediaDataPart(mock_spec($src, ['delay_ms' => 1]), $part, $state + ['max_seconds' => 0.3, 'md5' => $md5]);
        $calls++;
        $state = ['indexbuf' => $r['indexbuf'], 'offset' => $r['bytes']];
    } while (!$r['finished'] && $calls < 1000);
    check('saveMediaDataPart() finished the whole file', $r['finished'] && $r['bytes'] === $size);
});
check('the resumed file matches the source', md5_file($part) === $md5);
printf("# saveMediaDataPart() needed %d calls\n", $calls);
check('saveMediaDataPart() was called more than once', $calls > 1 || $mb < 256, (string)$calls);
?>
--EXPECTF--
ok - saveMediaData() wrote the whole file
# saveMediaData() %d MB: %fs, PHP peak +%s MB, process peak RSS +%s MB
ok - saveMediaData(): PHP peak memory grows by < 16 MB
ok - saveMediaData(): process peak RSS grows by < 16 MB
ok - saveMediaDataPart() finished the whole file
# saveMediaDataPart() %d MB: %fs, PHP peak +%s MB, process peak RSS +%s MB
ok - saveMediaDataPart(): PHP peak memory grows by < 16 MB
ok - saveMediaDataPart(): process peak RSS grows by < 16 MB
ok - the resumed file matches the source
# saveMediaDataPart() needed %d calls
ok - saveMediaDataPart() was called more than once
