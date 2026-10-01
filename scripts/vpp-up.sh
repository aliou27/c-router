#!/usr/bin/env bash
# M17: same lab as M5, but VPP is the router instead of the C program.
#
#   pc-a 10.0.1.10 --- vpp-a [ VPP ] vpp-b --- pc-b 10.0.2.10
#                      10.0.1.1     10.0.2.1
#
# VPP runs as a service in the VM's main namespace, so the router side of
# each cable (vpp-a, vpp-b) stays there. No "router" namespace this time.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "Run with sudo: sudo $0"
  exit 1
fi
if ! vppctl show version >/dev/null 2>&1; then
  echo "VPP is not running. Try: sudo systemctl start vpp"
  exit 1
fi

# Start clean if the lab already exists
"$(dirname "$0")/vpp-down.sh" >/dev/null 2>&1 || true

# 1. Machines (only the two PCs)
ip netns add pc-a
ip netns add pc-b
for ns in pc-a pc-b; do
  ip netns exec "$ns" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
  ip netns exec "$ns" sysctl -qw net.ipv6.conf.default.disable_ipv6=1
done

# 2. Cables: one end in the PC, the other end stays here for VPP
ip link add vpp-a type veth peer name eth0 netns pc-a
ip link add vpp-b type veth peer name eth0 netns pc-b
sysctl -qw net.ipv6.conf.vpp-a.disable_ipv6=1
sysctl -qw net.ipv6.conf.vpp-b.disable_ipv6=1

# 3. PCs: same MACs, IPs and gateways as the C router lab
ip -n pc-a link set eth0 address 02:00:00:00:01:0a
ip -n pc-b link set eth0 address 02:00:00:00:02:0a
ip -n pc-a addr add 10.0.1.10/24 dev eth0
ip -n pc-b addr add 10.0.2.10/24 dev eth0
for ns in pc-a pc-b; do
  ip -n "$ns" link set lo up
  ip -n "$ns" link set eth0 up
done
ip link set vpp-a up
ip link set vpp-b up
ip -n pc-a route add default via 10.0.1.1
ip -n pc-b route add default via 10.0.2.1

# 3b. Same checksum offload fix as M9 (VPP also reads raw frames)
if command -v ethtool >/dev/null; then
  ip netns exec pc-a ethtool -K eth0 tx off >/dev/null
  ip netns exec pc-b ethtool -K eth0 tx off >/dev/null
fi

# 4. VPP: plug into each cable (AF_PACKET, like our C router), give it an IP, turn it on
vppctl create host-interface name vpp-a hw-addr 02:00:00:00:01:01 >/dev/null
vppctl create host-interface name vpp-b hw-addr 02:00:00:00:02:01 >/dev/null
vppctl set interface ip address host-vpp-a 10.0.1.1/24
vppctl set interface ip address host-vpp-b 10.0.2.1/24
vppctl set interface state host-vpp-a up
vppctl set interface state host-vpp-b up

echo "VPP lab ready. Test: sudo ip netns exec pc-a ping -c 3 10.0.2.10"
