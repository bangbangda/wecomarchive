/*
  +----------------------------------------------------------------------+
  | wecomarchive - WeCom (WeChat Work) Chat Archive PHP Extension        |
  +----------------------------------------------------------------------+
  | Copyright (c) 2025                                                   |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,     |
  | that is bundled with this package in the file LICENSE, and is       |
  | available through the world-wide-web at the following url:          |
  | http://www.php.net/license/3_01.txt                                 |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "zend_smart_str.h"
#include "php_streams.h"
#include "php_wecomarchive.h"

#include <dlfcn.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>

/* Error codes (mirrors REGISTER_LONG_CONSTANT in MINIT) */
#define WECOM_ERR_PARAM   10000
#define WECOM_ERR_DECRYPT 10006
#define WECOM_ERR_PRIKEY  10007

ZEND_DECLARE_MODULE_GLOBALS(wecomarchive)

/* SDK structures (from WeWorkFinanceSdk_C.h) */
typedef struct WeWorkFinanceSdk_t WeWorkFinanceSdk_t;

typedef struct Slice_t {
    char *buf;
    int len;
} Slice_t;

typedef struct MediaData_t {
    char *outindexbuf;
    int out_len;
    char *data;
    int data_len;
    int is_finish;
} MediaData_t;

/* SDK function typedefs */
typedef WeWorkFinanceSdk_t* (*NewSdk_t)(void);
typedef int (*Init_t)(WeWorkFinanceSdk_t*, const char*, const char*);
typedef void (*DestroySdk_t)(WeWorkFinanceSdk_t*);
typedef int (*GetChatData_t)(WeWorkFinanceSdk_t*, unsigned long long, unsigned int, const char*, const char*, int, Slice_t*);
typedef int (*DecryptData_t)(const char*, const char*, Slice_t*);
typedef int (*GetMediaData_t)(WeWorkFinanceSdk_t*, const char*, const char*, const char*, const char*, int, MediaData_t*);
typedef Slice_t* (*NewSlice_t)(void);
typedef void (*FreeSlice_t)(Slice_t*);
typedef MediaData_t* (*NewMediaData_t)(void);
typedef void (*FreeMediaData_t)(MediaData_t*);

/* SDK function pointers */
static void *sdk_handle = NULL;
static NewSdk_t fn_NewSdk = NULL;
static Init_t fn_Init = NULL;
static DestroySdk_t fn_DestroySdk = NULL;
static GetChatData_t fn_GetChatData = NULL;
static DecryptData_t fn_DecryptData = NULL;
static GetMediaData_t fn_GetMediaData = NULL;
static NewSlice_t fn_NewSlice = NULL;
static FreeSlice_t fn_FreeSlice = NULL;
static NewMediaData_t fn_NewMediaData = NULL;
static FreeMediaData_t fn_FreeMediaData = NULL;

/* WeComArchive class */
static zend_class_entry *wecomarchive_ce;
static zend_object_handlers wecomarchive_object_handlers;

typedef struct {
    WeWorkFinanceSdk_t *sdk;
    /* Optional single key (BC: from constructor option "private_key"). Used by decryptData
       and as fallback in decryptChatItem when no publickey_ver is present. */
    zend_string *default_key;
    /* Optional version-keyed map (from constructor option "private_keys").
       Stores zend_string PEM contents indexed by integer publickey_ver. */
    HashTable *private_keys;
    zend_object std;
} wecomarchive_object;

static inline wecomarchive_object *wecomarchive_from_obj(zend_object *obj) {
    return (wecomarchive_object *)((char *)(obj) - XtOffsetOf(wecomarchive_object, std));
}

#define Z_WECOMARCHIVE_P(zv) wecomarchive_from_obj(Z_OBJ_P(zv))

/* INI settings */
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("wecomarchive.sdk_lib_path", "/usr/local/lib/libWeWorkFinanceSdk_C.so", PHP_INI_SYSTEM, OnUpdateString, sdk_lib_path, zend_wecomarchive_globals, wecomarchive_globals)
PHP_INI_END()

