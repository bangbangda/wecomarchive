# Changelog

## 1.4.0 — 2026-10-06

### Added
- `max_seconds` option for `saveMediaData()` and `getMediaData()`: a wall-clock budget for the whole call,
  measured on a monotonic clock, so it also covers time spent waiting for the network. It is checked before
  every chunk request (retries included) and cuts the per-chunk `timeout` down to the remaining time, at
  least 1 s. It is not a hard limit: the last chunk can end up to about 1 s late, and the final `fsync`
  (and the md5 read-back of `saveMediaDataPart()`) is not counted. Running out throws `WECOM_ERR_TIMEOUT`
  (20004); `saveMediaData()` removes its temporary file and leaves the target untouched.
- `saveMediaDataPart(string $sdkFileId, string $partPath, array $options = []): array` for resumable
  downloads. Each call appends to `$partPath` for at most `max_seconds`, flushes it to disk and returns
  `['finished' => bool, 'indexbuf' => string, 'bytes' => int]`; the next call continues from
  `indexbuf`/`offset`, from any process, after truncating bytes beyond `offset`. An exception never
  deletes the file, though it may already have been emptied or truncated to `offset`. The file is opened
  only after checking that it is the one that was inspected (a path replaced in between is refused with
  `WECOM_ERR_PATH`), the optional `md5` is checked by reading back that same file, and PHP's stat cache is
  cleared before returning. The resume token is the SDK's
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
- `saveMediaData()` fsyncs the target directory after the rename (best effort; failures are not reported).
- Options passed by reference (`['timeout' => &$t]`) are read through the reference, and a reference to
  null counts as not given, for every option of `getChatData()`, `getMediaData()`, `saveMediaData()` and
  `saveMediaDataPart()`. Before, `retries` and `md5` by reference threw `WECOM_ERR_PARAM` and the other
  options by reference were ignored.
- The `timeout` option of `getChatData()`, `getMediaData()`, `saveMediaData()` and `saveMediaDataPart()`
  is validated; see the upgrade notes below.
- Allocation failures of SDK buffers now carry `WECOM_ERR_SYSTEM` instead of code 0.

### Upgrade notes
- `timeout` must now be a positive number of seconds, at most 2147483647: an int, a float (rounded up to
  whole seconds) or a numeric string. What changes for existing callers:

  | Value passed | 1.3.0 | 1.4.0 |
  |---|---|---|
  | int > 0 | passed to the SDK | passed to the SDK |
  | int 0 or negative | passed to the SDK as is | throws `WECOM_ERR_PARAM` |
  | int > 2147483647 | truncated to a C `int` | throws `WECOM_ERR_PARAM` |
  | float, e.g. `2.5` | ignored, 5 s used | rounded up, 3 s used |
  | numeric string, e.g. `'30'` | ignored, 5 s used | 30 s used |
  | other types (`'abc'`, bool, array) | ignored, 5 s used | throws `WECOM_ERR_PARAM` |
  | `null` or not set | 5 s | 5 s |

  Callers that passed `0` to mean "no timeout" must pass an explicit number of seconds instead.
- Exceptions that used to carry code 0 now carry `WECOM_ERR_EXEC_TIME`, `WECOM_ERR_INDEXBUF`,
  `WECOM_ERR_NOT_INIT` or `WECOM_ERR_SYSTEM`; code that compared the code with 0 must be updated.

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
