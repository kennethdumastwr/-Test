#!/bin/sh
# Host-side tests for the pure C++ core (no Geode needed). Run from the project root.
set -e
cd "$(dirname "$0")/.."
g++ -std=c++20 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined tests/test_core.cpp -o /tmp/fw_test_core
/tmp/fw_test_core
