# wecomarchive

**[English](README.md)** | 简体中文

企业微信会话存档功能的 PHP 扩展。

> **使用 [PIE](https://github.com/php/pie) 安装**: `pie install bangbangda/wecomarchive`

## 特性

- **自动下载 SDK**：安装时自动下载企业微信 SDK
- **面向对象接口**：简洁、现代的 PHP API 用于会话存档操作
- **功能完整**：获取消息、解密内容、下载媒体文件
- **灵活配置**：支持自定义 SDK 路径和代理设置

## 系统要求

- PHP >= 8.0
- Linux (x86_64 或 ARM64)
- OpenSSL >= 1.1.0

## 安装

### 方式 1：PIE（推荐）

[PIE](https://github.com/php/pie) 是安装 PHP 扩展的现代方式。

**基础安装（自动）：**

```bash
# 如果还未安装 PIE
composer global require php/pie

# 安装扩展（SDK 将自动下载）
pie install bangbangda/wecomarchive
```

企业微信 SDK 将在安装过程中**自动下载**到 `/usr/local/lib/`。

**高级配置：**

如果需要自定义 SDK 路径或自动下载失败：

```bash
# 指定自定义 SDK 库路径
pie install bangbangda/wecomarchive --with-wecomarchive-sdk-path=/custom/lib/path

# 然后手动下载 SDK 到自定义路径
vendor/bin/download-sdk.sh --path /custom/lib/path
```

**注意：** 如果自动下载因权限问题失败，可能需要运行：

```bash
# 使用 sudo 手动下载 SDK
sudo ./scripts/download-sdk.sh
```

### 方式 2：手动安装

1. 下载并解压源代码：

```bash
git clone https://github.com/bangbangda/wecomarchive.git
cd wecomarchive
```

2. 编译扩展（configure 时会自动下载 SDK）：

```bash
phpize
./configure
make
sudo make install
```

**企业微信 SDK 将在 `./configure` 步骤中自动下载到 `/usr/local/lib/`。**

如果要使用自定义 SDK 路径：

```bash
phpize
./configure --with-wecomarchive-sdk-path=/custom/lib/path
make
sudo make install
```

如果自动下载失败，可以先手动下载 SDK：

```bash
chmod +x scripts/download-sdk.sh
./scripts/download-sdk.sh
# 或使用自定义路径
./scripts/download-sdk.sh --path /custom/path
```

3. 在 php.ini 中启用扩展：

```ini
extension=wecomarchive.so
wecomarchive.sdk_lib_path=/usr/local/lib/libWeWorkFinanceSdk_C.so
```

## 配置

| INI 配置项 | 默认值 | 说明 |
|-------------|---------|-------------|
| `wecomarchive.sdk_lib_path` | `/usr/local/lib/libWeWorkFinanceSdk_C.so` | 企业微信 SDK 库路径 |

## 使用方法

### 基础示例

```php
<?php
// 使用你的凭据初始化（私钥支持 PEM 字符串或文件路径，自动识别）
$archive = new WeComArchive([
    'corpid' => 'your_corp_id',
    'secret' => 'your_secret',
    'private_key' => '/path/to/private.pem', // 或直接传 PEM 内容
]);

// 获取聊天数据
$response = $archive->getChatData(seq: 0, limit: 100);
$data = json_decode($response, true);

if ($data['errcode'] === 0) {
    foreach ($data['chatdata'] as $chat) {
        // 解密消息
        $message = $archive->decryptData(
            $chat['encrypt_random_key'],
            $chat['encrypt_chat_msg']
        );
        $msgData = json_decode($message, true);
        print_r($msgData);
    }
}
```

### 多版本私钥（推荐）

企微后台支持轮换私钥，每条 chatdata 中带有 `publickey_ver` 字段。配置 `private_keys` 后，扩展会按版本号自动选择对应私钥：

```php
<?php
$archive = new WeComArchive([
    'corpid' => 'your_corp_id',
    'secret' => 'your_secret',
    // [publickey_ver => PEM 字符串或文件路径]
    'private_keys' => [
        1 => '/path/to/key_v1.pem',
        2 => '/path/to/key_v2.pem',
        3 => "-----BEGIN PRIVATE KEY-----\n...",
    ],
]);

$response = $archive->getChatData(0, 100);
$data = json_decode($response, true);

foreach ($data['chatdata'] as $chat) {
    // 直接传整条 chatdata 项即可，扩展自动按 publickey_ver 选私钥
    $message = $archive->decryptChatItem($chat);
    $msgData = json_decode($message, true);
    print_r($msgData);
}
```

如果 `publickey_ver` 在 `private_keys` 中找不到对应私钥，会抛出明确异常并指出缺失的版本号。

### 下载媒体文件

```php
<?php
// 获取媒体文件（图片、视频、文件等）
$mediaContent = $archive->getMediaData($sdkFileId, [
    'timeout' => 30,
]);

// 保存到文件
file_put_contents('/path/to/output.jpg', $mediaContent);
```

### 使用代理

```php
<?php
$archive = new WeComArchive([
    'corpid' => 'your_corp_id',
    'secret' => 'your_secret',
    'private_key' => file_get_contents('/path/to/private.pem'),
]);

// 使用代理
$response = $archive->getChatData(0, 100, [
    'proxy' => 'http://proxy.example.com:8080',
    'passwd' => 'user:password',
    'timeout' => 10,
]);
```

### 自定义 SDK 库路径

```php
<?php
$archive = new WeComArchive([
    'corpid' => 'your_corp_id',
    'secret' => 'your_secret',
    'lib_path' => '/custom/path/to/libWeWorkFinanceSdk_C.so',
]);
```

## API 参考

### WeComArchive 类

#### 构造函数

```php
public function __construct(array $options)
```

**参数：**
- `corpid`（必需）：企业微信 Corp ID
- `secret`（必需）：会话存档 secret
- `private_key`（可选）：单个 RSA 私钥，可传 PEM 内容字符串或 PEM 文件路径（自动识别）。`decryptData` 使用此私钥
- `private_keys`（可选）：多版本私钥映射 `[publickey_ver => PEM 字符串或文件路径]`。`decryptChatItem` 按版本号自动选择
- `lib_path`（可选）：SDK 库的自定义路径

> 说明：值若为多行内容（PEM 内容总是包含换行，文件路径则不含）或以 `-----BEGIN ` 开头，视为 PEM 内容；否则视为文件路径。这样能兼容带前导元数据的 PEM（例如 `openssl pkcs12` 导出时附带的 `Bag Attributes` 段）。`private_key` 与 `private_keys` 可同时设置，前者作为 `decryptChatItem` 的兜底。

#### getChatData

```php
public function getChatData(int $seq = 0, int $limit = 100, array $options = []): string
```

获取聊天消息。

**参数：**
- `$seq`：起始序列号（首次获取使用 0）
- `$limit`：最大获取消息数（1-1000）
- `$options`：可选设置（proxy, passwd, timeout）

**返回：** 包含聊天数据的 JSON 字符串

#### decryptData

```php
public function decryptData(string $encryptRandomKey, string $encryptChatMsg): string
```

使用构造时传入的 `private_key` 解密一条聊天消息。

**参数：**
- `$encryptRandomKey`：聊天数据中的 `encrypt_random_key`
- `$encryptChatMsg`：聊天数据中的 `encrypt_chat_msg`

**返回：** 解密后的消息 JSON 字符串

**抛出：** 若构造时未传 `private_key`，会抛 `WECOM_ERR_PRIKEY` 异常并提示。

#### decryptChatItem

```php
public function decryptChatItem(array $chatItem): string
```

解密单条 chatdata 数组项，自动按 `publickey_ver` 从 `private_keys` 中选择对应私钥。

**参数：**
- `$chatItem`：`getChatData` 返回的 `chatdata` 数组中的一项，至少需包含 `encrypt_random_key`、`encrypt_chat_msg` 字段；如带 `publickey_ver` 则按版本号自动选私钥

**返回：** 解密后的消息 JSON 字符串

**抛出：**
- 未配置任何私钥 → `WECOM_ERR_PRIKEY`，提示传入 `private_keys` 或 `private_key`
- `publickey_ver` 在 `private_keys` 中找不到 → `WECOM_ERR_PRIKEY`，错误信息包含具体版本号
- chat 项缺少必需字段 → `WECOM_ERR_PARAM`

#### getMediaData

```php
public function getMediaData(string $sdkFileId, array $options = []): string
```

下载媒体文件内容。

**参数：**
- `$sdkFileId`：消息中的 `sdkfileid`
- `$options`：可选设置（proxy, passwd, timeout）

**返回：** 媒体文件的二进制内容

#### getSdkVersion

```php
public static function getSdkVersion(): string
```

获取 SDK 版本。

## 错误码

| 代码 | 常量 | 说明 |
|------|----------|-------------|
| 10000 | `WECOM_ERR_PARAM` | 参数错误 |
| 10001 | `WECOM_ERR_NETWORK` | 网络错误 |
| 10002 | `WECOM_ERR_PARSE` | 数据解析失败 |
| 10003 | `WECOM_ERR_SYSTEM` | 系统错误 |
| 10004 | `WECOM_ERR_ENCRYPT` | 加密失败 |
| 10005 | `WECOM_ERR_FILEID` | 无效的文件 ID |
| 10006 | `WECOM_ERR_DECRYPT` | 解密失败 |
| 10007 | `WECOM_ERR_PRIKEY` | 未找到私钥 |
| 10008 | `WECOM_ERR_ENCKEY` | 加密密钥解析错误 |
| 10009 | `WECOM_ERR_IP` | IP 不允许 |
| 10010 | `WECOM_ERR_EXPIRED` | 数据已过期 |
| 10011 | `WECOM_ERR_CERT` | 证书错误 |

## 许可证

PHP License 3.01
