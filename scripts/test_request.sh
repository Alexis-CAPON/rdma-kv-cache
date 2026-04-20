#!/bin/bash

# ============================================
# Test Request - End-to-End Test
# ============================================
# Sends a request to the orchestrator and waits for response

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

# ============================================
# Configuration
# ============================================

PROMPT="${1:-Once upon a time}"
MAX_TOKENS="${2:-50}"
TEMPERATURE="${3:-0.7}"

# Get orchestrator address
ORCHESTRATOR_NODE="${CLOUDLAB_NODES[0]}"
ORCHESTRATOR_HOST=$(get_full_hostname "$ORCHESTRATOR_NODE")

log_info "=========================================="
log_info "Testing Disaggregated LLM Inference"
log_info "=========================================="
echo ""
log_info "Orchestrator: ${ORCHESTRATOR_HOST}:${ORCHESTRATOR_PORT}"
log_info "Prompt: \"${PROMPT}\""
log_info "Max tokens: ${MAX_TOKENS}"
echo ""

# ============================================
# Create Request JSON
# ============================================

REQUEST_JSON=$(cat <<EOF
{
  "model": "${MODEL_NAME}",
  "prompt": "${PROMPT}",
  "max_tokens": ${MAX_TOKENS},
  "temperature": ${TEMPERATURE},
  "stream": false
}
EOF
)

# ============================================
# Send Request
# ============================================

log_step "Sending request to orchestrator..."
echo ""

RESPONSE=$(curl -s -X POST \
    -H "Content-Type: application/json" \
    -d "${REQUEST_JSON}" \
    "http://${ORCHESTRATOR_HOST}:${ORCHESTRATOR_PORT}/v1/completions" \
    --max-time 60)

CURL_EXIT_CODE=$?

echo ""

# ============================================
# Check Response
# ============================================

if [ $CURL_EXIT_CODE -ne 0 ]; then
    log_error "Request failed (curl exit code: ${CURL_EXIT_CODE})"
    log_info "Is the orchestrator running?"
    log_info "Check logs: ssh ${USERNAME}@${ORCHESTRATOR_HOST} 'tail -100 ${REMOTE_DIR}/logs/orchestrator.log'"
    exit 1
fi

# Parse response
if echo "$RESPONSE" | jq . > /dev/null 2>&1; then
    log_success "Received response!"
    echo ""

    # Extract generated text
    GENERATED_TEXT=$(echo "$RESPONSE" | jq -r '.choices[0].text // .error // "No text in response"')

    echo "=========================================="
    echo "Generated Text:"
    echo "=========================================="
    echo ""
    echo "$GENERATED_TEXT"
    echo ""

    # Show full response if verbose
    if [ "${VERBOSE:-0}" == "1" ]; then
        echo "=========================================="
        echo "Full Response:"
        echo "=========================================="
        echo "$RESPONSE" | jq .
        echo ""
    fi

    # Check for errors in response
    ERROR=$(echo "$RESPONSE" | jq -r '.error // empty')
    if [ -n "$ERROR" ]; then
        log_error "Server returned error: $ERROR"
        exit 1
    fi

    log_success "Test completed successfully!"
else
    log_error "Invalid JSON response:"
    echo "$RESPONSE"
    exit 1
fi

echo ""
log_info "=========================================="
log_info "System Health Check"
log_info "=========================================="
echo ""

# Check orchestrator status
log_info "Orchestrator status:"
ORCH_STATUS=$(curl -s "http://${ORCHESTRATOR_HOST}:${ORCHESTRATOR_PORT}/health" || echo "UNREACHABLE")
echo "  $ORCH_STATUS"
echo ""

log_info "View logs:"
echo "  Orchestrator: ssh ${USERNAME}@${ORCHESTRATOR_HOST} 'tail -100 ${REMOTE_DIR}/logs/orchestrator.log'"
echo ""
