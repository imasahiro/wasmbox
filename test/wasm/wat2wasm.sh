#!/bin/bash

OBJDUMP=../wabt/bin/wasm-objdump
STRIP=../wabt/bin/wasm-strip
WAT2WASM=../wabt/bin/wat2wasm

$WAT2WASM $1 -o $1.wasm
$STRIP $1.wasm
$OBJDUMP -d $1.wasm
