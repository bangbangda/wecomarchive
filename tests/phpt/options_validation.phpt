--TEST--
max_seconds and timeout options: accepted forms and rejected values (WECOM_ERR_PARAM, never a silent default)
--EXTENSIONS--
wecomarchive
--SKIPIF--
<?php require __DIR__ . '/skipif.inc'; ?>
--FILE--
<?php
require __DIR__ . '/mock.inc';
$archive = mock_archive();
$dir = mock_dir();
$src = "{$dir}/small.src";
mock_source($src, 4096);
$target = "{$dir}/small.bin";
$log = "{$dir}/calls.log";
$label = function ($value): string {
    if (is_float($value) && !is_finite($value)) {
        return is_nan($value) ? 'NAN' : 'INF';
    }
    return is_object($value) ? 'object' : json_encode($value);
};

$timeoutOf = function (array $options) use ($archive, $src, $target, $log) {
    @unlink($log);
    $archive->saveMediaData(mock_spec($src, ['log' => $log]), $target, $options);
    return mock_log($log)[0][1];
};

check('timeout defaults to 5', $timeoutOf([]) === 5);
check('timeout null means the default', $timeoutOf(['timeout' => null]) === 5);
check('timeout int is passed as is', $timeoutOf(['timeout' => 30]) === 30);
check('timeout float is rounded up', $timeoutOf(['timeout' => 2.5]) === 3);
check('timeout numeric string is accepted', $timeoutOf(['timeout' => '7']) === 7);
check('timeout numeric float string is rounded up', $timeoutOf(['timeout' => '2.2']) === 3);

foreach (['abc', '', true, false, [], 0, -3, 0.0, -0.5, 1e12, NAN, INF, new stdClass] as $bad) {
    $e = caught(function () use ($archive, $src, $target, $bad) {
        $archive->saveMediaData(mock_spec($src), $target, ['timeout' => $bad]);
    });
    check('timeout ' . $label($bad) . ' is rejected with WECOM_ERR_PARAM',
        $e && $e->getCode() === WECOM_ERR_PARAM && strpos($e->getMessage(), "'timeout'") !== false, describe($e));
}

$e = caught(function () use ($archive, $src) {
    $archive->getMediaData(mock_spec($src), ['timeout' => 'abc']);
});
check('getMediaData() rejects a bad timeout too', $e && $e->getCode() === WECOM_ERR_PARAM, describe($e));
$e = caught(function () use ($archive) {
    $archive->getChatData(0, 10, ['timeout' => 'abc']);
});
check('getChatData() rejects a bad timeout too', $e && $e->getCode() === WECOM_ERR_PARAM && strpos($e->getMessage(), "'timeout'") !== false, describe($e));

check('max_seconds null means unlimited', $archive->saveMediaData(mock_spec($src), $target, ['max_seconds' => null]) === 4096);
check('max_seconds int is accepted', $archive->saveMediaData(mock_spec($src), $target, ['max_seconds' => 60]) === 4096);
check('max_seconds float is accepted', $archive->saveMediaData(mock_spec($src), $target, ['max_seconds' => 0.75]) === 4096);

