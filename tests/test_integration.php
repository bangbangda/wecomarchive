<?php
/**
 * Optional integration test against the real WeCom API.
 *
 * Required env (skipped when any is missing):
 *   WECOM_CORPID, WECOM_SECRET   chat archive credentials (the egress IP must be whitelisted)
 *   WECOM_SDKFILEID              sdkfileid of a media message
 *   WECOM_MD5                    md5sum of that media, from the message body
 * Optional env:
 *   WECOM_FILESIZE               filesize from the message body
 *   WECOM_PROXY, WECOM_PASSWD    proxy settings
 *
 * A file of a few hundred MB is the interesting case.
 */

require __DIR__ . '/helpers.php';

echo "=== WeComArchive Integration Test (real API) ===\n\n";

$env = [];
foreach (['WECOM_CORPID', 'WECOM_SECRET', 'WECOM_SDKFILEID', 'WECOM_MD5'] as $name) {
    $value = getenv($name);
    if ($value === false || $value === '') {
        skip("Integration test", "{$name} not set");
        finish();
    }
    $env[$name] = $value;
}

$expectedMd5 = strtolower($env['WECOM_MD5']);
$expectedSize = getenv('WECOM_FILESIZE') !== false ? (int)getenv('WECOM_FILESIZE') : null;
$options = ['timeout' => 30];
foreach (['proxy' => 'WECOM_PROXY', 'passwd' => 'WECOM_PASSWD'] as $option => $name) {
    if (getenv($name) !== false) {
        $options[$option] = getenv($name);
    }
}

$archive = new WeComArchive(['corpid' => $env['WECOM_CORPID'], 'secret' => $env['WECOM_SECRET']]);
$dir = make_temp_dir('wecomarchive-it');
$target = "{$dir}/media.bin";

if (function_exists('memory_reset_peak_usage')) {
    memory_reset_peak_usage();
}
$peakBefore = memory_get_peak_usage(true);
$start = microtime(true);
$e = catch_exception(function () use ($archive, $env, $target, $options, $expectedMd5, &$written) {
    $written = $archive->saveMediaData($env['WECOM_SDKFILEID'], $target, $options + ['md5' => $expectedMd5]);
});
$seconds = microtime(true) - $start;
$peakDelta = memory_get_peak_usage(true) - $peakBefore;

test("saveMediaData() downloads the media", $e === null, describe_exception($e));

if ($e === null) {
    $size = filesize($target);
    printf("       %.1f MB in %.2fs, PHP peak +%.1f MB\n", $size / 1048576, $seconds, $peakDelta / 1048576);
    test("returned byte count matches the file", $written === $size, "returned {$written}, file {$size}");
    if ($expectedSize !== null) {
        test("file size matches WECOM_FILESIZE", $size === $expectedSize, "got {$size}, expected {$expectedSize}");
    }
    test("file md5 matches WECOM_MD5", md5_file($target) === $expectedMd5);
    test("PHP peak memory grows by < 16 MB", $peakDelta < (16 << 20), sprintf('%.1f MB', $peakDelta / 1048576));
    test("no temporary file is left", leftover_parts($target) === []);

    // These download the whole file again (and getMediaData() holds it in memory), so only for small files
    if ($size <= (64 << 20)) {
        $other = "{$dir}/mismatch.bin";
        $e = catch_exception(function () use ($archive, $env, $other, $options) {
            $archive->saveMediaData($env['WECOM_SDKFILEID'], $other, $options + ['md5' => str_repeat('0', 32)]);
        });
        test(
            "a wrong md5 throws WECOM_ERR_MD5 and leaves no file",
            $e && $e->getCode() === WECOM_ERR_MD5 && !file_exists($other) && leftover_parts($other) === [],
            describe_exception($e)
        );

        $data = $archive->getMediaData($env['WECOM_SDKFILEID'], $options);
        test("getMediaData() returns the same content", md5($data) === $expectedMd5);
        unset($data);
    } else {
        skip("md5 mismatch and getMediaData() comparison", "file larger than 64 MB");
    }
}

remove_dir($dir);

finish();