/* Load SDK library */
static int load_sdk_library(const char *lib_path) {
    if (sdk_handle != NULL) {
        return SUCCESS;
    }

    sdk_handle = dlopen(lib_path, RTLD_LAZY);
    if (!sdk_handle) {
        php_error_docref(NULL, E_WARNING, "Failed to load WeCom SDK library: %s", dlerror());
        return FAILURE;
    }

    fn_NewSdk = (NewSdk_t)dlsym(sdk_handle, "NewSdk");
    fn_Init = (Init_t)dlsym(sdk_handle, "Init");
    fn_DestroySdk = (DestroySdk_t)dlsym(sdk_handle, "DestroySdk");
    fn_GetChatData = (GetChatData_t)dlsym(sdk_handle, "GetChatData");
    fn_DecryptData = (DecryptData_t)dlsym(sdk_handle, "DecryptData");
    fn_GetMediaData = (GetMediaData_t)dlsym(sdk_handle, "GetMediaData");
    fn_NewSlice = (NewSlice_t)dlsym(sdk_handle, "NewSlice");
    fn_FreeSlice = (FreeSlice_t)dlsym(sdk_handle, "FreeSlice");
    fn_NewMediaData = (NewMediaData_t)dlsym(sdk_handle, "NewMediaData");
    fn_FreeMediaData = (FreeMediaData_t)dlsym(sdk_handle, "FreeMediaData");

    if (!fn_NewSdk || !fn_Init || !fn_DestroySdk || !fn_GetChatData ||
        !fn_DecryptData || !fn_GetMediaData || !fn_NewSlice || !fn_FreeSlice ||
        !fn_NewMediaData || !fn_FreeMediaData) {
        php_error_docref(NULL, E_WARNING, "Failed to load SDK functions: %s", dlerror());
        dlclose(sdk_handle);
        sdk_handle = NULL;
        return FAILURE;
    }

    return SUCCESS;
}

