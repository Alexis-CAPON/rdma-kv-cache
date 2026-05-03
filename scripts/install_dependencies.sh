#!/bin/bash

# ============================================
# Install dependencies for GPUDirect RDMA system
# CloudLab Clemson r7525 nodes (Ubuntu 24.04)
# ============================================

set -e

echo "========================================"
echo "Installing GPUDirect RDMA Dependencies"
echo "CloudLab Clemson r7525 (Ubuntu 24.04)"
echo "========================================"
echo ""

# ============================================
# 1. System Update
# ============================================

echo "[1/10] Updating package list..."
sudo apt-get update -qq

# ============================================
# 2. Build Essentials
# ============================================

echo "[2/10] Installing build essentials..."
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    pybind11-dev

# ============================================
# 3. RDMA/InfiniBand Core Libraries
# ============================================

echo "[3/10] Installing RDMA/InfiniBand libraries..."
sudo apt-get install -y \
    libibverbs-dev \
    librdmacm-dev \
    ibverbs-utils \
    rdma-core

echo "  Checking InfiniBand devices..."
if command -v ibv_devices &> /dev/null; then
    ibv_devices || echo "  No IB devices found (may need kernel modules)"
fi

# ============================================
# 4. NVIDIA CUDA and Driver
# ============================================

# ============================================
# 4. NVIDIA CUDA and Driver  (GPU path only)
# ============================================

if [ "${USE_GPU:-true}" = "true" ]; then
    echo "[4/10] Checking CUDA installation (USE_GPU=true)..."

    if command -v nvidia-smi &> /dev/null; then
        echo "  NVIDIA driver appears to be installed:"
        nvidia-smi --query-gpu=name,driver_version,cuda_version --format=csv,noheader | head -1
    else
        echo "  WARNING: CUDA not detected. Installing NVIDIA drivers..."

        # Install NVIDIA server driver 570
        echo "  Installing NVIDIA server driver 570..."
        sudo apt-get install -y nvidia-driver-570-server

        echo "  NVIDIA driver installed. REBOOT REQUIRED for driver to take effect."
    fi

    # Check and install CUDA toolkit (nvcc)
    echo ""
    echo "  Checking CUDA toolkit (nvcc)..."
    if command -v nvcc &> /dev/null; then
        echo "  CUDA toolkit already installed:"
        nvcc --version | grep "release"
    else
        echo "  Installing CUDA toolkit (nvidia-cuda-toolkit)..."
        sudo apt-get install -y nvidia-cuda-toolkit

        if command -v nvcc &> /dev/null; then
            echo "  ✓ CUDA toolkit installed successfully:"
            nvcc --version | grep "release"
        else
            echo "  ⚠ CUDA toolkit installation may have failed"
        fi
    fi

    # Add CUDA to PATH permanently
    echo ""
    echo "  Configuring CUDA PATH..."
    if ! grep -q "/usr/local/cuda/bin" ~/.bashrc 2>/dev/null; then
        echo 'export PATH=/usr/local/cuda/bin:/usr/bin:$PATH' >> ~/.bashrc
        echo "  ✓ Added CUDA to ~/.bashrc"
    else
        echo "  ✓ CUDA already in ~/.bashrc"
    fi
else
    echo "[4/10] Skipping CUDA/GPU driver installation (USE_GPU=false, using MooncakeConnector)"
fi

# ============================================
# 5. GPUDirect RDMA Kernel Module (nvidia-peermem)
# ============================================

# ============================================
# 5. GPUDirect RDMA Kernel Module (nvidia-peermem)  [GPU path only]
# ============================================

