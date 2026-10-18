#!/bin/sh
# Copy the vkcore test to the Steam Frame (~/ssbu-port/vkcore), build with gcc and clang, run both. Usage: tests/port/vkcore/run_frame.sh [host]
set -e
HOST=${1:-steamos@frame}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"
tar -cf - runtime/plat.h runtime/fiber.h runtime/vmem.h runtime/nvn_vk_posix.h tests/port/vkcore/test_vkcore.c |
    ssh -o BatchMode=yes "$HOST" 'mkdir -p ~/ssbu-port/vkcore && cd ~/ssbu-port/vkcore && tar -xf -'
ssh -o BatchMode=yes "$HOST" 'cd ~/ssbu-port/vkcore && set -e
  VKINC=""; [ -f /usr/include/vulkan/vulkan.h ] || VKINC="-I$HOME/ssbu-port/deps/vulkan/include"
  for cc in gcc clang; do
    echo "######## $cc"; $cc -O1 -g -D_GNU_SOURCE -Wall -Wno-unused-function -Iruntime $VKINC tests/port/vkcore/test_vkcore.c -o test_vkcore_$cc -ldl -lpthread
    rm -rf /tmp/vkcore_test; ./test_vkcore_$cc /tmp/vkcore_test; rm -rf /tmp/vkcore_test
  done'