/* RSA decrypt encrypt_random_key */
static char *rsa_decrypt(const char *private_key, size_t private_key_len, const char *encrypted_data, size_t *decrypted_len) {
    BIO *bio = BIO_new_mem_buf(private_key, (int)private_key_len);
    if (!bio) {
        return NULL;
    }

    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pkey) {
        return NULL;
    }

    /* Base64 decode the encrypted data */
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *bmem = BIO_new_mem_buf(encrypted_data, -1);
    bmem = BIO_push(b64, bmem);
    BIO_set_flags(bmem, BIO_FLAGS_BASE64_NO_NL);

    size_t encrypted_len = strlen(encrypted_data);
    unsigned char *decoded = (unsigned char *)emalloc(encrypted_len);
    int decoded_len = BIO_read(bmem, decoded, encrypted_len);
    BIO_free_all(bmem);

    if (decoded_len <= 0) {
        efree(decoded);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    /* Decrypt using RSA */
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
    if (!ctx) {
        efree(decoded);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    if (EVP_PKEY_decrypt_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        efree(decoded);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    if (EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        efree(decoded);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    size_t outlen;
    if (EVP_PKEY_decrypt(ctx, NULL, &outlen, decoded, decoded_len) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        efree(decoded);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    unsigned char *decrypted = (unsigned char *)emalloc(outlen + 1);
    if (EVP_PKEY_decrypt(ctx, decrypted, &outlen, decoded, decoded_len) <= 0) {
        efree(decrypted);
        EVP_PKEY_CTX_free(ctx);
        efree(decoded);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    decrypted[outlen] = '\0';
    *decrypted_len = outlen;

    EVP_PKEY_CTX_free(ctx);
    efree(decoded);
    EVP_PKEY_free(pkey);

    return (char *)decrypted;
}

/* Resolve a private key source: if value starts with "-----BEGIN" treat as PEM content,
   otherwise treat as a file path and read its contents. Returns a new zend_string (caller
   owns the reference) or NULL on failure with err_buf populated. */
static zend_string *resolve_private_key_source(const char *src, size_t src_len, char *err_buf, size_t err_buf_size) {
    if (src_len >= 11 && memcmp(src, "-----BEGIN ", 11) == 0) {
        return zend_string_init(src, src_len, 0);
    }

    /* Treat as file path. Use php_stream so it respects open_basedir and supports user wrappers. */
    php_stream *stream = php_stream_open_wrapper((char *)src, "rb", REPORT_ERRORS, NULL);
    if (!stream) {
        snprintf(err_buf, err_buf_size, "failed to open private key file '%s'", src);
        return NULL;
    }

    zend_string *contents = php_stream_copy_to_mem(stream, PHP_STREAM_COPY_ALL, 0);
    php_stream_close(stream);

    if (!contents || ZSTR_LEN(contents) == 0) {
        if (contents) zend_string_release(contents);
        snprintf(err_buf, err_buf_size, "private key file '%s' is empty or unreadable", src);
        return NULL;
    }

    if (ZSTR_LEN(contents) < 11 || strstr(ZSTR_VAL(contents), "-----BEGIN ") == NULL) {
        zend_string_release(contents);
        snprintf(err_buf, err_buf_size, "file '%s' does not contain a PEM private key", src);
        return NULL;
    }

    return contents;
}

/* Object handlers */
static zend_object *wecomarchive_object_create(zend_class_entry *ce) {
    wecomarchive_object *intern = zend_object_alloc(sizeof(wecomarchive_object), ce);

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);

    intern->std.handlers = &wecomarchive_object_handlers;
    intern->sdk = NULL;
    intern->default_key = NULL;
    intern->private_keys = NULL;

    return &intern->std;
}

static void wecomarchive_object_free(zend_object *object) {
    wecomarchive_object *intern = wecomarchive_from_obj(object);

    if (intern->sdk && fn_DestroySdk) {
        fn_DestroySdk(intern->sdk);
        intern->sdk = NULL;
    }

    if (intern->default_key) {
        zend_string_release(intern->default_key);
        intern->default_key = NULL;
    }

    if (intern->private_keys) {
        zend_hash_destroy(intern->private_keys);
        efree(intern->private_keys);
        intern->private_keys = NULL;
    }

    zend_object_std_dtor(&intern->std);
}

/* {{{ proto void WeComArchive::__construct(array $options)
   Create a new WeComArchive instance */
PHP_METHOD(WeComArchive, __construct)
{
    zval *options;
    zend_string *corpid = NULL, *secret = NULL, *lib_path = NULL;
    zval *private_key_zv = NULL, *private_keys_zv = NULL;
    HashTable *options_ht;
    zval *tmp;

    /* Pre-loaded keys (released or transferred to intern at end) */
    zend_string *loaded_default_key = NULL;
    HashTable *loaded_private_keys = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();

    options_ht = Z_ARRVAL_P(options);

    /* Get corpid */
    tmp = zend_hash_str_find(options_ht, "corpid", sizeof("corpid") - 1);
    if (!tmp || Z_TYPE_P(tmp) != IS_STRING) {
        zend_throw_exception(zend_ce_exception, "Option 'corpid' is required and must be a string", 0);
        RETURN_THROWS();
    }
    corpid = Z_STR_P(tmp);

    /* Get secret */
    tmp = zend_hash_str_find(options_ht, "secret", sizeof("secret") - 1);
    if (!tmp || Z_TYPE_P(tmp) != IS_STRING) {
        zend_throw_exception(zend_ce_exception, "Option 'secret' is required and must be a string", 0);
        RETURN_THROWS();
    }
    secret = Z_STR_P(tmp);

    /* Optional: private_key (single, BC; accepts PEM content or file path) */
    private_key_zv = zend_hash_str_find(options_ht, "private_key", sizeof("private_key") - 1);
    if (private_key_zv && Z_TYPE_P(private_key_zv) != IS_STRING) {
        zend_throw_exception(zend_ce_exception, "Option 'private_key' must be a string (PEM content or file path)", 0);
        RETURN_THROWS();
    }

    /* Optional: private_keys (multi-version map [ver => PEM-or-path]) */
    private_keys_zv = zend_hash_str_find(options_ht, "private_keys", sizeof("private_keys") - 1);
    if (private_keys_zv && Z_TYPE_P(private_keys_zv) != IS_ARRAY) {
        zend_throw_exception(zend_ce_exception, "Option 'private_keys' must be an array of [version => PEM-or-path]", 0);
        RETURN_THROWS();
    }

    /* Optional lib_path */
    tmp = zend_hash_str_find(options_ht, "lib_path", sizeof("lib_path") - 1);
    if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
        lib_path = Z_STR_P(tmp);
    }

    /* Pre-load private_key (BC) */
    if (private_key_zv) {
        char err_buf[512];
        loaded_default_key = resolve_private_key_source(Z_STRVAL_P(private_key_zv), Z_STRLEN_P(private_key_zv), err_buf, sizeof(err_buf));
        if (!loaded_default_key) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PRIKEY, "Option 'private_key' invalid: %s", err_buf);
            RETURN_THROWS();
        }
    }

    /* Pre-load private_keys map */
    if (private_keys_zv) {
        HashTable *src_ht = Z_ARRVAL_P(private_keys_zv);
        if (zend_hash_num_elements(src_ht) == 0) {
            if (loaded_default_key) zend_string_release(loaded_default_key);
            zend_throw_exception(zend_ce_exception, "Option 'private_keys' must not be empty", 0);
            RETURN_THROWS();
        }

        loaded_private_keys = (HashTable *)emalloc(sizeof(HashTable));
        zend_hash_init(loaded_private_keys, zend_hash_num_elements(src_ht), NULL, ZVAL_PTR_DTOR, 0);

        zend_ulong num_idx;
        zend_string *str_idx;
        zval *entry;

        ZEND_HASH_FOREACH_KEY_VAL(src_ht, num_idx, str_idx, entry) {
            if (str_idx) {
                /* String key: must be numeric */
                if (!is_numeric_string(ZSTR_VAL(str_idx), ZSTR_LEN(str_idx), NULL, NULL, 0)) {
                    zend_hash_destroy(loaded_private_keys);
                    efree(loaded_private_keys);
                    if (loaded_default_key) zend_string_release(loaded_default_key);
                    zend_throw_exception_ex(zend_ce_exception, 0, "Option 'private_keys' key '%s' must be a numeric publickey_ver", ZSTR_VAL(str_idx));
                    RETURN_THROWS();
                }
                num_idx = (zend_ulong)ZEND_STRTOL(ZSTR_VAL(str_idx), NULL, 10);
            }

            if (Z_TYPE_P(entry) != IS_STRING) {
                zend_hash_destroy(loaded_private_keys);
                efree(loaded_private_keys);
                if (loaded_default_key) zend_string_release(loaded_default_key);
                zend_throw_exception_ex(zend_ce_exception, 0, "Option 'private_keys[%lld]' must be a string (PEM content or file path)", (long long)num_idx);
                RETURN_THROWS();
            }

            char err_buf[512];
            zend_string *pem = resolve_private_key_source(Z_STRVAL_P(entry), Z_STRLEN_P(entry), err_buf, sizeof(err_buf));
            if (!pem) {
                zend_hash_destroy(loaded_private_keys);
                efree(loaded_private_keys);
                if (loaded_default_key) zend_string_release(loaded_default_key);
                zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PRIKEY, "Option 'private_keys[%lld]' invalid: %s", (long long)num_idx, err_buf);
                RETURN_THROWS();
            }

            zval pem_zv;
            ZVAL_STR(&pem_zv, pem);
            zend_hash_index_update(loaded_private_keys, num_idx, &pem_zv);
        } ZEND_HASH_FOREACH_END();
    }

    /* Load SDK library */
    const char *sdk_path = lib_path ? ZSTR_VAL(lib_path) : WECOMARCHIVE_G(sdk_lib_path);
    if (load_sdk_library(sdk_path) == FAILURE) {
        if (loaded_default_key) zend_string_release(loaded_default_key);
        if (loaded_private_keys) { zend_hash_destroy(loaded_private_keys); efree(loaded_private_keys); }
        zend_throw_exception_ex(zend_ce_exception, 0, "Failed to load SDK library from: %s", sdk_path);
        RETURN_THROWS();
    }

    wecomarchive_object *intern = Z_WECOMARCHIVE_P(ZEND_THIS);

    /* Create SDK instance */
    intern->sdk = fn_NewSdk();
    if (!intern->sdk) {
        if (loaded_default_key) zend_string_release(loaded_default_key);
        if (loaded_private_keys) { zend_hash_destroy(loaded_private_keys); efree(loaded_private_keys); }
        zend_throw_exception(zend_ce_exception, "Failed to create SDK instance", 0);
        RETURN_THROWS();
    }

    /* Initialize SDK */
    int ret = fn_Init(intern->sdk, ZSTR_VAL(corpid), ZSTR_VAL(secret));
    if (ret != 0) {
        fn_DestroySdk(intern->sdk);
        intern->sdk = NULL;
        if (loaded_default_key) zend_string_release(loaded_default_key);
        if (loaded_private_keys) { zend_hash_destroy(loaded_private_keys); efree(loaded_private_keys); }
        zend_throw_exception_ex(zend_ce_exception, ret, "Failed to initialize SDK, error code: %d", ret);
        RETURN_THROWS();
    }

    /* Transfer ownership of pre-loaded keys onto the object */
    intern->default_key = loaded_default_key;
    intern->private_keys = loaded_private_keys;
}
/* }}} */

