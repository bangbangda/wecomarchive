#!/bin/bash
#
# Run wecomarchive extension tests using Docker
#
# Usage: tests/run-tests.sh [PHP_VERSION] [PLATFORM]
#   PHP_VERSION  e.g. 8.0 (default), 8.4, or 8.4-zts for a thread-safe build
#   PLATFORM     e.g. linux/amd64 or linux/arm64 (default: the host's)
#
# Env:
#   VALGRIND=1                 also check the streaming tests for leaks with valgrind
#   WECOM_MOCK_LARGE_MB        size of the large-file memory test (default 300; 8 under valgrind)
#   WECOM_CORPID, WECOM_SECRET, WECOM_SDKFILEID, WECOM_MD5 [, WECOM_FILESIZE, WECOM_PROXY, WECOM_PASSWD]
#                              enable the integration test against the real API
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Default PHP version
PHP_VERSION="${1:-8.0}"
PLATFORM="${2:-}"

PHP_VARIANT="cli"
if [[ "$PHP_VERSION" == *-zts ]]; then
    PHP_VARIANT="zts"
    PHP_VERSION="${PHP_VERSION%-zts}"
fi

IMAGE="wecomarchive-test:php${PHP_VERSION}-${PHP_VARIANT}"
PLATFORM_ARGS=()
if [ -n "$PLATFORM" ]; then
    IMAGE="${IMAGE}-${PLATFORM//\//-}"
    PLATFORM_ARGS=(--platform "$PLATFORM")
fi
if [ "${VALGRIND:-0}" = "1" ]; then
    IMAGE="${IMAGE}-valgrind"
fi

echo -e "${GREEN}=== WeComArchive Extension Test Runner ===${NC}"
echo "PHP Version: ${PHP_VERSION} (${PHP_VARIANT})"
echo "Platform:    ${PLATFORM:-host}"
echo "Project Dir: ${PROJECT_DIR}"
echo ""

# Build Docker image
echo -e "${YELLOW}Building test image...${NC}"
docker build \
    "${PLATFORM_ARGS[@]}" \
    --build-arg PHP_VERSION="${PHP_VERSION}" \
    --build-arg PHP_VARIANT="${PHP_VARIANT}" \
    --build-arg WITH_VALGRIND="${VALGRIND:-0}" \
    -t "${IMAGE}" \
    -f "${SCRIPT_DIR}/Dockerfile" \
    "${PROJECT_DIR}"

# Variables passed through only when set on the host; a 1 MB tmpfs backs the disk-full test
RUN_ARGS=("${PLATFORM_ARGS[@]}" --rm --tmpfs /small:size=1m -e WECOM_SMALL_FS=/small)
for var in VALGRIND WECOM_MOCK_LARGE_MB WECOM_CORPID WECOM_SECRET WECOM_SDKFILEID WECOM_MD5 \
           WECOM_FILESIZE WECOM_PROXY WECOM_PASSWD; do
    RUN_ARGS+=(-e "$var")
done

echo ""
echo -e "${YELLOW}Checking extension info...${NC}"
docker run "${RUN_ARGS[@]}" "${IMAGE}" php --ri wecomarchive

# Run tests
echo ""
echo -e "${YELLOW}Running tests...${NC}"
echo ""

if docker run "${RUN_ARGS[@]}" "${IMAGE}"; then
    echo ""
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo ""
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
fi
