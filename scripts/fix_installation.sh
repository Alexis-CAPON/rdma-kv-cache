#!/bin/bash

# ============================================
# Fix Installation Issues
# ============================================

set -e

echo "========================================"
echo "Fixing Installation Issues"
echo "========================================"
echo ""

# ============================================
# 1. Clean up disk space
# ============================================

echo "[1/3] Cleaning up disk space..."

# Remove pip cache
echo "  Removing pip cache..."
rm -rf ~/.cache/pip/*

# Clean apt cache
echo "  Cleaning apt cache..."
sudo apt-get clean
sudo apt-get autoclean
sudo apt-get autoremove -y

# Remove old kernels (keep current)
echo "  Removing old kernels..."
CURRENT_KERNEL=$(uname -r)
sudo apt-get remove --purge -y $(dpkg -l | grep linux-image | grep -v "$CURRENT_KERNEL" | awk '{print $2}') 2>/dev/null || true
sudo apt-get remove --purge -y $(dpkg -l | grep linux-headers | grep -v "$CURRENT_KERNEL" | awk '{print $2}') 2>/dev/null || true

# Remove old logs
echo "  Cleaning old logs..."
sudo journalctl --vacuum-time=3d || true
sudo rm -rf /var/log/*.gz /var/log/*.1 /var/log/*.old 2>/dev/null || true

# Clean tmp directories
echo "  Cleaning tmp directories..."
sudo rm -rf /tmp/* /var/tmp/* 2>/dev/null || true

# Show disk space
echo ""
echo "  Disk space after cleanup:"
df -h / | tail -1

echo ""
echo "✓ Cleanup complete"

# ============================================
# 2. Verify NVIDIA driver installation
# ============================================

echo ""
echo "[2/3] Verifying NVIDIA driver..."

if command -v nvidia-smi &> /dev/null; then
    echo "  nvidia-smi found, checking version..."

    # Try simple command first
    if nvidia-smi -L &> /dev/null; then
        echo "  ✓ NVIDIA driver is working"
        nvidia-smi -L
    else
        echo "  ⚠ NVIDIA driver installed but not working properly"
        echo "  This usually means a reboot is required"
    fi
else
    echo "  ✗ nvidia-smi not found"
    echo "  Checking if driver package is installed..."

    if dpkg -l | grep -q nvidia-driver-570-server; then
        echo "  ✓ Driver package is installed"
        echo "  ⚠ Reboot required to load driver"
    else
        echo "  ✗ Driver package not installed"
        echo "  Installing nvidia-driver-570-server..."
        sudo apt-get update
        sudo apt-get install -y nvidia-driver-570-server
        echo "  ✓ Driver installed, reboot required"
    fi
fi

# ============================================
# 3. Re-run vLLM installation if needed
# ============================================

echo ""
echo "[3/3] Checking vLLM installation..."

PROJECT_DIR="${HOME}/rdma-kv-cache"
VENV_DIR="${PROJECT_DIR}/venv"

if [ ! -d "$VENV_DIR" ]; then
    echo "  Creating virtual environment..."
    python3 -m venv "$VENV_DIR"
fi

source "${VENV_DIR}/bin/activate"

if python -c "import vllm" 2>/dev/null; then
    echo "  ✓ vLLM is already installed"
    python -c "import vllm; print(f'  Version: {vllm.__version__}')"
else
    echo "  Installing vLLM (this may take 10-15 minutes)..."
    pip install --upgrade pip setuptools wheel
    pip install vllm
    echo "  ✓ vLLM installed"
fi

echo ""
echo "========================================"
echo "Fix Complete!"
echo "========================================"
echo ""
echo "Next steps:"
echo "  1. REBOOT this node for NVIDIA driver to load:"
echo "     sudo reboot"
echo "  2. After reboot, verify:"
echo "     nvidia-smi"
echo "     lsmod | grep nvidia"
echo ""