/* {{{ proto array WeComArchive::getChatData(int $seq = 0, int $limit = 100, array $options = [])
   Fetch chat data from WeCom */
PHP_METHOD(WeComArchive, getChatData)
{
    zend_long seq = 0, limit = 100, timeout = 5;
    zval *options = NULL;
    char *proxy = "", *passwd = "";

    ZEND_PARSE_PARAMETERS_START(0, 3)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(seq)
        Z_PARAM_LONG(limit)
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();

    if (options) {
        HashTable *options_ht = Z_ARRVAL_P(options);
        zval *tmp;

        tmp = zend_hash_str_find(options_ht, "proxy", sizeof("proxy") - 1);
        if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
            proxy = Z_STRVAL_P(tmp);
        }

        tmp = zend_hash_str_find(options_ht, "passwd", sizeof("passwd") - 1);
        if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
            passwd = Z_STRVAL_P(tmp);
        }

        tmp = zend_hash_str_find(options_ht, "timeout", sizeof("timeout") - 1);
        if (tmp && Z_TYPE_P(tmp) == IS_LONG) {
            timeout = Z_LVAL_P(tmp);
        }
    }

    wecomarchive_object *intern = Z_WECOMARCHIVE_P(ZEND_THIS);

    if (!intern->sdk) {
        zend_throw_exception(zend_ce_exception, "SDK not initialized", 0);
        RETURN_THROWS();
    }

    Slice_t *chatData = fn_NewSlice();
    if (!chatData) {
        zend_throw_exception(zend_ce_exception, "Failed to allocate chat data buffer", 0);
        RETURN_THROWS();
    }

    int ret = fn_GetChatData(intern->sdk, (unsigned long long)seq, (unsigned int)limit, proxy, passwd, (int)timeout, chatData);
    if (ret != 0) {
        fn_FreeSlice(chatData);
        zend_throw_exception_ex(zend_ce_exception, ret, "Failed to get chat data, error code: %d", ret);
        RETURN_THROWS();
    }

    /* Return JSON string - user can json_decode in PHP */
    RETVAL_STRINGL(chatData->buf, chatData->len);
    fn_FreeSlice(chatData);
}
/* }}} */

