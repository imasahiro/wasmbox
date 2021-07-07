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

#include "wasmbox/wasmbox.h"

#ifndef WASMBOX_LEB128_H
#  define WASMBOX_LEB128_H

#  ifdef __cplusplus
extern "C" {
#  endif

/**
 * Parses a unsigned LEB128 to {@link wasm_u64_t}.
 */
wasm_u64_t wasmbox_parse_unsigned_leb128(const wasm_u8_t *p, wasm_u32_t *idx,
                                         wasm_u32_t len);

/**
 * Parses a signed LEB128 to {@link wasm_s64_t}.
 */
wasm_s64_t wasmbox_parse_signed_leb128(const wasm_u8_t *p, wasm_u32_t *idx,
                                       wasm_u32_t len);

#  ifdef __cplusplus
}
#  endif
#endif /* end of include guard */
