#!/bin/bash

# ============================================
# Install nv_peer_memory for GPUDirect RDMA
# ============================================
# This script installs the proper nv_peer_memory module
# that works with inbox RDMA drivers (not MLNX_OFED)

set -e

echo "=========================================="
echo "Installing nv_peer_memory for GPUDirect RDMA"
echo "=========================================="

# Check if already installed
if dpkg -l | grep -q nvidia-peer-memory-dkms; then
    echo "nvidia-peer-memory-dkms already installed"
    echo "To reinstall, run: sudo apt-get remove nvidia-peer-memory-dkms"
    exit 0
fi

# Install build dependencies
echo "Installing build dependencies..."
sudo apt-get update -qq
sudo apt-get install -y debhelper autotools-dev dkms git

# Clone nv_peer_memory
echo "Cloning nv_peer_memory..."
cd /tmp
rm -rf nv_peer_memory
git clone https://github.com/Mellanox/nv_peer_memory.git
cd nv_peer_memory

# Build the module
echo "Building nv_peer_memory module..."
./build_module.sh

# Extract and build debian package
echo "Building debian package..."
cd /tmp
tar xzf /tmp/nvidia-peer-memory_*.tar.gz
cd nvidia-peer-memory-*
dpkg-buildpackage -us -uc

# Install the package
echo "Installing nvidia-peer-memory-dkms package..."
cd /tmp
sudo dpkg -i nvidia-peer-memory-dkms_*.deb

# Unload old nvidia-peermem if loaded
echo "Unloading old nvidia-peermem..."
sudo modprobe -r nvidia_peermem 2>/dev/null || true

# Load the new module
echo "Loading nvidia_peer_mem..."
sudo modprobe nvidia_peer_mem

# Verify
echo ""
echo "=========================================="
echo "Verifying installation..."
echo "=========================================="

if lsmod | grep -q nvidia_peer_mem; then
    echo "✓ nvidia_peer_mem module loaded"
else
    echo "✗ nvidia_peer_mem module NOT loaded"
    exit 1
fi

if ibv_devinfo -v 2>/dev/null | grep -qi peer; then
    echo "✓ Peer memory support detected in IB stack"
else
    echo "⚠ Peer memory not detected - may need reboot"
fi

echo ""
echo "=========================================="
echo "Installation Complete!"
echo "=========================================="
echo ""
echo "IMPORTANT: Reboot the system for changes to fully take effect"
echo "  sudo reboot"
echo ""