/* {{{ proto string WeComArchive::decryptData(string $encryptRandomKey, string $encryptChatMsg)
   Decrypt chat message */
PHP_METHOD(WeComArchive, decryptData)
{
    zend_string *encrypt_random_key, *encrypt_chat_msg;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(encrypt_random_key)
        Z_PARAM_STR(encrypt_chat_msg)
    ZEND_PARSE_PARAMETERS_END();

    wecomarchive_object *intern = Z_WECOMARCHIVE_P(ZEND_THIS);

    if (!intern->default_key) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PRIKEY,
            "Private key not set. Pass 'private_key' to the constructor, "
            "or use decryptChatItem() with 'private_keys' for multi-version keys.");
        RETURN_THROWS();
    }

    /* Decrypt the encrypt_random_key using RSA private key */
    size_t decrypted_key_len;
    char *decrypted_key = rsa_decrypt(ZSTR_VAL(intern->default_key), ZSTR_LEN(intern->default_key), ZSTR_VAL(encrypt_random_key), &decrypted_key_len);
    if (!decrypted_key) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_DECRYPT,
            "RSA decryption of encrypt_random_key failed (wrong private key, or malformed encrypt_random_key)");
        RETURN_THROWS();
    }

    /* Decrypt the message using SDK */
    Slice_t *msg = fn_NewSlice();
    if (!msg) {
        efree(decrypted_key);
        zend_throw_exception(zend_ce_exception, "Failed to allocate message buffer", 0);
        RETURN_THROWS();
    }

    int ret = fn_DecryptData(decrypted_key, ZSTR_VAL(encrypt_chat_msg), msg);
    efree(decrypted_key);

    if (ret != 0) {
        fn_FreeSlice(msg);
        zend_throw_exception_ex(zend_ce_exception, ret, "Failed to decrypt message, error code: %d", ret);
        RETURN_THROWS();
    }

    RETVAL_STRINGL(msg->buf, msg->len);
    fn_FreeSlice(msg);
}
/* }}} */

