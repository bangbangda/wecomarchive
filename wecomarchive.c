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
#include "ext/standard/md5.h"
#include "ext/standard/php_filestat.h"
#if PHP_VERSION_ID >= 80200
# include "ext/random/php_random.h"
#else
# include "ext/standard/php_random.h"
#endif
#include "php_wecomarchive.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>

/* Error codes (mirrors REGISTER_LONG_CONSTANT in MINIT) */
#define WECOM_ERR_PARAM   10000
#define WECOM_ERR_NETWORK 10001
#define WECOM_ERR_SYSTEM  10003
#define WECOM_ERR_DECRYPT 10006
#define WECOM_ERR_PRIKEY  10007
/* Extension-defined codes, kept outside the SDK's 10000 range */
#define WECOM_ERR_WRITE     20001
#define WECOM_ERR_MD5       20002
#define WECOM_ERR_PATH      20003
#define WECOM_ERR_TIMEOUT   20004  /* the max_seconds option ran out */
#define WECOM_ERR_EXEC_TIME 20005  /* max_execution_time ran out */
#define WECOM_ERR_INDEXBUF  20006  /* the SDK returned an unfinished chunk without a usable outindexbuf */
#define WECOM_ERR_NOT_INIT  20007  /* the object has no SDK instance (constructor did not run or failed) */
#define WECOM_ERR_RESUME    20008  /* saveMediaDataPart(): offset/indexbuf do not match each other or the file */

/* Wait between retries of a chunk: this many seconds times the retry number */
#define WECOM_RETRY_BACKOFF_SECONDS 0.2

#if PHP_VERSION_ID >= 80200
# define WECOMARCHIVE_TIMED_OUT() zend_atomic_bool_load_ex(&EG(timed_out))
#else
# define WECOMARCHIVE_TIMED_OUT() EG(timed_out)
#endif

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

/* Resolve a private key source: treat the value as raw PEM content if it is multi-line
   (PEM bodies always contain newlines, file paths do not) or if it starts with the PEM
   header marker; otherwise treat it as a file path and read its contents. The multi-line
   check tolerates leading metadata such as the "Bag Attributes" block emitted by
   `openssl pkcs12 -out`, matching the old behaviour where the buffer was passed directly
   to PEM_read_bio_PrivateKey. Returns a new zend_string (caller owns the reference) or
   NULL on failure with err_buf populated. */
