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

#ifndef PHP_WECOMARCHIVE_H
#define PHP_WECOMARCHIVE_H

extern zend_module_entry wecomarchive_module_entry;
#define phpext_wecomarchive_ptr &wecomarchive_module_entry

#define PHP_WECOMARCHIVE_VERSION "1.3.0"

#ifdef PHP_WIN32
# define PHP_WECOMARCHIVE_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define PHP_WECOMARCHIVE_API __attribute__ ((visibility("default")))
#else
# define PHP_WECOMARCHIVE_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

#define WECOMARCHIVE_SDK_VERSION "v3_20250205"
#define WECOMARCHIVE_SDK_X86_URL "https://wwcdn.weixin.qq.com/node/wwcomm/sdk_x86_v3_20250205.tgz"
#define WECOMARCHIVE_SDK_ARM_URL "https://wwcdn.weixin.qq.com/node/wwcomm/sdk_arm_v3_20250205.tgz"

ZEND_BEGIN_MODULE_GLOBALS(wecomarchive)
    char *sdk_lib_path;
ZEND_END_MODULE_GLOBALS(wecomarchive)

#ifdef ZTS
#define WECOMARCHIVE_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(wecomarchive, v)
#else
#define WECOMARCHIVE_G(v) (wecomarchive_globals.v)
#endif

#endif /* PHP_WECOMARCHIVE_H */