/* {{{ proto string WeComArchive::decryptChatItem(array $chatItem)
   Decrypt one chatdata item, auto-selecting the private key by its publickey_ver. */
PHP_METHOD(WeComArchive, decryptChatItem)
{
    zval *chat_item;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ARRAY(chat_item)
    ZEND_PARSE_PARAMETERS_END();

    HashTable *item_ht = Z_ARRVAL_P(chat_item);
    zval *enc_rand_key_zv = zend_hash_str_find(item_ht, "encrypt_random_key", sizeof("encrypt_random_key") - 1);
    zval *enc_msg_zv     = zend_hash_str_find(item_ht, "encrypt_chat_msg",   sizeof("encrypt_chat_msg") - 1);
    zval *pub_ver_zv     = zend_hash_str_find(item_ht, "publickey_ver",      sizeof("publickey_ver") - 1);

    if (!enc_rand_key_zv || Z_TYPE_P(enc_rand_key_zv) != IS_STRING) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PARAM,
            "Chat item missing string field 'encrypt_random_key'");
        RETURN_THROWS();
    }
    if (!enc_msg_zv || Z_TYPE_P(enc_msg_zv) != IS_STRING) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PARAM,
            "Chat item missing string field 'encrypt_chat_msg'");
        RETURN_THROWS();
    }

    wecomarchive_object *intern = Z_WECOMARCHIVE_P(ZEND_THIS);

    /* Must have at least one key configured */
    if (!intern->private_keys && !intern->default_key) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PRIKEY,
            "No private key configured. Pass 'private_keys' (recommended) or 'private_key' to the WeComArchive constructor.");
        RETURN_THROWS();
    }

    /* Resolve which key to use. Priority: private_keys[publickey_ver] > default_key */
    zend_string *pem_to_use = NULL;
    zend_long ver = 0;
    int has_ver = 0;

    if (pub_ver_zv) {
        if (Z_TYPE_P(pub_ver_zv) == IS_LONG) {
            ver = Z_LVAL_P(pub_ver_zv);
            has_ver = 1;
        } else if (Z_TYPE_P(pub_ver_zv) == IS_STRING && is_numeric_string(Z_STRVAL_P(pub_ver_zv), Z_STRLEN_P(pub_ver_zv), NULL, NULL, 0)) {
            ver = (zend_long)ZEND_STRTOL(Z_STRVAL_P(pub_ver_zv), NULL, 10);
            has_ver = 1;
        }
    }

    if (has_ver && intern->private_keys) {
        zval *found = zend_hash_index_find(intern->private_keys, (zend_ulong)ver);
        if (found && Z_TYPE_P(found) == IS_STRING) {
            pem_to_use = Z_STR_P(found);
        } else {
            /* Version was provided but not configured. Fall back only if private_keys is the
               sole configuration source — otherwise honor the explicit version request. */
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PRIKEY,
                "Private key for publickey_ver=%lld not configured. "
                "Add it to the 'private_keys' constructor option.", (long long)ver);
            RETURN_THROWS();
        }
    } else if (intern->default_key) {
        /* No version in chat item, or only a single 'private_key' configured: use default */
        pem_to_use = intern->default_key;
    } else if (intern->private_keys && zend_hash_num_elements(intern->private_keys) == 1) {
        /* Single-entry private_keys with no usable version info: use the only entry */
        zval *only;
        ZEND_HASH_FOREACH_VAL(intern->private_keys, only) {
            pem_to_use = Z_STR_P(only);
            break;
        } ZEND_HASH_FOREACH_END();
    } else {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PARAM,
            "Chat item is missing a usable 'publickey_ver' and no default 'private_key' is configured.");
        RETURN_THROWS();
    }

    /* RSA-decrypt the random key */
    size_t decrypted_key_len;
    char *decrypted_key = rsa_decrypt(ZSTR_VAL(pem_to_use), ZSTR_LEN(pem_to_use), Z_STRVAL_P(enc_rand_key_zv), &decrypted_key_len);
    if (!decrypted_key) {
        if (has_ver) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_DECRYPT,
                "RSA decryption failed for publickey_ver=%lld (wrong private key for this version, or malformed encrypt_random_key)", (long long)ver);
        } else {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_DECRYPT,
                "RSA decryption of encrypt_random_key failed (wrong private key, or malformed encrypt_random_key)");
        }
        RETURN_THROWS();
    }

    /* SDK-decrypt the message */
    Slice_t *msg = fn_NewSlice();
    if (!msg) {
        efree(decrypted_key);
        zend_throw_exception(zend_ce_exception, "Failed to allocate message buffer", 0);
        RETURN_THROWS();
    }

    int ret = fn_DecryptData(decrypted_key, Z_STRVAL_P(enc_msg_zv), msg);
    efree(decrypted_key);

    if (ret != 0) {
        fn_FreeSlice(msg);
        zend_throw_exception_ex(zend_ce_exception, ret, "Failed to decrypt message, error code: %d", ret);
        RETURN_THROWS();
    }

    RETVAL_STRINGL(msg->buf, msg->len);
    fn_FreeSlice(msg);
}
/* }}} */

