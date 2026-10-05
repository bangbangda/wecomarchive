# Dockerfile for building and testing wecomarchive extension
# Build from project root: docker build -f tests/Dockerfile .
ARG PHP_VERSION=8.0
# cli (NTS) or zts
ARG PHP_VARIANT=cli
FROM php:${PHP_VERSION}-${PHP_VARIANT}

# 1 installs valgrind for leak checks (VALGRIND=1 at run time)
ARG WITH_VALGRIND=0

# Install dependencies. Debian 11 (bullseye, the base of the PHP 8.0 image) now lives on
# archive.debian.org; its packages are gone from deb.debian.org.
RUN if grep -q '^11\.' /etc/debian_version; then \
        sed -i 's|http://deb.debian.org|http://archive.debian.org|g' /etc/apt/sources.list; \
    fi \
    && apt-get -o Acquire::Retries=3 -o Acquire::Check-Valid-Until=false update \
    && apt-get -o Acquire::Retries=3 install -y \
    libssl-dev \
    pkg-config \
    curl \
    $( [ "$WITH_VALGRIND" = "1" ] && echo valgrind ) \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /ext

# Download WeCom SDK
COPY scripts ./scripts/
RUN chmod +x scripts/download-sdk.sh && ./scripts/download-sdk.sh

# Test-only tools: setfacl/getfacl for the ACL tests in tests/test_mock.php
RUN apt-get -o Acquire::Retries=3 -o Acquire::Check-Valid-Until=false update \
    && apt-get -o Acquire::Retries=3 install -y acl \
    && rm -rf /var/lib/apt/lists/*

# Build extension
COPY wecomarchive.c php_wecomarchive.h config.m4 ./
RUN phpize \
    && ./configure \
    && make \
    && make install

# Enable extension
RUN echo "extension=wecomarchive.so" > /usr/local/etc/php/conf.d/wecomarchive.ini

# Verify installation
RUN php -m | grep wecomarchive

# Tests, plus the mock SDK and the I/O fault-injection shim used by tests/test_mock.php
COPY tests ./tests/
RUN gcc -shared -fPIC -O2 -Wall -o /usr/local/lib/libMockWeComSdk.so tests/mock-sdk/mock_sdk.c \
    && gcc -shared -fPIC -O2 -Wall -o /usr/local/lib/libFailIo.so tests/mock-sdk/fail_io.c -ldl

# Run tests by default
CMD ["sh", "/ext/tests/run-suite.sh"]