static zend_string *resolve_private_key_source(const char *src, size_t src_len, char *err_buf, size_t err_buf_size) {
    if (src_len > 0 && memchr(src, '\n', src_len) != NULL) {
        return zend_string_init(src, src_len, 0);
    }
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

/* Look up an option in an options array. A value held by reference (['timeout' => &$t]) is
   read through the reference, and null counts as not given. Returns NULL if not given. */
static zval *find_option(HashTable *options_ht, const char *name, size_t name_len) {
    zval *value = zend_hash_str_find(options_ht, name, name_len);
    if (value) {
        ZVAL_DEREF(value);
        if (Z_TYPE_P(value) == IS_NULL) {
            return NULL;
        }
    }
    return value;
}

#define FIND_OPTION(ht, name) find_option((ht), (name), sizeof(name) - 1)

/* Read a timeout option given in seconds as an int, float or numeric string: anything else,
   and values that are not positive, are rejected rather than silently replaced by the
   default. Fractions are rounded up, since the SDK takes whole seconds. Returns FAILURE
   with a WECOM_ERR_PARAM exception thrown. */
static int parse_seconds_option(zval *value, const char *name, zend_long *seconds) {
    double d;

    switch (Z_TYPE_P(value)) {
        case IS_LONG:
            if (Z_LVAL_P(value) <= 0 || Z_LVAL_P(value) > INT_MAX) {
                goto invalid;
            }
            *seconds = Z_LVAL_P(value);
            return SUCCESS;
        case IS_DOUBLE:
            d = Z_DVAL_P(value);
            break;
        case IS_STRING: {
            zend_long l;
            double dd;
            zend_uchar type = is_numeric_string(Z_STRVAL_P(value), Z_STRLEN_P(value), &l, &dd, 0);
            if (type == IS_LONG) {
                d = (double)l;
            } else if (type == IS_DOUBLE) {
                d = dd;
            } else {
                goto invalid;
            }
            break;
        }
        default:
            goto invalid;
    }

    if (!(d > 0) || !zend_finite(d) || d > (double)INT_MAX) {
        goto invalid;
    }
    *seconds = (zend_long)ceil(d);
    return SUCCESS;

invalid:
    zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PARAM,
        "Option '%s' must be a positive number of seconds (int, float or numeric string)", name);
    return FAILURE;
}

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

        tmp = FIND_OPTION(options_ht, "proxy");
        if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
            proxy = Z_STRVAL_P(tmp);
        }

        tmp = FIND_OPTION(options_ht, "passwd");
        if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
            passwd = Z_STRVAL_P(tmp);
        }

        tmp = FIND_OPTION(options_ht, "timeout");
        if (tmp && parse_seconds_option(tmp, "timeout", &timeout) == FAILURE) {
            RETURN_THROWS();
        }
    }

    wecomarchive_object *intern = Z_WECOMARCHIVE_P(ZEND_THIS);

    if (!intern->sdk) {
        zend_throw_exception(zend_ce_exception, "SDK not initialized", WECOM_ERR_NOT_INIT);
        RETURN_THROWS();
    }

    Slice_t *chatData = fn_NewSlice();
    if (!chatData) {
        zend_throw_exception(zend_ce_exception, "Failed to allocate chat data buffer", WECOM_ERR_SYSTEM);
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
        zend_throw_exception(zend_ce_exception, "Failed to allocate message buffer", WECOM_ERR_SYSTEM);
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
        zend_throw_exception(zend_ce_exception, "Failed to allocate message buffer", WECOM_ERR_SYSTEM);
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

/* Options shared by getMediaData(), saveMediaData() and saveMediaDataPart() */
typedef struct {
    const char *proxy;
    const char *passwd;
    zend_long timeout;    /* per-chunk timeout passed to GetMediaData */
    zend_long retries;    /* per-chunk retries on WECOM_ERR_NETWORK..WECOM_ERR_SYSTEM */
    double max_seconds;   /* wall-clock budget for the whole call; 0 = unlimited */
} media_options;

/* Seconds on a monotonic clock: unaffected by clock changes, and unlike max_execution_time
   (CPU time on Linux) it keeps running while a chunk waits for the network. */
static double media_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Parse proxy/passwd/timeout/retries/max_seconds from $options. proxy/passwd keep the lenient
   handling of the other methods; the rest are validated. Returns FAILURE with an exception thrown. */
static int parse_media_options(zval *options, media_options *opts) {
    if (!options) {
        return SUCCESS;
    }

    HashTable *options_ht = Z_ARRVAL_P(options);
    zval *tmp;

    tmp = FIND_OPTION(options_ht, "proxy");
    if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
        opts->proxy = Z_STRVAL_P(tmp);
    }

    tmp = FIND_OPTION(options_ht, "passwd");
    if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
        opts->passwd = Z_STRVAL_P(tmp);
    }

    tmp = FIND_OPTION(options_ht, "timeout");
    if (tmp && parse_seconds_option(tmp, "timeout", &opts->timeout) == FAILURE) {
        return FAILURE;
    }

    tmp = FIND_OPTION(options_ht, "retries");
    if (tmp) {
        if (Z_TYPE_P(tmp) != IS_LONG || Z_LVAL_P(tmp) < 0) {
            zend_throw_exception(zend_ce_exception, "Option 'retries' must be a non-negative integer", WECOM_ERR_PARAM);
            return FAILURE;
        }
        opts->retries = Z_LVAL_P(tmp);
    }

    tmp = FIND_OPTION(options_ht, "max_seconds");
    if (tmp) {
        double max_seconds;
        if (Z_TYPE_P(tmp) == IS_LONG) {
            max_seconds = (double)Z_LVAL_P(tmp);
        } else if (Z_TYPE_P(tmp) == IS_DOUBLE) {
            max_seconds = Z_DVAL_P(tmp);
        } else {
            max_seconds = 0;
        }
        if (!(max_seconds > 0) || !zend_finite(max_seconds)) {
            zend_throw_exception(zend_ce_exception, "Option 'max_seconds' must be an int or float greater than 0", WECOM_ERR_PARAM);
            return FAILURE;
        }
        opts->max_seconds = max_seconds;
    }

    return SUCCESS;
}

/* Expected md5 from $options as a 32-character hex string, or NULL if not given (borrowed).
   Returns FAILURE with a WECOM_ERR_PARAM exception thrown. */
static int parse_md5_option(zval *options, zend_string **expected_md5) {
    *expected_md5 = NULL;
    if (!options) {
        return SUCCESS;
    }

    zval *tmp = FIND_OPTION(Z_ARRVAL_P(options), "md5");
    if (tmp) {
        if (Z_TYPE_P(tmp) != IS_STRING || Z_STRLEN_P(tmp) != 32
            || strspn(Z_STRVAL_P(tmp), "0123456789abcdefABCDEF") != 32) {
            zend_throw_exception(zend_ce_exception, "Option 'md5' must be a 32-character hexadecimal string", WECOM_ERR_PARAM);
            return FAILURE;
        }
        *expected_md5 = Z_STR_P(tmp);
    }
    return SUCCESS;
}

/* Receives one non-empty media chunk. Returns FAILURE with an exception thrown to stop the download. */
typedef int (*media_chunk_handler)(const char *data, size_t len, void *ctx);

/* Pull a media file chunk by chunk (indexbuf -> outindexbuf until is_finish) and pass every
   non-empty chunk to handler, so callers decide whether to buffer or stream it. A chunk that
   fails with WECOM_ERR_NETWORK..WECOM_ERR_SYSTEM is retried with the same indexbuf, as the
   SDK documentation recommends, after a short, growing pause.

   start_indexbuf is "" for the first chunk, or the outindexbuf a previous call stopped at.
   deadline is a media_now() time after which no further chunk is requested (0 = none); the
   per-chunk timeout is also cut down so that a chunk cannot overrun it by more than a second.

   Returns FAILURE with an exception thrown. On SUCCESS *finished says whether the last chunk
   was reached; if not, the download stopped because the deadline passed and *next_indexbuf is
   the token to continue from (a new string the caller releases). */
