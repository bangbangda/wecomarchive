# wecomarchive

English | **[简体中文](README_CN.md)**

> 📖 **中国用户请查看 [中文文档](README_CN.md) →**

PHP extension for WeCom (WeChat Work) Chat Archive functionality.

> **Install with [PIE](https://github.com/php/pie)**: `pie install bangbangda/wecomarchive`

## Features

- **Automatic SDK Download**: WeCom SDK is automatically downloaded during installation
- **Object-Oriented Interface**: Clean, modern PHP API for chat archive operations
- **Full Functionality**: Fetch messages, decrypt content, download media files (streamed to disk for large files)
- **Flexible Configuration**: Support for custom SDK paths and proxy settings

## Requirements

- PHP >= 8.0
- Linux (x86_64 or ARM64)
- OpenSSL >= 1.1.0

## Installation

### Method 1: PIE (Recommended)

[PIE](https://github.com/php/pie) is the modern way to install PHP extensions.

**Basic Installation (Automatic):**

```bash
# Install PIE if you haven't already
composer global require php/pie

# Install the extension (SDK will be downloaded automatically)
pie install bangbangda/wecomarchive
```

The WeCom SDK will be **automatically downloaded** during installation to `/usr/local/lib/`.

**Advanced Configuration:**

If you need to customize the SDK path or the automatic download fails:

```bash
# Specify custom SDK library path
pie install bangbangda/wecomarchive --with-wecomarchive-sdk-path=/custom/lib/path

# Then manually download SDK to your custom path
vendor/bin/download-sdk.sh --path /custom/lib/path
```

**Note:** If automatic download fails due to permission issues, you may need to run:

```bash
# Download SDK manually with sudo
sudo ./scripts/download-sdk.sh
```

### Method 2: Manual Installation

1. Download and extract the source:

```bash
git clone https://github.com/bangbangda/wecomarchive.git
cd wecomarchive
```

2. Build the extension (SDK will be downloaded automatically during configure):

```bash
phpize
./configure
make
sudo make install
```

**The WeCom SDK will be automatically downloaded to `/usr/local/lib/` during the `./configure` step.**

If you want to use a custom SDK path:

```bash
phpize
./configure --with-wecomarchive-sdk-path=/custom/lib/path
make
sudo make install
```

If automatic download fails, you can manually download the SDK first:

```bash
chmod +x scripts/download-sdk.sh
./scripts/download-sdk.sh
# Or with custom path
./scripts/download-sdk.sh --path /custom/path
```

3. Enable the extension in php.ini:

```ini
extension=wecomarchive.so
wecomarchive.sdk_lib_path=/usr/local/lib/libWeWorkFinanceSdk_C.so
```

## Configuration

| INI Setting | Default | Description |
|-------------|---------|-------------|
| `wecomarchive.sdk_lib_path` | `/usr/local/lib/libWeWorkFinanceSdk_C.so` | Path to the WeCom SDK library |

## Usage

### Basic Example

```php
<?php
// Initialize with your credentials. private_key accepts either PEM content or a file path
// (auto-detected — values starting with "-----BEGIN " are treated as PEM content).
$archive = new WeComArchive([
    'corpid' => 'your_corp_id',
    'secret' => 'your_secret',
    'private_key' => '/path/to/private.pem', // or raw PEM string
]);

// Fetch chat data
$response = $archive->getChatData(seq: 0, limit: 100);
$data = json_decode($response, true);

if ($data['errcode'] === 0) {
    foreach ($data['chatdata'] as $chat) {
        // Decrypt message
        $message = $archive->decryptData(
            $chat['encrypt_random_key'],
            $chat['encrypt_chat_msg']
        );
        $msgData = json_decode($message, true);
        print_r($msgData);
    }
}
```

### Multi-version Private Keys (Recommended)

WeCom supports rotating the chat-archive private key. Each chat item carries a `publickey_ver`
field. Configure `private_keys` and use `decryptChatItem()` to let the extension auto-select
the correct private key for each item:

```php
<?php
$archive = new WeComArchive([
    'corpid' => 'your_corp_id',
    'secret' => 'your_secret',
    // [publickey_ver => PEM content or file path]
    'private_keys' => [
        1 => '/path/to/key_v1.pem',
        2 => '/path/to/key_v2.pem',
        3 => "-----BEGIN PRIVATE KEY-----\n...",
    ],
]);

$response = $archive->getChatData(0, 100);
$data = json_decode($response, true);

foreach ($data['chatdata'] as $chat) {
    // Pass the whole chat item — extension picks the key by publickey_ver automatically.
    $message = $archive->decryptChatItem($chat);
    $msgData = json_decode($message, true);
    print_r($msgData);
}
```

If `publickey_ver` cannot be matched in `private_keys`, a clear exception is thrown
naming the missing version.

### Download Media Files

Use `saveMediaData()` to download media (images, voice, video, files) straight to disk. It writes
each chunk (at most 512 KB) as soon as it arrives, so memory use stays flat however large the file
is. **Use it for videos and files, which can be hundreds of MB.**

```php
<?php
// $msg is a decrypted message of type "file" (video, image and voice look alike)
$bytes = $archive->saveMediaData($msg['file']['sdkfileid'], '/data/media/' . $msg['msgid'], [
    'timeout' => 30,                    // per chunk
    'md5'     => $msg['file']['md5sum'], // optional integrity check
]);
```

The data goes to a temporary `<path>.part-<random>` file next to the target, which is flushed to disk
(`fsync`) and renamed onto `<path>` only after the whole download (and the md5 check) succeeded. On any
failure the temporary file is removed, `<path>` is left as it was, and an exception is thrown. These
guarantees assume the target directory is not moved or replaced while the download runs.

A file that is replaced keeps its owner, group, permissions and POSIX ACL. If they cannot be kept (for
example the file belongs to another user), the download fails with `WECOM_ERR_WRITE` before any data is
written and the file is left untouched. Replacing creates a new file, as `rename()` does, so other
extended attributes and hard links are not carried over, and security labels (SELinux, AppArmor) follow
the system policy for new files in that directory. A new file is created according to the umask and the
directory's default ACL, as with `file_put_contents()`.

`getMediaData()` returns the whole file as a string, so it holds the entire file in memory. It is
fine for small media such as images and voice messages:

```php
<?php
$mediaContent = $archive->getMediaData($sdkFileId, [
    'timeout' => 30,
]);
```

### Using Proxy

```php
<?php
$archive = new WeComArchive([
    'corpid' => 'your_corp_id',
    'secret' => 'your_secret',
    'private_key' => file_get_contents('/path/to/private.pem'),
]);

// With proxy
$response = $archive->getChatData(0, 100, [
    'proxy' => 'http://proxy.example.com:8080',
    'passwd' => 'user:password',
    'timeout' => 10,
]);
```

### Custom SDK Library Path

```php
<?php
$archive = new WeComArchive([
    'corpid' => 'your_corp_id',
    'secret' => 'your_secret',
    'lib_path' => '/custom/path/to/libWeWorkFinanceSdk_C.so',
]);
```

## API Reference

### WeComArchive Class

#### Constructor

```php
public function __construct(array $options)
```

**Options:**
- `corpid` (required): Your WeCom Corp ID
- `secret` (required): Chat archive secret
- `private_key` (optional): A single RSA private key. Accepts either PEM content or a file path (auto-detected). Used by `decryptData()`
- `private_keys` (optional): Multi-version key map `[publickey_ver => PEM-or-path]`. Used by `decryptChatItem()` for auto-selection
- `lib_path` (optional): Custom path to SDK library

> Detection rule: a value is treated as raw PEM content if it spans multiple lines (PEM bodies always contain newlines, file paths do not) or starts with `-----BEGIN `. Single-line values without the PEM header are treated as a file path. This tolerates PEM exports that have leading metadata (e.g. `Bag Attributes` lines from `openssl pkcs12`). `private_key` and `private_keys` may both be set — the former acts as a fallback for `decryptChatItem()`.

#### getChatData

```php
public function getChatData(int $seq = 0, int $limit = 100, array $options = []): string
```

Fetch chat messages.

**Parameters:**
- `$seq`: Starting sequence number (0 for first fetch)
- `$limit`: Maximum messages to fetch (1-1000)
- `$options`: Optional settings (proxy, passwd, timeout)

**Returns:** JSON string with chat data

#### decryptData

```php
public function decryptData(string $encryptRandomKey, string $encryptChatMsg): string
```

Decrypt a chat message using the `private_key` provided at construction time.

**Parameters:**
- `$encryptRandomKey`: The `encrypt_random_key` from chat data
- `$encryptChatMsg`: The `encrypt_chat_msg` from chat data

**Returns:** Decrypted message as JSON string

**Throws:** `WECOM_ERR_PRIKEY` if no `private_key` was configured.

#### decryptChatItem

```php
public function decryptChatItem(array $chatItem): string
```

Decrypt one chatdata item, auto-selecting the private key from `private_keys` by its `publickey_ver`.

**Parameters:**
- `$chatItem`: One element from the `chatdata` array returned by `getChatData()`. Must contain `encrypt_random_key` and `encrypt_chat_msg`; if `publickey_ver` is present it is used to pick the matching key from `private_keys`

**Returns:** Decrypted message as JSON string

**Throws:**
- `WECOM_ERR_PRIKEY` — no key configured at all (neither `private_keys` nor `private_key`)
- `WECOM_ERR_PRIKEY` — `publickey_ver` not found in `private_keys` (error message names the missing version)
- `WECOM_ERR_PARAM` — chat item is missing required fields

#### getMediaData

```php
public function getMediaData(string $sdkFileId, array $options = []): string
```

Download media file content. The whole file is held in memory; use `saveMediaData()` for large files.

**Parameters:**
- `$sdkFileId`: The `sdkfileid` from message
- `$options`: Optional settings (proxy, passwd, timeout, retries — same meaning as for `saveMediaData()`, but `retries` defaults to 0)

**Returns:** Binary content of the media file (`''` for an empty file)

#### saveMediaData

```php
public function saveMediaData(string $sdkFileId, string $path, array $options = []): int
```

Download a media file straight to `$path`, one chunk at a time. Memory use depends only on the
chunk size (at most 512 KB), not on the file size.

**Parameters:**
- `$sdkFileId`: The `sdkfileid` from message
- `$path`: Local file path to write. The directory must already exist (it is not created); an existing regular file is replaced and keeps its owner, group, permissions and POSIX ACL. Only local paths (optionally `file://`) are accepted, and `open_basedir` must allow the target's directory, where the temporary file is created
- `$options`: Optional settings
  - `proxy`, `passwd`: same as `getChatData()`
  - `timeout`: timeout in seconds for each chunk (default 5)
  - `retries`: how many times a chunk that fails with 10001–10003 is retried with the same arguments, as the SDK documentation recommends (default 2)
  - `md5`: expected MD5 of the file as 32 hex characters, e.g. the message's `md5sum`; computed while writing

**Returns:** Number of bytes written (0 for an empty file, which is still created)

**Throws:** an `Exception` whose code is either the SDK error code, or one of
`WECOM_ERR_PATH` (invalid target path), `WECOM_ERR_WRITE` (the file cannot be written),
`WECOM_ERR_MD5` (checksum mismatch) or `WECOM_ERR_PARAM` (invalid option). In every case `$path` is
left as it was and no temporary file remains. If `max_execution_time` runs out during a download, the
download stops before the next chunk and removes the temporary file before the fatal error, so normally
nothing is left behind; if it runs out while the last chunk is being fetched, the complete file is still
moved into place first.

#### getSdkVersion

```php
public static function getSdkVersion(): string
```

Get the SDK version.

## Error Codes

| Code | Constant | Description |
|------|----------|-------------|
| 10000 | `WECOM_ERR_PARAM` | Parameter error |
| 10001 | `WECOM_ERR_NETWORK` | Network error |
| 10002 | `WECOM_ERR_PARSE` | Data parse failed |
| 10003 | `WECOM_ERR_SYSTEM` | System error |
| 10004 | `WECOM_ERR_ENCRYPT` | Encryption failed |
| 10005 | `WECOM_ERR_FILEID` | Invalid file ID |
| 10006 | `WECOM_ERR_DECRYPT` | Decryption failed |
| 10007 | `WECOM_ERR_PRIKEY` | Private key not found |
| 10008 | `WECOM_ERR_ENCKEY` | Encrypt key parse error |
| 10009 | `WECOM_ERR_IP` | IP not allowed |
| 10010 | `WECOM_ERR_EXPIRED` | Data expired |
| 10011 | `WECOM_ERR_CERT` | Certificate error |
| 20001 | `WECOM_ERR_WRITE` | Failed to write the target file (`saveMediaData()`) |
| 20002 | `WECOM_ERR_MD5` | Downloaded file does not match the `md5` option (`saveMediaData()`) |
| 20003 | `WECOM_ERR_PATH` | Invalid target path: not local, outside `open_basedir`, directory missing, an existing directory or other non-regular file, or an existing target that cannot be inspected (`saveMediaData()`) |

Codes 100xx come from the WeCom SDK and are passed through unchanged; codes 200xx are raised by the extension itself.

## License

PHP License 3.01