foreach ([0, -1, 0.0, -0.1, '5', '', true, [], NAN, INF] as $bad) {
    $e = caught(function () use ($archive, $src, $target, $bad) {
        $archive->saveMediaData(mock_spec($src), $target, ['max_seconds' => $bad]);
    });
    check('max_seconds ' . $label($bad) . ' is rejected with WECOM_ERR_PARAM',
        $e && $e->getCode() === WECOM_ERR_PARAM && strpos($e->getMessage(), "'max_seconds'") !== false, describe($e));
}
// Options held by reference are read through the reference
$t = 7;
check('timeout by reference is used', $timeoutOf(['timeout' => &$t]) === 7);
$n = null;
check('timeout by reference to null means the default', $timeoutOf(['timeout' => &$n]) === 5);
$bad = 'abc';
$e = caught(function () use ($archive, $src, $target, &$bad) {
    $archive->saveMediaData(mock_spec($src), $target, ['timeout' => &$bad]);
});
check('timeout by reference is still validated', $e && $e->getCode() === WECOM_ERR_PARAM, describe($e));
$ms = 0.5;
$e = caught(function () use ($archive, $src, $target, &$ms) {
    $archive->saveMediaData(mock_spec($src, ['delay_ms' => 300, 'chunk' => 1024]), $target, ['max_seconds' => &$ms]);
});
check('max_seconds by reference is used', $e && $e->getCode() === WECOM_ERR_TIMEOUT, describe($e));
$rt = 1;
$bytes = $archive->saveMediaData(mock_spec($src, ['chunk' => 1024, 'fail_at' => 1, 'fail_times' => 1]), $target, ['retries' => &$rt]);
check('retries by reference is used', $bytes === 4096);
$sum = md5_file($src);
check('md5 by reference is used', $archive->saveMediaData(mock_spec($src), $target, ['md5' => &$sum]) === 4096);
$wrong = str_repeat('0', 32);
$e = caught(function () use ($archive, $src, $target, &$wrong) {
    $archive->saveMediaData(mock_spec($src), $target, ['md5' => &$wrong]);
});
check('md5 by reference is checked', $e && $e->getCode() === WECOM_ERR_MD5, describe($e));
$part = "{$dir}/ref.part";
$r = $archive->saveMediaDataPart(mock_spec($src, ['chunk' => 1024, 'delay_ms' => 30]), $part, ['max_seconds' => 0.01]);
$off = $r['bytes'];
$tok = $r['indexbuf'];
$r = $archive->saveMediaDataPart(mock_spec($src, ['chunk' => 1024]), $part, ['offset' => &$off, 'indexbuf' => &$tok]);
check('offset and indexbuf by reference are used', $r['finished'] && $r['bytes'] === 4096 && md5_file($part) === $sum, json_encode($r));
$ct = 'abc';
$e = caught(function () use ($archive, &$ct) {
    $archive->getChatData(0, 10, ['timeout' => &$ct]);
});
check('getChatData() validates timeout by reference', $e && $e->getCode() === WECOM_ERR_PARAM, describe($e));
?>
--EXPECT--
ok - timeout defaults to 5
ok - timeout null means the default
ok - timeout int is passed as is
ok - timeout float is rounded up
ok - timeout numeric string is accepted
ok - timeout numeric float string is rounded up
ok - timeout "abc" is rejected with WECOM_ERR_PARAM
ok - timeout "" is rejected with WECOM_ERR_PARAM
ok - timeout true is rejected with WECOM_ERR_PARAM
ok - timeout false is rejected with WECOM_ERR_PARAM
ok - timeout [] is rejected with WECOM_ERR_PARAM
ok - timeout 0 is rejected with WECOM_ERR_PARAM
ok - timeout -3 is rejected with WECOM_ERR_PARAM
ok - timeout 0 is rejected with WECOM_ERR_PARAM
ok - timeout -0.5 is rejected with WECOM_ERR_PARAM
ok - timeout 1000000000000 is rejected with WECOM_ERR_PARAM
ok - timeout NAN is rejected with WECOM_ERR_PARAM
ok - timeout INF is rejected with WECOM_ERR_PARAM
ok - timeout object is rejected with WECOM_ERR_PARAM
ok - getMediaData() rejects a bad timeout too
ok - getChatData() rejects a bad timeout too
ok - max_seconds null means unlimited
ok - max_seconds int is accepted
ok - max_seconds float is accepted
ok - max_seconds 0 is rejected with WECOM_ERR_PARAM
ok - max_seconds -1 is rejected with WECOM_ERR_PARAM
ok - max_seconds 0 is rejected with WECOM_ERR_PARAM
ok - max_seconds -0.1 is rejected with WECOM_ERR_PARAM
ok - max_seconds "5" is rejected with WECOM_ERR_PARAM
ok - max_seconds "" is rejected with WECOM_ERR_PARAM
ok - max_seconds true is rejected with WECOM_ERR_PARAM
ok - max_seconds [] is rejected with WECOM_ERR_PARAM
ok - max_seconds NAN is rejected with WECOM_ERR_PARAM
ok - max_seconds INF is rejected with WECOM_ERR_PARAM
ok - timeout by reference is used
ok - timeout by reference to null means the default
ok - timeout by reference is still validated
ok - max_seconds by reference is used
ok - retries by reference is used
ok - md5 by reference is used
ok - md5 by reference is checked
ok - offset and indexbuf by reference are used
ok - getChatData() validates timeout by reference