static int fetch_media_chunks(WeWorkFinanceSdk_t *sdk, const char *sdk_file_id, const media_options *opts,
                              const char *start_indexbuf, double deadline,
                              media_chunk_handler handler, void *ctx,
                              bool *finished, zend_string **next_indexbuf) {
    char *indexbuf = estrdup(start_indexbuf);
    zend_long attempts = 0;

    *finished = false;
    *next_indexbuf = NULL;

    for (;;) {
        /* Once max_execution_time has passed, the engine kills the process if we keep running
           past hard_timeout. Stop here so the caller can still clean up; the fatal error is
           raised as soon as we return. */
        if (WECOMARCHIVE_TIMED_OUT()) {
            zend_throw_exception(zend_ce_exception, "Media download aborted: maximum execution time exceeded", WECOM_ERR_EXEC_TIME);
            goto fail;
        }

        zend_long timeout = opts->timeout;
        if (deadline > 0) {
            double remaining = deadline - media_now();
            if (remaining <= 0) {
                *next_indexbuf = zend_string_init(indexbuf, strlen(indexbuf), 0);
                efree(indexbuf);
                return SUCCESS;
            }
            double whole = ceil(remaining);
            if (whole < 1) {
                whole = 1;
            }
            if (whole < (double)timeout) {
                timeout = (zend_long)whole;
            }
        }

        MediaData_t *media = fn_NewMediaData();
        if (!media) {
            zend_throw_exception(zend_ce_exception, "Failed to allocate media data buffer", WECOM_ERR_SYSTEM);
            goto fail;
        }

        int ret = fn_GetMediaData(sdk, indexbuf, sdk_file_id, opts->proxy, opts->passwd, (int)timeout, media);
        if (ret != 0) {
            fn_FreeMediaData(media);
            if (ret >= WECOM_ERR_NETWORK && ret <= WECOM_ERR_SYSTEM && attempts < opts->retries) {
                attempts++;
                double pause = WECOM_RETRY_BACKOFF_SECONDS * (double)attempts;
                if (deadline > 0) {
                    double remaining = deadline - media_now();
                    if (remaining < pause) {
                        pause = remaining;
                    }
                }
                if (pause > 0) {
                    usleep((useconds_t)(pause * 1e6));
                }
                continue;
            }
            if (attempts > 0) {
                zend_throw_exception_ex(zend_ce_exception, ret, "Failed to get media data after %lld retries, error code: %d", (long long)attempts, ret);
            } else {
                zend_throw_exception_ex(zend_ce_exception, ret, "Failed to get media data, error code: %d", ret);
            }
            goto fail;
        }
        attempts = 0;

        if (media->data_len > 0 && handler(media->data, (size_t)media->data_len, ctx) == FAILURE) {
            fn_FreeMediaData(media);
            goto fail;
        }

        if (media->is_finish) {
            fn_FreeMediaData(media);
            break;
        }

        /* Without a new token the next call would fetch the same bytes again, forever */
        if (!media->outindexbuf || media->outindexbuf[0] == '\0' || strcmp(media->outindexbuf, indexbuf) == 0) {
            fn_FreeMediaData(media);
            zend_throw_exception(zend_ce_exception, "SDK returned an unfinished media chunk without a new outindexbuf", WECOM_ERR_INDEXBUF);
            goto fail;
        }
        char *next = estrdup(media->outindexbuf);
        efree(indexbuf);
        indexbuf = next;
        fn_FreeMediaData(media);
    }

    efree(indexbuf);
    *finished = true;
    return SUCCESS;

fail:
    efree(indexbuf);
    return FAILURE;
}

static int media_append_to_buffer(const char *data, size_t len, void *ctx) {
    smart_str_appendl((smart_str *)ctx, data, len);
    return SUCCESS;
}

typedef struct {
    php_stream *stream;
    const char *path;     /* destination path, for error messages */
    bool verify_md5;
    PHP_MD5_CTX md5;
    zend_long written;
} media_file_writer;

static int media_write_to_file(const char *data, size_t len, void *ctx) {
    media_file_writer *writer = (media_file_writer *)ctx;

    errno = 0;
    ssize_t n = php_stream_write(writer->stream, data, len);
    if (n < 0 || (size_t)n != len) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to write media data for '%s': %s",
            writer->path, errno ? strerror(errno) : "short write");
        return FAILURE;
    }

    if (writer->verify_md5) {
        PHP_MD5Update(&writer->md5, data, len);
    }
    writer->written += (zend_long)len;
    return SUCCESS;
}

/* Check the destination of saveMediaData(): a plain local path (optionally "file://"),
   allowed by open_basedir, not a directory, inside a directory that already exists.
   Returns a new string with the directory resolved (symlinks and ".." removed), so that the
   temporary file, rename and unlink all refer to the same place: ZTS builds resolve paths
   lexically in VCWD_RENAME/VCWD_UNLINK, while opening a file follows symlinks. Sets
   *replacing, and *replaced to the stat of the file being replaced if there is one.
   Returns NULL with a WECOM_ERR_PATH exception thrown. */
