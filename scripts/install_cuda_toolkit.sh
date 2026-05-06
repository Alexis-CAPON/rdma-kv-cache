#!/bin/bash

# ============================================
# Install CUDA Toolkit on nodes missing it
# ============================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/deploy_config.sh"

echo "=========================================="
echo "Installing CUDA Toolkit on Missing Nodes"
echo "=========================================="
echo ""

# Nodes to install on (adjust as needed)
TARGET_NODES=("d8545-10s10501.wisc.cloudlab.us" "d8545-10s10305.wisc.cloudlab.us" "d8545-10s10301.wisc.cloudlab.us")

for NODE in "${TARGET_NODES[@]}"; do
    HOST=$(get_full_hostname "$NODE")

    echo "=========================================="
    echo "Installing on: ${HOST}"
    echo "=========================================="

    ssh "${SSH_OPTS[@]}" "${USERNAME}@${HOST}" bash <<'EOF'
echo "Installing CUDA toolkit..."

# Install nvidia-cuda-toolkit package (includes nvcc)
sudo apt-get update -qq
sudo apt-get install -y nvidia-cuda-toolkit

echo ""
echo "Verifying installation..."
if command -v nvcc &> /dev/null; then
    echo "✓ nvcc installed successfully:"
    nvcc --version | grep "release"
else
    echo "✗ nvcc installation failed"
    exit 1
fi

echo ""
echo "Adding CUDA to PATH permanently..."
if ! grep -q "/usr/local/cuda/bin" ~/.bashrc 2>/dev/null; then
    echo 'export PATH=/usr/local/cuda/bin:/usr/bin:$PATH' >> ~/.bashrc
    echo "✓ Added to ~/.bashrc"
else
    echo "✓ Already in ~/.bashrc"
fi

echo ""
echo "Installation complete!"
EOF

    echo ""
done

echo "=========================================="
echo "Installation Complete"
echo "=========================================="
echo ""
echo "Next: Run deployment"
echo "  ./scripts/smart_deploy.sh"
echo ""
