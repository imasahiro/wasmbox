/*
 * Copyright (C) 2021 Masahiro Ide
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

#ifndef WASMBOX_H
#define WASMBOX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  wasm_u8_t;
typedef int8_t   wasm_s8_t;
typedef uint16_t wasm_u16_t;
typedef int16_t  wasm_s16_t;
typedef uint32_t wasm_u32_t;
typedef int32_t  wasm_s32_t;
typedef uint64_t wasm_u64_t;
typedef int64_t  wasm_s64_t;
typedef float    wasm_f32_t;
typedef double   wasm_f64_t;

typedef union wasmbox_value_t {
    wasm_u8_t u8;
    wasm_s8_t s8;
    wasm_u16_t u16;
    wasm_s16_t s16;
    wasm_u32_t u32;
    wasm_s32_t s32;
    wasm_u64_t u64;
    wasm_s64_t s64;
    wasm_f32_t f32;
    wasm_f64_t f64;
} wasmbox_value_t;

typedef struct wasmbox_module_t {
} wasmbox_module_t;

int wasmbox_load_module(wasmbox_module_t *mod, const char *file_name,
                        wasm_u16_t file_name_len);

int wasmbox_eval_module(wasmbox_module_t *mod, wasmbox_value_t result[],
                        wasm_u16_t result_stack_size);
#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
