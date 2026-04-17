#!/bin/bash

# Install dependencies on Ubuntu (CloudLab) node
# This script is copied to and run on each CloudLab node

set -e

echo "========================================"
echo "Installing RHT Dependencies on Ubuntu"
echo "========================================"
echo ""

# Update package list
echo "[1/7] Updating package list..."
sudo apt-get update -qq

# Install build essentials
echo "[2/7] Installing build essentials..."
sudo apt-get install -y build-essential cmake git

# Install yaml-cpp (from package manager)
echo "[3/7] Installing yaml-cpp..."
sudo apt-get install -y libyaml-cpp-dev

# Install RDMA/InfiniBand libraries
echo "[4/7] Installing RDMA/InfiniBand libraries..."
sudo apt-get install -y libibverbs-dev librdmacm-dev ibverbs-utils rdma-core perftest

# Configure system for RDMA
echo "[5/7] Configuring system for RDMA..."
# Increase locked memory limit for RDMA (required for memory registration)
if ! grep -q "memlock" /etc/security/limits.conf 2>/dev/null; then
    echo "  Setting unlimited locked memory for all users..."
    echo "*  soft  memlock  unlimited" | sudo tee -a /etc/security/limits.conf
    echo "*  hard  memlock  unlimited" | sudo tee -a /etc/security/limits.conf
else
    echo "  Locked memory limits already configured"
fi

# Enable huge pages for better RDMA performance (optional but recommended)
echo "  Configuring huge pages..."
echo 128 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null || true
if ! grep -q "vm.nr_hugepages" /etc/sysctl.conf 2>/dev/null; then
    echo "vm.nr_hugepages = 128" | sudo tee -a /etc/sysctl.conf
fi

# Install other useful tools
echo "[6/7] Installing additional tools..."
sudo apt-get install -y net-tools htop

# Verify installations
echo "[7/7] Verifying installations..."
echo -n "  gcc: "
gcc --version | head -1
echo -n "  g++: "
g++ --version | head -1
echo -n "  cmake: "
cmake --version | head -1
echo -n "  yaml-cpp: "
pkg-config --modversion yaml-cpp 2>/dev/null || echo "installed (version check unavailable)"
echo -n "  libibverbs: "
if [ -f /usr/lib/x86_64-linux-gnu/libibverbs.so ] || [ -f /usr/lib/libibverbs.so ]; then
    echo "installed"
else
    echo "NOT FOUND (may need manual installation)"
fi
echo -n "  RDMA devices: "
if command -v ibv_devices &> /dev/null; then
    DEVICE_COUNT=$(ibv_devices 2>/dev/null | grep -c "^[[:space:]]*mlx\|ib" || echo "0")
    if [ "$DEVICE_COUNT" -gt 0 ]; then
        echo "$DEVICE_COUNT device(s) found"
    else
        echo "NO DEVICES (will use software fallback)"
    fi
else
    echo "ibv_devices not found"
fi

echo ""
echo "========================================"
echo "Dependencies installed successfully!"
echo "========================================"
echo ""
echo "IMPORTANT: RDMA Configuration Notes:"
echo "  - Locked memory limit set to unlimited"
echo "  - Huge pages configured (128 pages)"
echo "  - You may need to LOGOUT and LOGIN again for limits to take effect"
echo "  - Check RDMA devices with: ibv_devices"
echo "  - Check huge pages with: cat /proc/meminfo | grep Huge"
echo ""
