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

#include <assert.h>

int main(int argc, char const *argv[]) {
  const wasm_u8_t d1[] = {0xE5, 0x8E, 0x26};
  const wasm_u8_t d2[] = {0xc0, 0xbb, 0x78};
  const wasm_u8_t d3[] = {0xff, 0x01};
  const wasm_u8_t d4[] = {0xaf, 0xfd, 0xb6, 0xf5, 0x0d};
  wasm_u32_t idx = 0;
  wasm_u32_t len = 3;
  assert(wasmbox_parse_unsigned_leb128(d1, &idx, len) == 624485);
  idx = 0;
  assert(wasmbox_parse_signed_leb128(d1, &idx, len) == 624485);
  idx = 0;
  assert(wasmbox_parse_signed_leb128(d2, &idx, len) == -123456);
  idx = 0;
  assert(wasmbox_parse_signed_leb128(d3, &idx, 2) == 255);
  idx = 0;
  assert(wasmbox_parse_signed_leb128(d4, &idx, 5) == 0xdeadbeaf);
  idx = 0;
  assert(wasmbox_parse_unsigned_leb128(d4, &idx, 5) == 0xdeadbeaf);
  return 0;
}
