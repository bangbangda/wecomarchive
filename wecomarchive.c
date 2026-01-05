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
#include "php_wecomarchive.h"

#include <dlfcn.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>

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
    char *private_key;
    size_t private_key_len;
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

/* Object handlers */
static zend_object *wecomarchive_object_create(zend_class_entry *ce) {
    wecomarchive_object *intern = zend_object_alloc(sizeof(wecomarchive_object), ce);

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);

    intern->std.handlers = &wecomarchive_object_handlers;
    intern->sdk = NULL;
    intern->private_key = NULL;
    intern->private_key_len = 0;

    return &intern->std;
}

static void wecomarchive_object_free(zend_object *object) {
    wecomarchive_object *intern = wecomarchive_from_obj(object);

    if (intern->sdk && fn_DestroySdk) {
        fn_DestroySdk(intern->sdk);
        intern->sdk = NULL;
    }

    if (intern->private_key) {
        efree(intern->private_key);
        intern->private_key = NULL;
    }

    zend_object_std_dtor(&intern->std);
}

/* {{{ proto void WeComArchive::__construct(array $options)
   Create a new WeComArchive instance */
PHP_METHOD(WeComArchive, __construct)
{
    zval *options;
    zend_string *corpid = NULL, *secret = NULL, *private_key = NULL, *lib_path = NULL;
    HashTable *options_ht;
    zval *tmp;

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

    /* Get optional private_key */
    tmp = zend_hash_str_find(options_ht, "private_key", sizeof("private_key") - 1);
    if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
        private_key = Z_STR_P(tmp);
    }

    /* Get optional lib_path */
    tmp = zend_hash_str_find(options_ht, "lib_path", sizeof("lib_path") - 1);
    if (tmp && Z_TYPE_P(tmp) == IS_STRING) {
        lib_path = Z_STR_P(tmp);
    }

    /* Load SDK library */
    const char *sdk_path = lib_path ? ZSTR_VAL(lib_path) : WECOMARCHIVE_G(sdk_lib_path);
    if (load_sdk_library(sdk_path) == FAILURE) {
        zend_throw_exception_ex(zend_ce_exception, 0, "Failed to load SDK library from: %s", sdk_path);
        RETURN_THROWS();
    }

    wecomarchive_object *intern = Z_WECOMARCHIVE_P(ZEND_THIS);

    /* Create SDK instance */
    intern->sdk = fn_NewSdk();
    if (!intern->sdk) {
        zend_throw_exception(zend_ce_exception, "Failed to create SDK instance", 0);
        RETURN_THROWS();
    }

    /* Initialize SDK */
    int ret = fn_Init(intern->sdk, ZSTR_VAL(corpid), ZSTR_VAL(secret));
    if (ret != 0) {
        fn_DestroySdk(intern->sdk);
        intern->sdk = NULL;
        zend_throw_exception_ex(zend_ce_exception, ret, "Failed to initialize SDK, error code: %d", ret);
        RETURN_THROWS();
    }

    /* Store private key if provided */
    if (private_key) {
        intern->private_key = estrndup(ZSTR_VAL(private_key), ZSTR_LEN(private_key));
        intern->private_key_len = ZSTR_LEN(private_key);
    }
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

    if (!intern->private_key) {
        zend_throw_exception(zend_ce_exception, "Private key not set. Please provide 'private_key' in constructor options.", 0);
        RETURN_THROWS();
    }

    /* Decrypt the encrypt_random_key using RSA private key */
    size_t decrypted_key_len;
    char *decrypted_key = rsa_decrypt(intern->private_key, intern->private_key_len, ZSTR_VAL(encrypt_random_key), &decrypted_key_len);
    if (!decrypted_key) {
        zend_throw_exception(zend_ce_exception, "Failed to decrypt encrypt_random_key with RSA private key", 0);
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
    PHP_ME(WeComArchive, decryptData,   arginfo_wecomarchive_decryptData,  ZEND_ACC_PUBLIC)
    PHP_ME(WeComArchive, getMediaData,  arginfo_wecomarchive_getMediaData, ZEND_ACC_PUBLIC)
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
