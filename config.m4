dnl config.m4 for extension wecomarchive

PHP_ARG_ENABLE([wecomarchive],
  [whether to enable wecomarchive support],
  [AS_HELP_STRING([--enable-wecomarchive],
    [Enable wecomarchive support])],
  [no])

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

  PHP_SUBST(WECOMARCHIVE_SHARED_LIBADD)
  PHP_NEW_EXTENSION(wecomarchive, wecomarchive.c, $ext_shared)
fi
