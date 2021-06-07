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

#include "wasmbox/wasmbox.h"

#include <assert.h>
#include <stdio.h>

#ifndef WASMBOX_INPUT_STREAM_H
#define WASMBOX_INPUT_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wasmbox_input_stream_t {
    wasm_u32_t index;
    wasm_u32_t length;
    wasm_u8_t *data;
} wasmbox_input_stream_t;

wasmbox_input_stream_t *wasmbox_input_stream_open(wasmbox_input_stream_t *ins, const char *file_name);

int wasmbox_input_stream_is_end_of_stream(wasmbox_input_stream_t *ins);

wasm_u8_t wasmbox_input_stream_peek_u8(wasmbox_input_stream_t *ins);

wasm_u8_t wasmbox_input_stream_read_u8(wasmbox_input_stream_t *ins);

wasm_u32_t wasmbox_input_stream_read_u32(wasmbox_input_stream_t *ins);

void wasmbox_input_stream_close(wasmbox_input_stream_t *ins);

#ifdef __cplusplus
}
#endif
#endif /* end of include guard */
