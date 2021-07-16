#!/bin/bash

CLANG=clang
WASM_LD=wasm-ld
OBJDUMP=../wabt/bin/wasm-objdump
STRIP=../wabt/bin/wasm-strip
OPTS=""
OPTS="-fno-optimize-sibling-calls "

$CLANG -Oz $OPTS -target wasm32-wasm $1 -c -o $1.o
$WASM_LD -o $1.wasm $1.o
$STRIP $1.wasm
$OBJDUMP -d $1.wasm
