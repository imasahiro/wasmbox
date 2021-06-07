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
#include "input-stream.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

wasmbox_input_stream_t *wasmbox_input_stream_open(wasmbox_input_stream_t *ins, const char *file_name) {
    assert(ins != NULL);

    FILE *fp = fopen(file_name, "r");
    assert(fp != NULL);
    fseek(fp, 0, SEEK_END);
    ins->length = (size_t) ftell(fp);
    fseek(fp, 0, SEEK_SET);
    ins->index = 0;

    ins->data = (wasm_u8_t *) malloc(ins->length);
    size_t readed = fread(ins->data, 1, ins->length, fp);
    assert(ins->length == readed);
    fclose(fp);

    return ins;
}

int wasmbox_input_stream_is_end_of_stream(wasmbox_input_stream_t *ins) {
    return ins->index >= ins->length;
}

wasm_u8_t wasmbox_input_stream_peek_u8(wasmbox_input_stream_t *ins) {
    if (wasmbox_input_stream_is_end_of_stream(ins)) {
        return -1;
    }
    return ins->data[ins->index];
}

wasm_u8_t wasmbox_input_stream_read_u8(wasmbox_input_stream_t *ins) {
    if (wasmbox_input_stream_is_end_of_stream(ins)) {
        return -1;
    }
    return ins->data[ins->index++];
}

wasm_u32_t wasmbox_input_stream_read_u32(wasmbox_input_stream_t *ins) {
    if (ins->index + 4 > ins->length) {
        return -1;
    }
    wasm_u32_t v = *(wasm_u32_t *) (ins->data + ins->index);
    ins->index += 4;
    return v;
}

void wasmbox_input_stream_close(wasmbox_input_stream_t *ins) {
    free(ins->data);
}

#ifdef __cplusplus
}
#endif
