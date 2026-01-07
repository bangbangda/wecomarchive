dnl config.m4 for extension wecomarchive

PHP_ARG_ENABLE([wecomarchive],
  [whether to enable wecomarchive support],
  [AS_HELP_STRING([--enable-wecomarchive],
    [Enable wecomarchive support])],
  [no])

PHP_ARG_WITH([wecomarchive-sdk-path],
  [for WeCom SDK library path],
  [AS_HELP_STRING([--with-wecomarchive-sdk-path=DIR],
    [Path to WeCom SDK library directory (default: /usr/local/lib)])],
  [/usr/local/lib])

if test "$PHP_WECOMARCHIVE" != "no"; then
  dnl Check for dlopen support
  AC_CHECK_LIB(dl, dlopen, [
    PHP_ADD_LIBRARY(dl, 1, WECOMARCHIVE_SHARED_LIBADD)
  ], [
    AC_MSG_ERROR([dlopen not found, please install libdl])
  ])

  dnl Check for OpenSSL (required for RSA decryption)
  PKG_CHECK_MODULES([OPENSSL], [openssl >= 1.1.0], [
    PHP_EVAL_INCLINE($OPENSSL_CFLAGS)
    PHP_EVAL_LIBLINE($OPENSSL_LIBS, WECOMARCHIVE_SHARED_LIBADD)
  ], [
    AC_MSG_ERROR([OpenSSL >= 1.1.0 not found])
  ])

  dnl Check and download WeCom SDK if needed
  SDK_LIB_NAME="libWeWorkFinanceSdk_C.so"
  SDK_LIB_PATH="$PHP_WECOMARCHIVE_SDK_PATH/$SDK_LIB_NAME"

  AC_MSG_CHECKING([for WeCom SDK at $SDK_LIB_PATH])
  if test -f "$SDK_LIB_PATH"; then
    AC_MSG_RESULT([found])
  else
    AC_MSG_RESULT([not found])
    AC_MSG_NOTICE([WeCom SDK not found, attempting to download...])

    dnl Try to download SDK automatically
    if test -f "$srcdir/scripts/download-sdk.sh"; then
      AC_MSG_NOTICE([Running download-sdk.sh to install WeCom SDK...])
      chmod +x "$srcdir/scripts/download-sdk.sh"

      dnl Try to download SDK to the specified path
      if "$srcdir/scripts/download-sdk.sh" -p "$PHP_WECOMARCHIVE_SDK_PATH" 2>/dev/null; then
        AC_MSG_NOTICE([WeCom SDK downloaded successfully to $SDK_LIB_PATH])
      else
        AC_MSG_WARN([])
        AC_MSG_WARN([************************************************************])
        AC_MSG_WARN([* WeCom SDK could not be downloaded automatically.        *])
        AC_MSG_WARN([* Please download it manually using:                      *])
        AC_MSG_WARN([*                                                          *])
        AC_MSG_WARN([*   ./scripts/download-sdk.sh -p $PHP_WECOMARCHIVE_SDK_PATH])
        AC_MSG_WARN([*                                                          *])
        AC_MSG_WARN([* Or if installed via PIE/Composer:                       *])
        AC_MSG_WARN([*   vendor/bin/download-sdk.sh -p $PHP_WECOMARCHIVE_SDK_PATH])
        AC_MSG_WARN([*                                                          *])
        AC_MSG_WARN([* The extension will still compile, but you MUST install  *])
        AC_MSG_WARN([* the SDK before using the extension at runtime.          *])
        AC_MSG_WARN([************************************************************])
        AC_MSG_WARN([])
      fi
    else
      AC_MSG_WARN([])
      AC_MSG_WARN([************************************************************])
      AC_MSG_WARN([* Download script not found!                               *])
      AC_MSG_WARN([* Please manually download WeCom SDK from:                 *])
      AC_MSG_WARN([*   https://developer.work.weixin.qq.com/document/path/91774 *])
      AC_MSG_WARN([* And place libWeWorkFinanceSdk_C.so in:                   *])
      AC_MSG_WARN([*   $PHP_WECOMARCHIVE_SDK_PATH                             *])
      AC_MSG_WARN([************************************************************])
      AC_MSG_WARN([])
    fi
  fi

  PHP_SUBST(WECOMARCHIVE_SHARED_LIBADD)
  PHP_NEW_EXTENSION(wecomarchive, wecomarchive.c, $ext_shared)
fi