if [ "${USE_GPU:-true}" = "true" ]; then
    echo "[5/10] Installing GPUDirect RDMA kernel module (USE_GPU=true)..."

    # Check if nvidia-peermem is already loaded
    if lsmod | grep -q nvidia_peermem; then
        echo "  nvidia-peermem module already loaded"
    else
        # Try to load from DKMS if available
        if [ -d /usr/src/nvidia-peermem-* ]; then
            echo "  Found nvidia-peermem DKMS source, building..."
            PEERMEM_VERSION=$(ls -d /usr/src/nvidia-peermem-* | head -1 | sed 's/.*nvidia-peermem-//')
            sudo dkms install nvidia-peermem/$PEERMEM_VERSION || true
            sudo modprobe nvidia-peermem || echo "  Failed to load nvidia-peermem (may need reboot)"
        else
            echo "  Installing nvidia-peermem from source..."

            # Clone and build nvidia-peermem
            cd /tmp
            git clone https://github.com/Mellanox/nv_peer_memory.git || true
            cd nv_peer_memory

            # Build and install
            if ./build_module.sh; then
                cd /tmp
                if tar xzf /tmp/nvidia-peer-memory_*.tar.gz 2>/dev/null; then
                    cd nvidia-peer-memory-* 2>/dev/null || true
                    if [ -f ./install.sh ]; then
                        sudo ./install.sh
                    else
                        echo "  WARNING: install.sh not found, attempting manual DKMS install..."
                        # Try DKMS approach instead
                        sudo dpkg-buildpackage -us -uc 2>/dev/null || true
                    fi
                else
                    echo "  WARNING: nvidia-peer-memory tarball not found, skipping..."
                fi
            else
                echo "  WARNING: build_module.sh failed, skipping nvidia-peermem..."
            fi

            # Load module
            sudo modprobe nvidia-peermem || echo "  Failed to load nvidia-peermem"

            # Enable on boot
            echo "nvidia-peermem" | sudo tee /etc/modules-load.d/nvidia-peermem.conf
        fi
    fi

    # Verify GPUDirect is available
    if lsmod | grep -q nvidia_peermem; then
        echo "  ✓ GPUDirect RDMA module loaded successfully"
    else
        echo "  ⚠ GPUDirect RDMA module not loaded (may need reboot)"
    fi
else
    echo "[5/10] Skipping nvidia-peermem installation (USE_GPU=false, using MooncakeConnector)"
fi

# ============================================
# 6. Python and vLLM Dependencies
# ============================================

echo "[6/10] Installing Python dependencies..."

# Install Python and virtualenv tools
sudo apt-get install -y \
    python3 \
    python3-dev \
    python3-pip \
    python3-venv \
    python3-setuptools

# Determine project directory (default to ~/rdma-kv-cache)
PROJECT_DIR="${HOME}/rdma-kv-cache"
VENV_DIR="${PROJECT_DIR}/venv"

# Create virtual environment in project directory
if [ ! -d "$VENV_DIR" ]; then
    echo "  Creating virtual environment at ${VENV_DIR}..."
    python3 -m venv "$VENV_DIR"
else
    echo "  Virtual environment already exists at ${VENV_DIR}"
fi

# Activate virtual environment
echo "  Activating virtual environment..."
source "${VENV_DIR}/bin/activate"

# Upgrade pip in venv
echo "  Upgrading pip..."
pip install --upgrade pip setuptools wheel

# Install vLLM (will install PyTorch and other dependencies automatically)
echo "  Installing vLLM..."
pip install vllm

echo "  ✓ vLLM installed in virtual environment: ${VENV_DIR}"
echo "  To use: source ${VENV_DIR}/bin/activate"

# ============================================
# 7. Additional Libraries (libcurl, nlohmann-json)
# ============================================

echo "[7/10] Installing additional libraries..."

sudo apt-get install -y \
    libcurl4-openssl-dev \
    nlohmann-json3-dev

# ============================================
# 8. RDMA System Configuration
# ============================================

echo "[8/10] Configuring system for RDMA..."

# Increase locked memory limit (required for GPU memory registration)
if ! grep -q "memlock" /etc/security/limits.conf 2>/dev/null; then
    echo "  Setting unlimited locked memory..."
    echo "*  soft  memlock  unlimited" | sudo tee -a /etc/security/limits.conf
    echo "*  hard  memlock  unlimited" | sudo tee -a /etc/security/limits.conf
else
    echo "  Locked memory already configured"
fi

# Enable huge pages for better performance
echo "  Configuring huge pages (2048 pages = 4GB)..."
echo 2048 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null || true
if ! grep -q "vm.nr_hugepages" /etc/sysctl.conf 2>/dev/null; then
    echo "vm.nr_hugepages = 2048" | sudo tee -a /etc/sysctl.conf
fi

# Set max locked memory in systemd (for modern systems)
sudo mkdir -p /etc/systemd/system/user@.service.d
cat <<EOF | sudo tee /etc/systemd/system/user@.service.d/override.conf
[Service]
LimitMEMLOCK=infinity
EOF

# PCIe relaxed ordering (improves GPUDirect performance)
if [ -d /sys/module/mlx5_core/parameters ]; then
    echo "  Enabling PCIe relaxed ordering for Mellanox..."
    echo 1 | sudo tee /sys/module/mlx5_core/parameters/relaxed_ordering_write > /dev/null || true
fi

# ============================================
# 9. Network and IB Configuration
# ============================================

echo "[9/10] Configuring InfiniBand..."