/* {{{ proto string WeComArchive::getMediaData(string $sdkFileId, array $options = [])
   Download media file */
PHP_METHOD(WeComArchive, getMediaData)
{
    zend_string *sdk_file_id;
    zval *options = NULL;
    zend_long timeout = 5;
    char *proxy = "", *passwd = "";

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(sdk_file_id)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();

    if (options) {
        HashTable *options_ht = Z_ARRVAL_P(options);
        zval *tmp;

        tmp = zend_hash_str_find(options_ht, "proxy", sizeof("proxy") - 1);
        if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
            proxy = Z_STRVAL_P(tmp);
        }

        tmp = zend_hash_str_find(options_ht, "passwd", sizeof("passwd") - 1);
        if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
            passwd = Z_STRVAL_P(tmp);
        }

        tmp = zend_hash_str_find(options_ht, "timeout", sizeof("timeout") - 1);
        if (tmp && Z_TYPE_P(tmp) == IS_LONG) {
            timeout = Z_LVAL_P(tmp);
        }
    }

    wecomarchive_object *intern = Z_WECOMARCHIVE_P(ZEND_THIS);

    if (!intern->sdk) {
        zend_throw_exception(zend_ce_exception, "SDK not initialized", 0);
        RETURN_THROWS();
    }

    /* Collect all media data chunks */
    smart_str buffer = {0};
    char *indexbuf = "";
    int is_finish = 0;

    while (!is_finish) {
        MediaData_t *mediaData = fn_NewMediaData();
        if (!mediaData) {
            smart_str_free(&buffer);
            zend_throw_exception(zend_ce_exception, "Failed to allocate media data buffer", 0);
            RETURN_THROWS();
        }

        int ret = fn_GetMediaData(intern->sdk, indexbuf, ZSTR_VAL(sdk_file_id), proxy, passwd, (int)timeout, mediaData);
        if (ret != 0) {
            fn_FreeMediaData(mediaData);
            smart_str_free(&buffer);
            zend_throw_exception_ex(zend_ce_exception, ret, "Failed to get media data, error code: %d", ret);
            RETURN_THROWS();
        }

        /* Append data to buffer */
        smart_str_appendl(&buffer, mediaData->data, mediaData->data_len);

        is_finish = mediaData->is_finish;
        if (!is_finish) {
            indexbuf = estrdup(mediaData->outindexbuf);
        }

        fn_FreeMediaData(mediaData);

        if (!is_finish && indexbuf) {
            /* indexbuf was duplicated, will be freed after next iteration */
        }
    }

    smart_str_0(&buffer);
    RETVAL_STR(buffer.s);
}
/* }}} */

/* {{{ proto string WeComArchive::getSdkVersion()
   Get SDK version */
PHP_METHOD(WeComArchive, getSdkVersion)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(WECOMARCHIVE_SDK_VERSION);
}
/* }}} */

