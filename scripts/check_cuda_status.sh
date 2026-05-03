#!/bin/bash

# ============================================
# Check CUDA Installation Status on All Nodes
# ============================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

echo "=========================================="
echo "Checking CUDA Status on All Nodes"
echo "=========================================="
echo ""

for NODE in "${CLOUDLAB_NODES[@]:0:3}"; do
    HOST=$(get_full_hostname "$NODE")

    echo "=========================================="
    echo "Node: ${HOST}"
    echo "=========================================="

    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" bash <<'EOF'
echo "1. NVIDIA Driver:"
if command -v nvidia-smi &> /dev/null; then
    nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>&1 || echo "   Driver not loaded (reboot needed?)"
else
    echo "   nvidia-smi not found"
fi

echo ""
echo "2. CUDA Toolkit (nvcc):"
if command -v nvcc &> /dev/null; then
    nvcc --version | grep "release"
else
    echo "   nvcc not found"
fi

echo ""
echo "3. CUDA packages installed:"
dpkg -l | grep -E "nvidia-driver|nvidia-cuda|cuda-" | grep "^ii" | wc -l
if [ $(dpkg -l | grep -E "nvidia-driver|nvidia-cuda|cuda-" | grep "^ii" | wc -l) -gt 0 ]; then
    echo "   Installed packages:"
    dpkg -l | grep -E "nvidia-driver|nvidia-cuda|cuda-" | grep "^ii" | awk '{print "   - " $2}' | head -5
else
    echo "   No NVIDIA/CUDA packages found"
fi

echo ""
echo "4. System reboot status:"
echo "   Uptime: $(uptime -p)"
echo "   Last reboot: $(who -b | awk '{print $3, $4}')"

echo ""
echo "5. CUDA in PATH:"
echo $PATH | grep -o "[^:]*cuda[^:]*" || echo "   No CUDA in PATH"

echo ""
echo "6. Recommended action:"
if command -v nvidia-smi &> /dev/null; then
    if nvidia-smi &> /dev/null; then
        echo "   ✓ CUDA is working"
    else
        echo "   ⚠ NVIDIA driver installed but not loaded - REBOOT REQUIRED"
    fi
elif [ $(dpkg -l | grep nvidia-driver | grep "^ii" | wc -l) -gt 0 ]; then
    echo "   ⚠ NVIDIA driver installed but not loaded - REBOOT REQUIRED"
else
    echo "   ✗ CUDA not installed - Run: USE_GPU=true ./scripts/install_all_nodes.sh"
fi
EOF

    echo ""
done

echo "=========================================="
echo "Summary"
echo "=========================================="
echo ""
echo "If nodes need reboot: ./scripts/reboot_all_nodes.sh"
echo "If CUDA not installed: USE_GPU=true ./scripts/install_all_nodes.sh"
echo ""