static zend_string *resolve_media_target(zend_string *path, zend_stat_t *replaced, bool *replacing) {
    const char *local = NULL;

    *replacing = false;

    if (ZSTR_LEN(path) == 0) {
        zend_throw_exception(zend_ce_exception, "Target path must not be empty", WECOM_ERR_PATH);
        return NULL;
    }

    if (php_stream_locate_url_wrapper(ZSTR_VAL(path), &local, 0) != &php_plain_files_wrapper || !local || !*local) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "Target path '%s' is not a local file path", ZSTR_VAL(path));
        return NULL;
    }

    size_t local_len = strlen(local);
    const char *slash = zend_memrchr(local, '/', local_len);
    const char *base = slash ? slash + 1 : local;
    if (*base == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "Target path '%s' must name a file, not a directory", ZSTR_VAL(path));
        return NULL;
    }

    /* Before looking at the file system, so nothing is revealed about paths outside it */
    if (php_check_open_basedir_ex(local, 0) != 0) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "Target path '%s' is not within the allowed path(s) of open_basedir", ZSTR_VAL(path));
        return NULL;
    }

    char *dir = estrndup(local, local_len);
    char resolved_dir[MAXPATHLEN];
    zend_stat_t st;
    zend_dirname(dir, local_len);
    if (!VCWD_REALPATH(dir, resolved_dir) || VCWD_STAT(resolved_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "Directory '%s' of target path does not exist", dir);
        efree(dir);
        return NULL;
    }
    efree(dir);

    size_t dir_len = strlen(resolved_dir);
    zend_string *target = zend_strpprintf(0, "%s%s%s", resolved_dir,
        (dir_len > 0 && resolved_dir[dir_len - 1] == '/') ? "" : "/", base);

    if (php_check_open_basedir_ex(ZSTR_VAL(target), 0) != 0) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "Target path '%s' is not within the allowed path(s) of open_basedir", ZSTR_VAL(path));
        zend_string_release(target);
        return NULL;
    }

    /* target is absolute, so plain stat() works in ZTS too, and unlike VCWD_STAT it does not
       report every resolution failure as ENOENT. Only a target that is really missing may be
       created from scratch: an existing file that cannot be inspected would lose its owner,
       permissions and ACL. */
    if (php_sys_stat(ZSTR_VAL(target), &st) != 0) {
        if (errno != ENOENT) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "Cannot inspect target path '%s': %s", ZSTR_VAL(path), strerror(errno));
            zend_string_release(target);
            return NULL;
        }
    } else {
        if (S_ISDIR(st.st_mode)) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "Target path '%s' is a directory", ZSTR_VAL(path));
            zend_string_release(target);
            return NULL;
        }
        if (!S_ISREG(st.st_mode)) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "Target path '%s' exists and is not a regular file", ZSTR_VAL(path));
            zend_string_release(target);
            return NULL;
        }
        *replaced = st;
        *replacing = true;
    }

    return target;
}

#define WECOM_ACL_XATTR "system.posix_acl_access"

/* Give fd the POSIX access ACL of the file at path, or none if that file has none (the new
   file may have inherited the directory's default ACL), so that named users and groups of a
   replaced file neither gain nor lose access. Returns FAILURE with errno set. */
static int media_copy_access_acl(const char *path, int fd) {
    ssize_t len = getxattr(path, WECOM_ACL_XATTR, NULL, 0);
    if (len < 0) {
        if (errno != ENODATA && errno != ENOTSUP) {
            return FAILURE;
        }
        if (fremovexattr(fd, WECOM_ACL_XATTR) != 0 && errno != ENODATA && errno != ENOTSUP) {
            return FAILURE;
        }
        return SUCCESS;
    }

    char *acl = emalloc(len > 0 ? (size_t)len : 1);
    ssize_t got = getxattr(path, WECOM_ACL_XATTR, acl, (size_t)len);
    int result = FAILURE;
    if (got != len) {
        if (got >= 0) {
            errno = ERANGE;  /* the ACL changed in between */
        }
    } else if (fsetxattr(fd, WECOM_ACL_XATTR, acl, (size_t)len, 0) == 0) {
        result = SUCCESS;
    }
    efree(acl);
    return result;
}

/* Flush the directory holding path, so that a rename() or a new entry survives a crash as
   the file's own fsync() does. Best effort: the entry is already in place, and some file
   systems do not support syncing a directory. */
static void media_sync_dir(const char *path) {
    size_t len = strlen(path);
    char *dir = estrndup(path, len);
    zend_dirname(dir, len);

    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(dir, flags);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    efree(dir);
}

/* Flush the file to stable storage. Write errors that are only reported asynchronously
   (EIO, and ENOSPC/EDQUOT on NFS) surface here or at close() rather than at write(). */
static int media_sync_stream(php_stream *stream) {
#if PHP_VERSION_ID >= 80100
    errno = 0;
    return php_stream_sync(stream, false) == PHP_STREAM_OPTION_RETURN_OK ? SUCCESS : FAILURE;
#else
    int fd;

    if (php_stream_cast(stream, PHP_STREAM_AS_FD, (void **)&fd, 0) != SUCCESS) {
        errno = EBADF;
        return FAILURE;
    }
    return fsync(fd) == 0 ? SUCCESS : FAILURE;
#endif
}

/* {{{ proto string WeComArchive::getMediaData(string $sdkFileId, array $options = [])
   Download media file */
