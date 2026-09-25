#!/usr/bin/env bash
# Builds the lab: pc-a <-> router <-> pc-b
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "Run with sudo: sudo $0"
  exit 1
fi

# Start clean if the lab already exists
for ns in pc-a router pc-b; do
  ip netns del "$ns" 2>/dev/null || true
done

# 1. Machines
ip netns add pc-a
ip netns add router
ip netns add pc-b

# 2. Cables
ip link add eth0 netns pc-a type veth peer name eth0 netns router
ip link add eth0 netns pc-b type veth peer name eth1 netns router

# 3. Addresses
ip -n pc-a   addr add 10.0.1.10/24 dev eth0
ip -n router addr add 10.0.1.1/24  dev eth0
ip -n router addr add 10.0.2.1/24  dev eth1
ip -n pc-b   addr add 10.0.2.10/24 dev eth0

# 4. Interfaces up
for ns in pc-a router pc-b; do
  ip -n "$ns" link set lo up
done
ip -n pc-a   link set eth0 up
ip -n router link set eth0 up
ip -n router link set eth1 up
ip -n pc-b   link set eth0 up

# 5. Default gateways
ip -n pc-a route add default via 10.0.1.1
ip -n pc-b route add default via 10.0.2.1

# 6. Linux does the routing for now (M5: set this to 0)
ip netns exec router sysctl -qw net.ipv4.ip_forward=1

echo "Lab ready. Test: sudo ip netns exec pc-a ping -c 3 10.0.2.10"