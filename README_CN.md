# wecomarchive

**[English](README.md)** | 简体中文

企业微信会话存档功能的 PHP 扩展。

> **使用 [PIE](https://github.com/php/pie) 安装**: `pie install bangbangda/wecomarchive`

## 特性

- **自动下载 SDK**：安装时自动下载企业微信 SDK
- **面向对象接口**：简洁、现代的 PHP API 用于会话存档操作
- **功能完整**：获取消息、解密内容、下载媒体文件（边下边写入磁盘，支持墙钟时长上限和断点续传）
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

用 `saveMediaData()` 把媒体（图片、语音、视频、文件）直接下载到磁盘。它每拿到一个分片（最大 512KB）
就立即写入文件，不论文件多大，内存占用都基本不变。**视频和文件动辄几百 MB，请务必使用它。**

```php
<?php
// $msg 是解密后的一条 file 类型消息（video、image、voice 同理）
$bytes = $archive->saveMediaData($msg['file']['sdkfileid'], '/data/media/' . $msg['msgid'], [
    'timeout'     => 30,                    // 每个分片的超时
    'max_seconds' => 600,                   // 可选：整个下载最多用 10 分钟（墙钟时间）
    'md5'         => $msg['file']['md5sum'], // 可选：校验文件完整性
]);
```

`max_seconds` 按单调墙钟计时，等待网络的时间也算在内（Linux 上 `max_execution_time` 按 CPU 时间计，
等网络时不走）。时间用完时会在拉取下一个分片前停止，删除临时文件并抛出 `WECOM_ERR_TIMEOUT`。

它在每次请求分片前检查，不是硬上限。SDK 只接受整秒，临近截止时发出的那一片会拿到 1 秒超时，最多可能晚约 1 秒结束。
最后一片之后的落盘（以及 `saveMediaDataPart()` 的 md5 回读）也不计入它。外层任务的超时要给它留出余量。

数据先写到目标旁边的临时文件 `<path>.part-<随机串>`，整个下载（以及 md5 校验）成功后先 `fsync`
落盘，再原子地 rename 为 `<path>`。任何环节失败都会删除临时文件、保持 `<path>` 原样，并抛出异常。
以上保证的前提是下载期间目标目录没有被移动或替换。

被替换的文件保留原有的属主、属组、权限和 POSIX ACL；如果无法保留（例如文件属于其他用户），会在写入任何数据之前以
`WECOM_ERR_WRITE` 失败，原文件保持不变。替换会像 `rename()` 一样产生一个新文件，因此其他扩展属性和硬链接不会保留，
安全标签（SELinux、AppArmor）按系统策略为该目录下的新文件设置。新建的文件按 umask 和目录的默认 ACL 设置权限，
与 `file_put_contents()` 一致。

### 断点续传

一个任务的时间预算内下不完的文件，可以用 `saveMediaDataPart()` 分多次下载，各次调用可以在不同进程里、
相隔几天（企业微信保留媒体文件 5 天）。每次调用最多用 `max_seconds` 往 `$partPath` 追加数据，然后把文件
落盘并返回停在哪里。调用方记下 `indexbuf` 和 `bytes`，下次以 `indexbuf` 和 `offset` 传回：

```php
<?php
$part = '/data/media/' . $msg['msgid'] . '.part';
$state = ['indexbuf' => '', 'offset' => 0];   // 来自你的任务存储；首次为空

$result = $archive->saveMediaDataPart($msg['file']['sdkfileid'], $part, $state + [
    'max_seconds' => 600,
    'timeout'     => 30,
    'md5'         => $msg['file']['md5sum'],   // 下载完成时校验
]);

if ($result['finished']) {
    rename($part, '/data/media/' . $msg['msgid']);  // 文件已完整且已 fsync
} else {
    // 保存 ['indexbuf' => $result['indexbuf'], 'offset' => $result['bytes']]，稍后再调用
}
```

`offset` 是调用方确认已写入文件的字节数；超出它的部分（某次调用写入了但结果没来得及记录，例如进程被杀）
会在继续下载前被截掉。`max_seconds` 用完不算错误，返回 `finished => false`。只有真正出错才抛异常。
出错时不会删除 `$partPath`，但它可能已经被清空（`offset` 为 0）或截断到 `offset`，之后还可能写入了一部分数据，
所以续传一律以上次记录的状态为准，不要以文件大小为准。方法不会 rename 文件，下载完成后放到哪里由调用方决定。

续传令牌就是 SDK 自己的 `outindexbuf`。SDK v3_20250205 的令牌是文本 `Range:bytes=<start>-<end>`，
即下一片的 HTTP 字节范围，不含任何会话状态，所以跨进程、跨时间都有效。扩展会检查 `<start>` 是否等于
`offset`，不一致时以 `WECOM_ERR_RESUME` 拒绝；其他形式的令牌原样传给 SDK。

`getMediaData()` 以字符串形式返回整个文件，文件内容会全部留在内存里，适合图片、语音这类小媒体：

```php
<?php
$mediaContent = $archive->getMediaData($sdkFileId, [
    'timeout' => 30,
]);
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
- `$options`：可选设置（proxy, passwd, timeout——正数秒，可以是 int、float 或数字字符串，其他值抛 `WECOM_ERR_PARAM`）

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

下载媒体文件内容。整个文件会保存在内存中，大文件请使用 `saveMediaData()`。

**参数：**
- `$sdkFileId`：消息中的 `sdkfileid`
- `$options`：可选设置（proxy, passwd, timeout, retries, max_seconds，含义同 `saveMediaData()`，但 `retries` 默认为 0）

**返回：** 媒体文件的二进制内容（空文件返回 `''`）

#### saveMediaData

```php
public function saveMediaData(string $sdkFileId, string $path, array $options = []): int
```

把媒体文件逐个分片直接写入 `$path`。内存占用只取决于分片大小（最大 512KB），与文件大小无关。

**参数：**
- `$sdkFileId`：消息中的 `sdkfileid`
- `$path`：要写入的本地文件路径。所在目录必须已存在（不会自动创建）；目标已是普通文件时会被覆盖，并保留原有的属主、属组、权限和 POSIX ACL。只接受本地路径（可带 `file://`），且 `open_basedir` 必须允许目标所在目录（临时文件建在该目录下）
- `$options`：可选设置
  - `proxy`、`passwd`：同 `getChatData()`
  - `timeout`：每个分片的超时秒数（默认 5）。正数的 int、float 或数字字符串，小数向上取整；其他值抛 `WECOM_ERR_PARAM`
  - `retries`：某个分片返回 10001～10003 时，按官方建议用相同参数重试的次数（默认 2）。每次重试前等待 200ms × 重试次数，且不超过 `max_seconds` 的剩余时间
  - `max_seconds`：整次调用的墙钟时间预算，大于 0 的 int 或 float（默认不限）。每次请求分片（含重试）前检查；单片 `timeout` 会被压到剩余时间以内（至少 1 秒）。用完时抛 `WECOM_ERR_TIMEOUT`。不是硬上限：最后一片最多可能晚约 1 秒结束，最后的 `fsync` 也不计入
  - `md5`：文件的预期 MD5，32 位十六进制，例如消息里的 `md5sum`；在写入过程中计算

**返回：** 写入的字节数（空文件返回 0，文件仍会被创建）

**异常：** 抛出 `Exception`，code 为 SDK 原始错误码，或者以下扩展错误码之一：
`WECOM_ERR_PATH`（目标路径不合法）、`WECOM_ERR_WRITE`（写文件失败）、`WECOM_ERR_MD5`（md5 不一致）、
`WECOM_ERR_TIMEOUT`（`max_seconds` 用完）、`WECOM_ERR_INDEXBUF`（SDK 响应无法继续）、
`WECOM_ERR_EXEC_TIME`（`max_execution_time` 到期）、`WECOM_ERR_PARAM`（选项不合法）。无论哪种情况，`$path` 都保持原样，
也不会残留临时文件。rename 成功后还会对所在目录做一次 fsync，这一步尽力而为，目录同步失败不会报告。
如果下载过程中 `max_execution_time` 到期，会在拉取下一个分片前停止并删除临时文件，然后才出现超时致命错误，
因此通常不会留下残留；如果恰好在拉取最后一个分片时到期，已完整下载的文件仍会先 rename 到位。

#### saveMediaDataPart

```php
public function saveMediaDataPart(string $sdkFileId, string $partPath, array $options = []): array
```

分段、可续传地下载媒体文件：最多用 `max_seconds` 往 `$partPath` 追加数据，然后把文件落盘并报告停在哪里。
见[断点续传](#断点续传)。

**参数：**
- `$sdkFileId`：消息中的 `sdkfileid`
- `$partPath`：接收数据的本地文件，路径规则同 `saveMediaData()`。`offset` 为 0 时创建或清空；否则必须已存在且不小于 `offset` 字节，继续下载前会先截断到 `offset`。符号链接会被跟随。确认打开的正是检查过的那个文件之后才会创建或截断；如果路径在两步之间被替换，抛 `WECOM_ERR_PATH`
- `$options`：可选设置
  - `indexbuf`：上次调用返回的 `indexbuf`（默认 `''`，从文件开头下载）。`offset` 大于 0 时必填
  - `offset`：此前各次调用已写入 `$partPath` 的字节数（默认 0）
  - `max_seconds`、`timeout`、`retries`、`proxy`、`passwd`：同 `saveMediaData()`
  - `md5`：预期 MD5；下载完成时流式回读刚写入的那个文件来校验（而不是路径此时指向的文件）

**返回：** `['finished' => bool, 'indexbuf' => string, 'bytes' => int]`。`finished` 为 true 表示最后一片已写入；
`indexbuf` 是下次调用的令牌（完成时为 `''`）；`bytes` 是 `$partPath` 当前的大小。`max_seconds` 用完返回
`finished => false`，不算错误。每次返回前都会 `fsync` 文件，并清除 PHP 的 stat 缓存，`filesize()` 能立即看到新大小。
本次调用新建了文件时，还会对所在目录 fsync（尽力而为，失败不报告）：万一崩溃后目录项仍然丢失，下次以 `offset` 大于 0
续传会得到 `WECOM_ERR_RESUME`，需要从头下载。

**异常：** 抛出 `Exception`，code 为 SDK 原始错误码，或 `WECOM_ERR_PATH`、`WECOM_ERR_WRITE`、
`WECOM_ERR_MD5`（完成的文件与 `md5` 不一致）、`WECOM_ERR_RESUME`（`$partPath` 不存在或小于 `offset`，或 `indexbuf`
对应另一个偏移）、`WECOM_ERR_INDEXBUF`、`WECOM_ERR_EXEC_TIME`、`WECOM_ERR_PARAM`。抛异常时不会删除 `$partPath`，
但它可能已被清空或截断到 `offset`，也可能写入了出错前的部分数据，请从上次记录的状态继续。

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
| 20001 | `WECOM_ERR_WRITE` | 写入目标文件失败（`saveMediaData()`） |
| 20002 | `WECOM_ERR_MD5` | 下载内容与 `md5` 选项不一致（`saveMediaData()`） |
| 20003 | `WECOM_ERR_PATH` | 目标路径不合法：非本地路径、超出 `open_basedir`、目录不存在、目标已是目录等非普通文件，或已存在的目标无法读取元数据（`saveMediaData()`、`saveMediaDataPart()`）；对 `saveMediaDataPart()` 还包括 `$partPath` 在打开过程中被替换、新建或删除 |
| 20004 | `WECOM_ERR_TIMEOUT` | `max_seconds` 用完时下载还没结束（`getMediaData()`、`saveMediaData()`） |
| 20005 | `WECOM_ERR_EXEC_TIME` | 下载期间 `max_execution_time` 到期，随后出现引擎的致命错误（`getMediaData()`、`saveMediaData()`、`saveMediaDataPart()`） |
| 20006 | `WECOM_ERR_INDEXBUF` | SDK 返回了未完成的分片却没有新的 `outindexbuf`，无法继续（`getMediaData()`、`saveMediaData()`、`saveMediaDataPart()`） |
| 20007 | `WECOM_ERR_NOT_INIT` | 对象没有 SDK 实例，例如绕过构造函数创建（所有调用 SDK 的方法） |
| 20008 | `WECOM_ERR_RESUME` | 无法续传：`$partPath` 不存在或小于 `offset`，或 `indexbuf` 对应另一个偏移（`saveMediaDataPart()`） |

100xx 来自企业微信 SDK，原样透传；200xx 由扩展自身抛出。

## 许可证

PHP License 3.01
