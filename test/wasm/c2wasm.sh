#!/bin/bash

CLANG=../llvm-project/build/bin/clang
WASM_LD=../llvm-project/build/bin/wasm-ld
OBJDUMP=../wabt/bin/wasm-objdump
STRIP=../wabt/bin/wasm-strip
#OPTS="-fno-optimize-sibling-calls "
OPTS=""

$CLANG -Oz $OPTS -target wasm32-wasm $1 -c -o $1.o
$WASM_LD -o $1.wasm $1.o
$STRIP $1.wasm
$OBJDUMP -d $1.wasm
