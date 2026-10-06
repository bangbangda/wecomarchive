<?php
/**
 * Verifies against the real WeCom API that GetMediaData()'s outindexbuf can be reused by a later
 * process (what saveMediaDataPart() relies on), using tests/indexbuf-probe.c.
 *
 * Required env: WECOM_CORPID, WECOM_SECRET, WECOM_SDKFILEID (a media of more than 512 KB).
 * Optional env: WECOM_MD5, WECOM_PROBE_DELAY (seconds to wait before the last check, default 150),
 *               WECOM_SDK_LIB (library path).
 *
 * Usage (inside the test container, which has gcc and the SDK):
 *   php tests/indexbuf-probe.php
 *
 * Every step prints one JSON line. The interesting fields: "matches_full_at_offset" must be true for
 * every resume_* and synth_* step that returns ret 0, and "outindexbuf" shows the token format.
 */

foreach (['WECOM_CORPID', 'WECOM_SECRET', 'WECOM_SDKFILEID'] as $name) {
    if (getenv($name) === false || getenv($name) === '') {
        fwrite(STDERR, "{$name} not set\n");
        exit(1);
    }
}
$fileId = getenv('WECOM_SDKFILEID');
$dir = sys_get_temp_dir() . '/wecomarchive-probe-' . bin2hex(random_bytes(4));
mkdir($dir, 0700, true);
$probe = "{$dir}/indexbuf-probe";
passthru('gcc -O1 -o ' . escapeshellarg($probe) . ' ' . escapeshellarg(__DIR__ . '/indexbuf-probe.c') . ' -ldl', $rc);
if ($rc !== 0) {
    exit(1);
}

$out = function (array $d) { echo json_encode($d, JSON_UNESCAPED_SLASHES), "\n"; };
$run = function (array $args, string $outfile) use ($probe, $fileId) {
    $cmd = escapeshellarg($probe) . ' ' . implode(' ', array_map('escapeshellarg', $args)) . ' ' . escapeshellarg($outfile) . ' 2>&1 >/dev/null';
    exec($cmd, $lines);
    return array_values(array_filter(array_map(fn($l) => json_decode($l, true), $lines)));
};

// A: the whole file in one process, recording every outindexbuf
$t = microtime(true);
$full = $run(['full', $fileId], "{$dir}/full.bin");
$fullData = file_get_contents("{$dir}/full.bin");
$out(['step' => 'full', 'calls' => count($full), 'seconds' => round(microtime(true) - $t, 2), 'file_size' => strlen($fullData),
    'md5_ok' => getenv('WECOM_MD5') ? md5($fullData) === strtolower(getenv('WECOM_MD5')) : null,
    'tokens' => array_map(fn($c) => ['indexbuf' => $c['indexbuf'], 'ret' => $c['ret'], 'data_len' => $c['data_len'] ?? null,
        'is_finish' => $c['is_finish'] ?? null, 'outindexbuf' => $c['outindexbuf'] ?? null, 'out_len' => $c['out_len'] ?? null], $full)]);
if (count($full) < 2 || end($full)['ret'] !== 0) {
    fwrite(STDERR, "need a media of more than one chunk that downloads successfully\n");
    exit(1);
}

$check = function (string $label, string $indexbuf, int $expectOffset) use ($run, $dir, $fullData, $out) {
    $c = $run(['one', $indexbuf], "{$dir}/one.bin")[0] ?? null;
    $got = file_get_contents("{$dir}/one.bin");
    $out(['step' => $label, 'indexbuf' => $indexbuf, 'ret' => $c['ret'] ?? null, 'data_len' => $c['data_len'] ?? null,
        'is_finish' => $c['is_finish'] ?? null, 'outindexbuf' => $c['outindexbuf'] ?? null, 'expect_offset' => $expectOffset,
        'matches_full_at_offset' => strlen($got) > 0 && $got === substr($fullData, $expectOffset, strlen($got)), 'at' => date('c')]);
};

// B: every token reused by a new process
$offset = 0;
foreach ($full as $i => $c) {
    if (!empty($c['is_finish'])) {
        break;
    }
    $offset += $c['data_len'];
    $check("resume_new_process_call{$i}", $c['outindexbuf'], $offset);
}

// C: tokens the extension never produced: does the SDK honour any byte range?
$size = strlen($fullData);
$check('synth_arbitrary_start', 'Range:bytes=1000-1999', 1000);
$check('synth_open_end', 'Range:bytes=700000-', 700000);
$check('synth_unaligned_512k', 'Range:bytes=700000-1224287', 700000);
$check('synth_last_byte', 'Range:bytes=' . ($size - 1) . '-' . ($size + 524286), $size - 1);
$check('synth_beyond_eof', 'Range:bytes=' . ($size + 10) . '-' . ($size + 1000), 0);
$check('garbage_token', 'idx-524288', 0);

// D: the first token again after a delay, in yet another process
$delay = (int)(getenv('WECOM_PROBE_DELAY') ?: 150);
$out(['step' => 'sleep', 'seconds' => $delay]);
sleep($delay);
$check('resume_after_delay', $full[0]['outindexbuf'], $full[0]['data_len']);

array_map('unlink', glob("{$dir}/*"));
rmdir($dir);
