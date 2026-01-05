#!/bin/bash
#
# Run wecomarchive extension tests using Docker
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

echo -e "${GREEN}=== WeComArchive Extension Test Runner ===${NC}"
echo "PHP Version: ${PHP_VERSION}"
echo "Project Dir: ${PROJECT_DIR}"
echo ""

# Build Docker image
echo -e "${YELLOW}Building test image...${NC}"
docker build \
    --build-arg PHP_VERSION="${PHP_VERSION}" \
    -t wecomarchive-test:php${PHP_VERSION} \
    -f "${SCRIPT_DIR}/Dockerfile" \
    "${PROJECT_DIR}"

echo ""
echo -e "${YELLOW}Checking extension info...${NC}"
docker run --rm wecomarchive-test:php${PHP_VERSION} php --ri wecomarchive

# Run tests
echo ""
echo -e "${YELLOW}Running tests...${NC}"
echo ""

if docker run --rm wecomarchive-test:php${PHP_VERSION}; then
    echo ""
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo ""
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
fi
