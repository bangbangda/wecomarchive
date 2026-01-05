# Dockerfile for building and testing wecomarchive extension
# Build from project root: docker build -f tests/Dockerfile .
ARG PHP_VERSION=8.0
FROM php:${PHP_VERSION}-cli

# Install dependencies
RUN apt-get update && apt-get install -y \
    libssl-dev \
    pkg-config \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy extension source
WORKDIR /ext
COPY wecomarchive.c php_wecomarchive.h config.m4 ./
COPY scripts ./scripts/
COPY tests/test.php ./tests/

# Download WeCom SDK
RUN chmod +x scripts/download-sdk.sh && ./scripts/download-sdk.sh

# Build extension
RUN phpize \
    && ./configure \
    && make \
    && make install

# Enable extension
RUN echo "extension=wecomarchive.so" > /usr/local/etc/php/conf.d/wecomarchive.ini

# Verify installation
RUN php -m | grep wecomarchive

# Run tests by default
CMD ["php", "/ext/tests/test.php"]
