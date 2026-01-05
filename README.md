# wecomarchive

PHP extension for WeCom (WeChat Work) Chat Archive functionality.

## Requirements

- PHP >= 8.0
- Linux (x86_64 or ARM64)
- OpenSSL >= 1.1.0

## Installation

### Method 1: PECL (Recommended)

```bash
pecl install wecomarchive
```

After installation, run the SDK download script:

```bash
wecomarchive-download-sdk
```

### Method 2: Manual Installation

1. Download and extract the source:

```bash
git clone https://github.com/yourname/wecomarchive.git
cd wecomarchive
```

2. Download the WeCom SDK:

```bash
chmod +x scripts/download-sdk.sh
./scripts/download-sdk.sh
```

3. Build the extension:

```bash
phpize
./configure
make
sudo make install
```

4. Enable the extension in php.ini:

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
// Initialize with your credentials
$archive = new WeComArchive([
    'corpid' => 'your_corp_id',
    'secret' => 'your_secret',
    'private_key' => file_get_contents('/path/to/private.pem'),
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

### Download Media Files

```php
<?php
// Get media file (image, video, file, etc.)
$mediaContent = $archive->getMediaData($sdkFileId, [
    'timeout' => 30,
]);

// Save to file
file_put_contents('/path/to/output.jpg', $mediaContent);
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
- `private_key` (optional): RSA private key for decryption
- `lib_path` (optional): Custom path to SDK library

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

Decrypt a chat message.

**Parameters:**
- `$encryptRandomKey`: The `encrypt_random_key` from chat data
- `$encryptChatMsg`: The `encrypt_chat_msg` from chat data

**Returns:** Decrypted message as JSON string

#### getMediaData

```php
public function getMediaData(string $sdkFileId, array $options = []): string
```

Download media file content.

**Parameters:**
- `$sdkFileId`: The `sdkfileid` from message
- `$options`: Optional settings (proxy, passwd, timeout)

**Returns:** Binary content of the media file

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

## License

PHP License 3.01