PHP_METHOD(WeComArchive, getMediaData)
{
    zend_string *sdk_file_id;
    zval *options = NULL;
    media_options opts = { "", "", 5, 0, 0 };  /* no retries unless asked for, as before */
    double start = media_now();

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(sdk_file_id)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();

    if (parse_media_options(options, &opts) == FAILURE) {
        RETURN_THROWS();
    }

    wecomarchive_object *intern = Z_WECOMARCHIVE_P(ZEND_THIS);

    if (!intern->sdk) {
        zend_throw_exception(zend_ce_exception, "SDK not initialized", WECOM_ERR_NOT_INIT);
        RETURN_THROWS();
    }

    /* Collect all media data chunks */
    smart_str buffer = {0};
    bool finished;
    zend_string *next_indexbuf;
    double deadline = opts.max_seconds > 0 ? start + opts.max_seconds : 0;
    if (fetch_media_chunks(intern->sdk, ZSTR_VAL(sdk_file_id), &opts, "", deadline, media_append_to_buffer, &buffer,
                           &finished, &next_indexbuf) == FAILURE) {
        smart_str_free(&buffer);
        RETURN_THROWS();
    }
    if (!finished) {
        zend_string_release(next_indexbuf);
        smart_str_free(&buffer);
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_TIMEOUT, "Media download exceeded max_seconds (%.3g s)", opts.max_seconds);
        RETURN_THROWS();
    }

    /* smart_str_extract() returns "" when nothing was appended (zero-byte file) */
    RETURN_STR(smart_str_extract(&buffer));
}
/* }}} */

/* {{{ proto int WeComArchive::saveMediaData(string $sdkFileId, string $path, array $options = [])
   Download media file straight to disk, one chunk at a time */
PHP_METHOD(WeComArchive, saveMediaData)
{
    zend_string *sdk_file_id, *path;
    zval *options = NULL;
    zend_string *expected_md5 = NULL;
    media_options opts = { "", "", 5, 2, 0 };
    double start = media_now();  /* max_seconds counts from here */

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(sdk_file_id)
        Z_PARAM_PATH_STR(path)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();

    if (parse_media_options(options, &opts) == FAILURE || parse_md5_option(options, &expected_md5) == FAILURE) {
        RETURN_THROWS();
    }

    wecomarchive_object *intern = Z_WECOMARCHIVE_P(ZEND_THIS);

    if (!intern->sdk) {
        zend_throw_exception(zend_ce_exception, "SDK not initialized", WECOM_ERR_NOT_INIT);
        RETURN_THROWS();
    }

    zend_stat_t replaced = {0};
    bool replacing;
    zend_string *target = resolve_media_target(path, &replaced, &replacing);
    if (!target) {
        RETURN_THROWS();
    }

    /* Write to a uniquely named sibling first, then rename it over the target, so the
       target is either left untouched or replaced by a complete file. */
    unsigned char rand_bytes[8];
    char rand_hex[2 * sizeof(rand_bytes) + 1];
    if (php_random_bytes_throw(rand_bytes, sizeof(rand_bytes)) == FAILURE) {
        zend_string_release(target);
        RETURN_THROWS();
    }
    make_digest_ex(rand_hex, rand_bytes, sizeof(rand_bytes));
    zend_string *tmp_path = zend_strpprintf(0, "%s.part-%s", ZSTR_VAL(target), rand_hex);

    /* The checks on the target follow a final symlink, while the temporary file (and so the entry
       the rename replaces) lives in the target's own directory: it must be allowed by itself.
       Opening below bypasses the stream wrapper that would otherwise check it. */
    if (php_check_open_basedir_ex(ZSTR_VAL(tmp_path), 0) != 0) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "Directory of target path '%s' is not within the allowed path(s) of open_basedir",
            ZSTR_VAL(path));
        zend_string_release(tmp_path);
        zend_string_release(target);
        RETURN_THROWS();
    }

    /* O_EXCL refuses to reuse an existing file. While a file is being replaced, the temporary
       file is only accessible to its owner until the replaced file's owner, group and permissions
       are restored below; a new file gets 0666 minus the umask, as with file_put_contents().
       The data itself is written through a PHP stream. */
    int open_flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    open_flags |= O_CLOEXEC;