/* Arginfo definitions for PHP 8.0+ */
ZEND_BEGIN_ARG_INFO_EX(arginfo_wecomarchive_construct, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, options, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_wecomarchive_getChatData, 0, 0, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, seq, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, limit, IS_LONG, 0, "100")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_wecomarchive_decryptData, 0, 2, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, encryptRandomKey, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, encryptChatMsg, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_wecomarchive_decryptChatItem, 0, 1, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, chatItem, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_wecomarchive_getMediaData, 0, 1, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, sdkFileId, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 1, "null")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_wecomarchive_getSdkVersion, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* Class methods */
static const zend_function_entry wecomarchive_methods[] = {
    PHP_ME(WeComArchive, __construct,   arginfo_wecomarchive_construct,    ZEND_ACC_PUBLIC)
    PHP_ME(WeComArchive, getChatData,   arginfo_wecomarchive_getChatData,  ZEND_ACC_PUBLIC)
    PHP_ME(WeComArchive, decryptData,     arginfo_wecomarchive_decryptData,     ZEND_ACC_PUBLIC)
    PHP_ME(WeComArchive, decryptChatItem, arginfo_wecomarchive_decryptChatItem, ZEND_ACC_PUBLIC)
    PHP_ME(WeComArchive, getMediaData,    arginfo_wecomarchive_getMediaData,    ZEND_ACC_PUBLIC)
    PHP_ME(WeComArchive, getSdkVersion, arginfo_wecomarchive_getSdkVersion, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(wecomarchive)
{
    REGISTER_INI_ENTRIES();

    /* Register WeComArchive class */
    zend_class_entry ce;
    INIT_CLASS_ENTRY(ce, "WeComArchive", wecomarchive_methods);
    wecomarchive_ce = zend_register_internal_class(&ce);
    wecomarchive_ce->create_object = wecomarchive_object_create;

    memcpy(&wecomarchive_object_handlers, zend_get_std_object_handlers(), sizeof(wecomarchive_object_handlers));
    wecomarchive_object_handlers.offset = XtOffsetOf(wecomarchive_object, std);
    wecomarchive_object_handlers.free_obj = wecomarchive_object_free;

    /* Register error code constants */
    REGISTER_LONG_CONSTANT("WECOM_ERR_PARAM", 10000, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_NETWORK", 10001, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_PARSE", 10002, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_SYSTEM", 10003, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_ENCRYPT", 10004, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_FILEID", 10005, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_DECRYPT", 10006, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_PRIKEY", 10007, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_ENCKEY", 10008, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_IP", 10009, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_EXPIRED", 10010, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_CERT", 10011, CONST_CS | CONST_PERSISTENT);

    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(wecomarchive)
{
    UNREGISTER_INI_ENTRIES();

    if (sdk_handle) {
        dlclose(sdk_handle);
        sdk_handle = NULL;
    }

    return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(wecomarchive)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "wecomarchive support", "enabled");
    php_info_print_table_row(2, "Version", PHP_WECOMARCHIVE_VERSION);
    php_info_print_table_row(2, "SDK Version", WECOMARCHIVE_SDK_VERSION);
    php_info_print_table_row(2, "SDK Library Path", WECOMARCHIVE_G(sdk_lib_path));
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}
/* }}} */

/* {{{ PHP_GINIT_FUNCTION */
static PHP_GINIT_FUNCTION(wecomarchive)
{
#if defined(COMPILE_DL_WECOMARCHIVE) && defined(ZTS)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    wecomarchive_globals->sdk_lib_path = NULL;
}
/* }}} */

/* {{{ wecomarchive_module_entry */
zend_module_entry wecomarchive_module_entry = {
    STANDARD_MODULE_HEADER,
    "wecomarchive",
    NULL,
    PHP_MINIT(wecomarchive),
    PHP_MSHUTDOWN(wecomarchive),
    NULL,
    NULL,
    PHP_MINFO(wecomarchive),
    PHP_WECOMARCHIVE_VERSION,
    PHP_MODULE_GLOBALS(wecomarchive),
    PHP_GINIT(wecomarchive),
    NULL,
    NULL,
    STANDARD_MODULE_PROPERTIES_EX
};
/* }}} */

#ifdef COMPILE_DL_WECOMARCHIVE
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(wecomarchive)
#endif
