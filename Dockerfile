# Dockerfile for building and testing wecomarchive extension
FROM php:8.0-cli

# Install dependencies
RUN apt-get update && apt-get install -y \
    libssl-dev \
    pkg-config \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Copy extension source
WORKDIR /ext
COPY . .

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

CMD ["php", "-i"]