#endif
    int fd = VCWD_OPEN_MODE(ZSTR_VAL(tmp_path), open_flags, replacing ? (replaced.st_mode & 0700) : 0666);
    if (fd < 0) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to create temporary file '%s': %s",
            ZSTR_VAL(tmp_path), strerror(errno));
        zend_string_release(tmp_path);
        zend_string_release(target);
        RETURN_THROWS();
    }

    php_stream *stream = php_stream_fopen_from_fd(fd, "wb", NULL);
    if (!stream) {
        close(fd);
        VCWD_UNLINK(ZSTR_VAL(tmp_path));
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to open temporary file '%s'", ZSTR_VAL(tmp_path));
        zend_string_release(tmp_path);
        zend_string_release(target);
        RETURN_THROWS();
    }
    /* Write errors are reported through the exception, not as notices */
    stream->flags |= PHP_STREAM_FLAG_SUPPRESS_ERRORS;

    media_file_writer writer = { .stream = stream, .path = ZSTR_VAL(path), .verify_md5 = expected_md5 != NULL };
    if (writer.verify_md5) {
        PHP_MD5Init(&writer.md5);
    }

    int result = SUCCESS;

    /* Before any data is written, give the new file the replaced file's owner, group, access
       ACL and permissions, or give up: otherwise the permission bits could let other users
       read it (the process's own group, or an ACL that narrowed access). Until then the mode
       given to open() keeps it private, which also zeroes the mask of an inherited ACL.
       The stream owns fd from here on. */
    if (replacing && (fchown(fd, replaced.st_uid, replaced.st_gid) != 0
            || media_copy_access_acl(ZSTR_VAL(target), fd) == FAILURE
            || fchmod(fd, replaced.st_mode & 0777) != 0)) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to keep the owner, group and permissions of '%s': %s",
            ZSTR_VAL(path), strerror(errno));
        result = FAILURE;
    }

    if (result == SUCCESS) {
        bool finished;
        zend_string *next_indexbuf;
        double deadline = opts.max_seconds > 0 ? start + opts.max_seconds : 0;
        result = fetch_media_chunks(intern->sdk, ZSTR_VAL(sdk_file_id), &opts, "", deadline, media_write_to_file, &writer,
                                    &finished, &next_indexbuf);
        if (result == SUCCESS && !finished) {
            zend_string_release(next_indexbuf);
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_TIMEOUT, "Media download of '%s' exceeded max_seconds (%.3g s) after %lld bytes",
                ZSTR_VAL(path), opts.max_seconds, (long long)writer.written);
            result = FAILURE;
        }
    }

    if (result == SUCCESS && media_sync_stream(stream) == FAILURE) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to flush media data for '%s': %s",
            ZSTR_VAL(path), errno ? strerror(errno) : "unknown error");
        result = FAILURE;
    }

    errno = 0;
    if (php_stream_close(stream) != 0 && result == SUCCESS) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to close media file for '%s': %s",
            ZSTR_VAL(path), errno ? strerror(errno) : "unknown error");
        result = FAILURE;
    }

    if (result == SUCCESS && writer.verify_md5) {
        unsigned char digest[16];
        char actual[33];
        PHP_MD5Final(digest, &writer.md5);
        make_digest_ex(actual, digest, sizeof(digest));
        if (strncasecmp(actual, ZSTR_VAL(expected_md5), 32) != 0) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_MD5, "MD5 mismatch for '%s': expected %s, got %s",
                ZSTR_VAL(path), ZSTR_VAL(expected_md5), actual);
            result = FAILURE;
        }
    }

    if (result == SUCCESS) {
        if (VCWD_RENAME(ZSTR_VAL(tmp_path), ZSTR_VAL(target)) != 0) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to move temporary file to '%s': %s",
                ZSTR_VAL(path), strerror(errno));
            result = FAILURE;
        } else {
            /* As rename() does: cached stat results and realpaths (e.g. of a symlink that was
               just replaced) would otherwise still describe the old file */
            php_clear_stat_cache(1, NULL, 0);
            media_sync_dir(ZSTR_VAL(target));
        }
    }

    if (result == FAILURE) {
        VCWD_UNLINK(ZSTR_VAL(tmp_path));
    }

    zend_string_release(tmp_path);
    zend_string_release(target);
    if (result == FAILURE) {
        RETURN_THROWS();
    }
    RETURN_LONG(writer.written);
}
/* }}} */

/* If indexbuf has the form the SDK (v3_20250205) produces, "Range:bytes=<start>-<end>", set
   *start to the byte offset it continues from and return true. Any other token is treated as
   opaque. */
static bool media_indexbuf_offset(const char *indexbuf, zend_long *start) {
    static const char prefix[] = "Range:bytes=";
    if (strncmp(indexbuf, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *digits = indexbuf + sizeof(prefix) - 1;
    char *end;
    errno = 0;
    unsigned long long value = strtoull(digits, &end, 10);
    if (end == digits || *end != '-' || errno != 0 || value > (unsigned long long)ZEND_LONG_MAX) {
        return false;
    }
    *start = (zend_long)value;
    return true;
}

/* Compute the md5 of the file open as fd by reading it from the start in chunks, and compare
   it with expected. Reading through the descriptor that was written checks the very file that
   received the data, even if the path has been pointed elsewhere meanwhile. Returns FAILURE
   with an exception thrown (WECOM_ERR_WRITE if the file cannot be read, WECOM_ERR_MD5 on a
   mismatch). */
static int media_verify_fd_md5(int fd, const char *display_path, zend_string *expected_md5) {
    const size_t buf_size = 512 * 1024;
    char *buf = emalloc(buf_size);
    PHP_MD5_CTX md5;
    off_t pos = 0;

    PHP_MD5Init(&md5);
    for (;;) {
        ssize_t n = pread(fd, buf, buf_size, pos);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            int err = errno;
            efree(buf);
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to read back '%s' for the md5 check: %s", display_path, strerror(err));
            return FAILURE;
        }
        if (n == 0) {
            break;
        }
        PHP_MD5Update(&md5, buf, (size_t)n);
        pos += n;
    }
    efree(buf);

    unsigned char digest[16];
    char actual[33];
    PHP_MD5Final(digest, &md5);
    make_digest_ex(actual, digest, sizeof(digest));
    if (strncasecmp(actual, ZSTR_VAL(expected_md5), 32) != 0) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_MD5, "MD5 mismatch for '%s': expected %s, got %s",
            display_path, ZSTR_VAL(expected_md5), actual);
        return FAILURE;
    }
    return SUCCESS;
}

