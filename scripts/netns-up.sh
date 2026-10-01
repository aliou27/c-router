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

# 1b. Disable IPv6 until M15 to keep captures clean
for ns in pc-a router pc-b; do
  ip netns exec "$ns" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
  ip netns exec "$ns" sysctl -qw net.ipv6.conf.default.disable_ipv6=1
done

# 2. Cables
ip link add eth0 netns pc-a type veth peer name eth0 netns router
ip link add eth0 netns pc-b type veth peer name eth1 netns router

# 2b. Fixed MAC addresses (M5): the C router needs to know them in advance.
#     Pattern: 02:00:00:00:<network>:<host>
ip -n pc-a   link set eth0 address 02:00:00:00:01:0a   # pc-a       10.0.1.10
ip -n router link set eth0 address 02:00:00:00:01:01   # router eth0 10.0.1.1
ip -n router link set eth1 address 02:00:00:00:02:01   # router eth1 10.0.2.1
ip -n pc-b   link set eth0 address 02:00:00:00:02:0a   # pc-b       10.0.2.10

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

# 4b. Turn off TX checksum offload on the PCs (M9). Otherwise Linux leaves the
#     TCP/UDP checksum unfinished, and since the C router copies raw bytes,
#     the receiver would get a wrong checksum and drop the packet.
if command -v ethtool >/dev/null; then
  ip netns exec pc-a ethtool -K eth0 tx off >/dev/null
  ip netns exec pc-b ethtool -K eth0 tx off >/dev/null
fi

# 5. Default gateways
ip -n pc-a route add default via 10.0.1.1
ip -n pc-b route add default via 10.0.2.1

# 6. Who forwards packets in the router namespace?
#    "./netns-up.sh"      -> the Linux kernel forwards (M1 to M4)
#    "./netns-up.sh off"  -> kernel forwarding OFF, the C router must do it (M5)
if [[ "${1:-on}" == "off" ]]; then
  ip netns exec router sysctl -qw net.ipv4.ip_forward=0
  echo "Lab ready. Kernel forwarding is OFF: start the C router, then ping."
else
  ip netns exec router sysctl -qw net.ipv4.ip_forward=1
  echo "Lab ready. Kernel forwarding is ON. Test: sudo ip netns exec pc-a ping -c 3 10.0.2.10"
fi