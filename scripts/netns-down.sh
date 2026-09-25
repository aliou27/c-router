#!/usr/bin/env bash
set -uo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "Run with sudo: sudo $0"
  exit 1
fi

for ns in pc-a router pc-b; do
  ip netns del "$ns" 2>/dev/null || true
done
echo "Lab removed."