/* Open the part file of saveMediaDataPart() for writing, positioned at offset, and return the
   descriptor, or -1 with an exception thrown.

   target is the absolute path from resolve_media_target(), and *existing / exists what it found
   there. The file is opened with a plain open(): in ZTS builds VCWD_OPEN would resolve a
   symlinked target through PHP's realpath cache, which may still point at a file the link no
   longer names. Nothing is truncated until the open descriptor is known to be the file that
   was checked, so a path replaced in between (another file, or a symlink to one outside
   open_basedir) is refused with WECOM_ERR_PATH instead of being emptied. A file that did not
   exist is created with O_EXCL for the same reason. With md5 the descriptor is opened for
   reading too, so that the check reads back this file. */
static int media_open_part(const char *target, const char *display_path, const zend_stat_t *existing, bool exists,
                           zend_long offset, bool readable) {
    int flags = (readable ? O_RDWR : O_WRONLY) | (exists ? 0 : O_CREAT | O_EXCL);
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(target, flags, 0666);
    if (fd < 0) {
        if (!exists && errno == EEXIST) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "'%s' was created by someone else while it was being opened", display_path);
        } else if (exists && errno == ENOENT) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "'%s' was removed while it was being opened", display_path);
        } else {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to open '%s' for writing: %s", display_path, strerror(errno));
        }
        return -1;
    }

    zend_stat_t st;
    if (fstat(fd, &st) != 0) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to inspect '%s': %s", display_path, strerror(errno));
        close(fd);
        return -1;
    }
    if (exists && (st.st_dev != existing->st_dev || st.st_ino != existing->st_ino || !S_ISREG(st.st_mode))) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_PATH, "'%s' was replaced while it was being opened", display_path);
        close(fd);
        return -1;
    }
    if ((zend_long)st.st_size < offset) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_RESUME, "Cannot continue at offset %lld: '%s' is only %lld bytes",
            (long long)offset, display_path, (long long)st.st_size);
        close(fd);
        return -1;
    }

    /* offset 0 starts over; offset > 0 drops whatever lies beyond the offset, which the caller
       never recorded (a previous process may have died after writing it) */
    if ((st.st_size != (off_t)offset && ftruncate(fd, (off_t)offset) != 0)
        || lseek(fd, (off_t)offset, SEEK_SET) != (off_t)offset) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to truncate '%s' to %lld bytes: %s",
            display_path, (long long)offset, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* {{{ proto array WeComArchive::saveMediaDataPart(string $sdkFileId, string $partPath, array $options = [])
   Download part of a media file to $partPath, continuing where an earlier call stopped */
PHP_METHOD(WeComArchive, saveMediaDataPart)
{
    zend_string *sdk_file_id, *path;
    zval *options = NULL;
    zend_string *expected_md5 = NULL;
    media_options opts = { "", "", 5, 2, 0 };
    const char *indexbuf = "";
    zend_long offset = 0;
    double start = media_now();  /* max_seconds counts from here */

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STR(sdk_file_id)
        Z_PARAM_PATH_STR(path)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY(options)
    ZEND_PARSE_PARAMETERS_END();

    if (parse_media_options(options, &opts) == FAILURE || parse_md5_option(options, &expected_md5) == FAILURE) {
        RETURN_THROWS();
    }

    if (options) {
        zval *tmp = FIND_OPTION(Z_ARRVAL_P(options), "indexbuf");
        if (tmp) {
            if (Z_TYPE_P(tmp) != IS_STRING) {
                zend_throw_exception(zend_ce_exception, "Option 'indexbuf' must be a string", WECOM_ERR_PARAM);
                RETURN_THROWS();
            }
            indexbuf = Z_STRVAL_P(tmp);
        }

        tmp = FIND_OPTION(Z_ARRVAL_P(options), "offset");
        if (tmp) {
            if (Z_TYPE_P(tmp) != IS_LONG || Z_LVAL_P(tmp) < 0) {
                zend_throw_exception(zend_ce_exception, "Option 'offset' must be a non-negative integer", WECOM_ERR_PARAM);
                RETURN_THROWS();
            }
            offset = Z_LVAL_P(tmp);
        }
    }

    if (offset > 0 && indexbuf[0] == '\0') {
        zend_throw_exception(zend_ce_exception, "Option 'indexbuf' is required to continue from an offset greater than 0", WECOM_ERR_PARAM);
        RETURN_THROWS();
    }

    /* The token encodes the offset it continues from; a pair from different downloads, or a
       token recorded for a different offset, would silently corrupt the file. */
    zend_long token_offset;
    if (media_indexbuf_offset(indexbuf, &token_offset) && token_offset != offset) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_RESUME, "Option 'indexbuf' continues from byte %lld but 'offset' is %lld",
            (long long)token_offset, (long long)offset);
        RETURN_THROWS();
    }

    wecomarchive_object *intern = Z_WECOMARCHIVE_P(ZEND_THIS);

    if (!intern->sdk) {
        zend_throw_exception(zend_ce_exception, "SDK not initialized", WECOM_ERR_NOT_INIT);
        RETURN_THROWS();
    }

    zend_stat_t existing = {0};
    bool exists;
    zend_string *target = resolve_media_target(path, &existing, &exists);
    if (!target) {
        RETURN_THROWS();
    }

    if (offset > 0) {
        if (!exists) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_RESUME, "Cannot continue at offset %lld: '%s' does not exist",
                (long long)offset, ZSTR_VAL(path));
            zend_string_release(target);
            RETURN_THROWS();
        }
        if ((zend_long)existing.st_size < offset) {
            zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_RESUME, "Cannot continue at offset %lld: '%s' is only %lld bytes",
                (long long)offset, ZSTR_VAL(path), (long long)existing.st_size);
            zend_string_release(target);
            RETURN_THROWS();
        }
    }

    int fd = media_open_part(ZSTR_VAL(target), ZSTR_VAL(path), &existing, exists, offset, expected_md5 != NULL);
    if (fd < 0) {
        zend_string_release(target);
        RETURN_THROWS();
    }

    /* From here on the file may have been created, truncated or written: PHP's stat cache
       (filesize() and friends) must not keep describing the old file, whatever the outcome.
       PHP < 8.3 does not do this for writes through a descriptor. */
    int result = SUCCESS;
    bool finished = false;
    zend_string *next_indexbuf = NULL;
    media_file_writer writer = { .path = ZSTR_VAL(path), .verify_md5 = false };

    /* Keeps the file open for the md5 read-back once the stream has closed its descriptor */
    int md5_fd = -1;
    if (expected_md5 && (md5_fd = dup(fd)) < 0) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to open '%s' for the md5 check: %s", ZSTR_VAL(path), strerror(errno));
        close(fd);
        result = FAILURE;
        goto done;
    }

    php_stream *stream = php_stream_fopen_from_fd(fd, "wb", NULL);
    if (!stream) {
        close(fd);
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to open '%s' for writing", ZSTR_VAL(path));
        result = FAILURE;
        goto done;
    }
    stream->flags |= PHP_STREAM_FLAG_SUPPRESS_ERRORS;

    /* The md5 is checked by reading the finished file back: a running digest cannot survive
       between calls */
    writer.stream = stream;
    double deadline = opts.max_seconds > 0 ? start + opts.max_seconds : 0;

    result = fetch_media_chunks(intern->sdk, ZSTR_VAL(sdk_file_id), &opts, indexbuf, deadline, media_write_to_file, &writer,
                                &finished, &next_indexbuf);

    /* Whatever was written stays in the file, so flush it to disk even when giving up: the
       caller may keep the bytes it has recorded. Only a successful call reports sync errors. */
    if (media_sync_stream(stream) == FAILURE && result == SUCCESS) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to flush media data for '%s': %s",
            ZSTR_VAL(path), errno ? strerror(errno) : "unknown error");
        result = FAILURE;
    }

    errno = 0;
    if (php_stream_close(stream) != 0 && result == SUCCESS) {
        zend_throw_exception_ex(zend_ce_exception, WECOM_ERR_WRITE, "Failed to close media file for '%s': %s",
            ZSTR_VAL(path), errno ? strerror(errno) : "unknown error");
        result = FAILURE;
    }

    if (result == SUCCESS) {
        /* A file created by this call needs its directory entry on disk as well */
        media_sync_dir(ZSTR_VAL(target));
    }

    if (result == SUCCESS && finished && expected_md5) {
        result = media_verify_fd_md5(md5_fd, ZSTR_VAL(path), expected_md5);
    }

