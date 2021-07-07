/*
 * Copyright (C) 2020 Masahiro Ide
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "leb128.h"

#ifdef __cplusplus
extern "C" {
#endif

// https://en.wikipedia.org/wiki/LEB128#Decode_unsigned_integer
wasm_u64_t wasmbox_parse_unsigned_leb128(const wasm_u8_t *p, wasm_u32_t *idx,
                                         wasm_u32_t len) {
  wasm_u64_t result = 0;
  const wasm_u64_t mask = 1 << (sizeof(wasm_u8_t) * 8 - 1);
  unsigned shift = 0;
  wasm_u64_t v;
  while (*idx <= len) {
    v = *p++;
    *idx += 1;
    result |= (v & ~mask) << shift;
    if ((v & mask) == 0) {
      break;
    }
    shift += 7;
  }
  return result;
}

// https://en.wikipedia.org/wiki/LEB128#Decode_signed_integer
wasm_s64_t wasmbox_parse_signed_leb128(const wasm_u8_t *p, wasm_u32_t *idx,
                                       wasm_u32_t len) {
  wasm_s64_t result = 0;
  const wasm_s64_t mask = 1ULL << (sizeof(wasm_u8_t) * 8 - 1);
  unsigned shift = 0;
  wasm_s64_t v;
  do {
    v = *p++;
    *idx += 1;
    result |= (v & ~mask) << shift;
    shift += 7;
  } while ((v & mask) != 0 && *idx < len);

  if ((shift < sizeof(wasm_s64_t) * 8) && (v & 0x40) != 0) {
    result |= (~0 << shift);
  }
  return result;
}

#ifdef __cplusplus
}
#endif
