#!/bin/bash
# usage: build.sh NAME file.c...   -> /tmp/NAME (host build of kernel/net code with ASan+UBSan)
here=$(cd "$(dirname "$0")" && pwd); name=$1; shift
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-function \
  -I "$here/shim" -I "$here/../fs-test/shim" -I "$here/../../kernel/net" "$@" -o /tmp/$name