done:
    if (md5_fd >= 0) {
        close(md5_fd);
    }
    php_clear_stat_cache(0, NULL, 0);
    zend_string_release(target);
    if (result == FAILURE) {
        if (next_indexbuf) {
            zend_string_release(next_indexbuf);
        }
        RETURN_THROWS();
    }

    array_init(return_value);
    add_assoc_bool(return_value, "finished", finished);
    if (finished) {
        add_assoc_stringl(return_value, "indexbuf", "", 0);
    } else {
        add_assoc_str(return_value, "indexbuf", next_indexbuf);  /* takes over the reference */
    }
    add_assoc_long(return_value, "bytes", offset + writer.written);
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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_wecomarchive_saveMediaData, 0, 2, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, sdkFileId, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 0, "[]")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_wecomarchive_saveMediaDataPart, 0, 2, IS_ARRAY, 0)
    ZEND_ARG_TYPE_INFO(0, sdkFileId, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, partPath, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, options, IS_ARRAY, 0, "[]")
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
    PHP_ME(WeComArchive, saveMediaData,   arginfo_wecomarchive_saveMediaData,   ZEND_ACC_PUBLIC)
    PHP_ME(WeComArchive, saveMediaDataPart, arginfo_wecomarchive_saveMediaDataPart, ZEND_ACC_PUBLIC)
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
    REGISTER_LONG_CONSTANT("WECOM_ERR_WRITE", WECOM_ERR_WRITE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_MD5", WECOM_ERR_MD5, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_PATH", WECOM_ERR_PATH, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_TIMEOUT", WECOM_ERR_TIMEOUT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_EXEC_TIME", WECOM_ERR_EXEC_TIME, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_INDEXBUF", WECOM_ERR_INDEXBUF, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_NOT_INIT", WECOM_ERR_NOT_INIT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("WECOM_ERR_RESUME", WECOM_ERR_RESUME, CONST_CS | CONST_PERSISTENT);

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
