# Changelog

## 1.4.0 — 2026-10-06

### Added
- `max_seconds` option for `saveMediaData()` and `getMediaData()`: a wall-clock limit for the whole call,
  measured on a monotonic clock, so it also covers time spent waiting for the network. It is checked before
  every chunk request (retries included) and cuts the per-chunk `timeout` down to the remaining time.
  Running out throws `WECOM_ERR_TIMEOUT` (20004); `saveMediaData()` removes its temporary file and leaves
  the target untouched.
- `saveMediaDataPart(string $sdkFileId, string $partPath, array $options = []): array` for resumable
  downloads. Each call appends to `$partPath` for at most `max_seconds`, flushes it to disk and returns
  `['finished' => bool, 'indexbuf' => string, 'bytes' => int]`; the next call continues from
  `indexbuf`/`offset`, from any process, after truncating bytes beyond `offset`. Errors keep the file.
  The optional `md5` is checked by reading the finished file back. The resume token is the SDK's
  `outindexbuf` (`Range:bytes=<start>-<end>` for SDK v3_20250205); a token whose start differs from
  `offset` is rejected with `WECOM_ERR_RESUME` (20008).
- Error codes `WECOM_ERR_EXEC_TIME` (20005, `max_execution_time` ran out), `WECOM_ERR_INDEXBUF` (20006,
  unfinished chunk without a new `outindexbuf`) and `WECOM_ERR_NOT_INIT` (20007, no SDK instance) replace
  the former code 0 of these exceptions.
- Mock SDK (`tests/mock-sdk`): `delay_ms`, `no_outindex` and `log` options; `outindexbuf` now has the
  official `Range:bytes=<start>-<end>` form. New phpt tests under `tests/phpt`.

### Changed
- Retries of chunks failing with 10001–10003 wait 200 ms × the retry number, never longer than the
  remaining `max_seconds`.
- `saveMediaData()` fsyncs the target directory after the rename.
- The `timeout` option of `getChatData()`, `getMediaData()`, `saveMediaData()` and `saveMediaDataPart()`
  accepts a positive int, float (rounded up) or numeric string; any other value throws `WECOM_ERR_PARAM`
  instead of silently using the default of 5 seconds.
- Allocation failures of SDK buffers now carry `WECOM_ERR_SYSTEM` instead of code 0.

## 1.3.0 — 2026-10-05

- `saveMediaData()`: stream media files to disk chunk by chunk (atomic rename, retries, md5 check).
- New error codes `WECOM_ERR_WRITE` (20001), `WECOM_ERR_MD5` (20002), `WECOM_ERR_PATH` (20003).
- `getMediaData()`: fix per-chunk memory leak, return `''` for empty files, optional `retries`.

## 1.2.1

- Fix PEM private keys with leading metadata being mistaken for file paths.

## 1.1.0

- `decryptChatItem()` and the `private_keys` option: pick the private key by `publickey_ver`.

## 1.0.0

- Initial release: `getChatData()`, `decryptData()`, `getMediaData()`, PIE installation.
