#!/usr/bin/env bash
# M17: removes the VPP lab
set -uo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "Run with sudo: sudo $0"
  exit 1
fi

vppctl delete host-interface name vpp-a 2>/dev/null || true
vppctl delete host-interface name vpp-b 2>/dev/null || true
for ns in pc-a pc-b; do
  ip netns del "$ns" 2>/dev/null || true
done
ip link del vpp-a 2>/dev/null || true
ip link del vpp-b 2>/dev/null || true
echo "VPP lab removed."