# Start OpenSM subnet manager (if not already running)
if ! systemctl is-active --quiet opensm; then
    sudo systemctl enable opensm || true
    sudo systemctl start opensm || true
    echo "  Started OpenSM subnet manager"
else
    echo "  OpenSM already running"
fi

# Bring up IB interface (if present)
if [ -d "/sys/class/infiniband" ]; then
    for iface in $(ls /sys/class/infiniband/*/device/net/ 2>/dev/null || true); do
        if [ -n "$iface" ]; then
            echo "  Bringing up InfiniBand interface: $iface"
            sudo ip link set $iface up || true
        fi
    done
fi

# ============================================
# 10. Verification
# ============================================

echo "[10/10] Verifying installation..."
echo ""
echo "=========================================="
echo "System Information:"
echo "=========================================="

echo -n "  gcc: "
gcc --version | head -1

echo -n "  cmake: "
cmake --version | head -1

echo -n "  Python: "
python3 --version

echo -n "  PyTorch: "
python3 -c "import torch; print(torch.__version__)" 2>/dev/null || echo "NOT INSTALLED"

echo -n "  CUDA available in PyTorch: "
python3 -c "import torch; print('YES' if torch.cuda.is_available() else 'NO')" 2>/dev/null || echo "UNKNOWN"

echo ""
echo "=========================================="
echo "RDMA Configuration:"
echo "=========================================="

echo "  InfiniBand devices:"
ibv_devices 2>/dev/null || echo "    No IB devices found"

echo ""
echo "  IB device status:"
ibstat 2>/dev/null | grep -E "CA |State:|Rate:" || echo "    ibstat not available"

echo ""
echo "  Locked memory limits:"
ulimit -l

echo ""
echo "  Huge pages:"
cat /proc/meminfo | grep -E "HugePages_Total|HugePages_Free|Hugepagesize"

echo ""
echo "=========================================="
if [ "${USE_GPU:-true}" = "true" ]; then
    echo "NVIDIA/CUDA (USE_GPU=true):"
    echo "=========================================="

    if command -v nvidia-smi &> /dev/null; then
        # Try simple command first
        if nvidia-smi -L &> /dev/null; then
            echo "  GPU devices:"
            nvidia-smi -L
        else
            echo "  ⚠ nvidia-smi found but driver not loaded (reboot required)"
        fi
    else
        echo "  ⚠ NVIDIA driver not loaded (reboot required)"
    fi

    echo ""
    echo "  CUDA Toolkit (nvcc):"
    if command -v nvcc &> /dev/null; then
        echo "    ✓ nvcc installed:"
        nvcc --version | grep "release" | sed 's/^/    /'
    else
        echo "    ✗ nvcc NOT FOUND (installation may have failed)"
    fi

    echo ""
    echo "  GPUDirect RDMA status:"
    if lsmod | grep -q nvidia_peermem; then
        echo "    ✓ nvidia-peermem module LOADED"
    else
        echo "    ✗ nvidia-peermem module NOT LOADED"
    fi
else
    echo "MooncakeConnector (USE_GPU=false):"
    echo "=========================================="
    echo "  GPU/CUDA: skipped"
    echo ""
    echo "  mooncake-transfer-engine:"
    pip show mooncake-transfer-engine > /dev/null 2>&1 \
        && echo "    ✓ mooncake-transfer-engine INSTALLED" \
        || echo "    ✗ mooncake-transfer-engine NOT FOUND (check pip install)"
    echo ""
    echo "  Mooncake config template: configs/mooncake.json"
    echo "  Edit <node_ip> and <orchestrator_ip> before starting nodes"
fi

echo ""
echo "=========================================="
echo "Installation Complete!"
echo "=========================================="
echo ""
echo "IMPORTANT NEXT STEPS:"
echo "  1. LOGOUT and LOGIN again (or reboot) for limits to take effect"
if [ "${USE_GPU:-true}" = "true" ]; then
    echo "  2. If CUDA was just installed, REBOOT the system"
    echo "  3. Verify GPUDirect: lsmod | grep nvidia_peermem"
    echo "  4. Test RDMA: ibv_devinfo"
    echo "  5. Test GPU: nvidia-smi"
    echo ""
    echo "To test GPUDirect RDMA bandwidth:"
    echo "  ib_write_bw -d mlx5_0 -a --use_cuda=0"
else
    echo "  2. Edit configs/mooncake.json with your node IPs"
    echo "  3. Test RDMA: ibv_devinfo"
    echo "  4. Set USE_GPU=false in .env before running smart_run.sh"
fi
echo ""
