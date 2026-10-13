#!/usr/bin/env sh
# Build nxport on Linux (or any POSIX system with /proc).
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O2 -Wall -Wextra -o nxport nxport.cpp
echo "[build] Built $(pwd)/nxport"
