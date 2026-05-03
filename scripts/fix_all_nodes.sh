#!/bin/bash

# ============================================
# Fix Installation on All Failed Nodes
# ============================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

echo "========================================"
echo "Fixing Installation on All Nodes"
echo "========================================"
echo ""

# Failed nodes from installation (full hostnames)
FAILED_NODES=(
    "clgpu013.clemson.cloudlab.us"
    "clgpu017.clemson.cloudlab.us"
    "clgpu009.clemson.cloudlab.us"
)

echo "Nodes to fix: ${#FAILED_NODES[@]}"
for node in "${FAILED_NODES[@]}"; do
    echo "  - $node"
done
echo ""

read -p "Continue? (y/n) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Cancelled"
    exit 1
fi

# ============================================
# Step 1: Copy fix script to all nodes
# ============================================

echo "[STEP] Copying fix script to all nodes..."
echo ""

for HOST in "${FAILED_NODES[@]}"; do
    echo "Copying to ${HOST}..."

    scp "${SSH_OPTS[@]}" \
        "${SCRIPT_DIR}/fix_installation.sh" \
        "${USERNAME}@${HOST}:/tmp/fix_installation.sh"
done

echo ""
echo "✓ Fix script copied"
echo ""

# ============================================
# Step 2: Run fix on all nodes
# ============================================

echo "[STEP] Running fix on all nodes..."
echo ""

for HOST in "${FAILED_NODES[@]}"; do
    echo "========================================"
    echo "Node: ${HOST}"
    echo "========================================"

    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" \
        "chmod +x /tmp/fix_installation.sh && bash /tmp/fix_installation.sh"

    echo ""
done

echo ""
echo "========================================"
echo "Fix Complete!"
echo "========================================"
echo ""
echo "Next steps:"
echo "  1. Reboot all nodes:"
echo "     ./scripts/reboot_all_nodes.sh"
echo ""
echo "  2. After reboot, verify NVIDIA driver:"
echo "     for node in clgpu013 clgpu017 clgpu009; do"
echo "       echo \"\$node:\""
echo "       ssh Alexis@\${node}.clemson.cloudlab.us nvidia-smi"
echo "     done"
echo ""
echo "  3. Re-run installation if needed:"
echo "     ./scripts/install_all_nodes.sh"
echo ""
