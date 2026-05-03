#!/bin/bash

# Test build on a single remote node to see actual errors

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

# Use first node from CLOUDLAB_NODES
TEST_NODE="${CLOUDLAB_NODES[0]}"
HOST=$(get_full_hostname "$TEST_NODE")

echo "=========================================="
echo "Testing build on ${HOST}..."
echo "=========================================="
echo ""

# Show the build command that will be executed
echo "Build command: ${REMOTE_BUILD_CMD}"
echo ""

# Execute build and show full output
ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" "${REMOTE_BUILD_CMD}"

EXIT_CODE=$?

echo ""
echo "=========================================="
if [ $EXIT_CODE -eq 0 ]; then
    echo "Build succeeded on ${HOST}"
else
    echo "Build failed on ${HOST} with exit code ${EXIT_CODE}"
fi
echo "=========================================="

exit $EXIT_CODE
