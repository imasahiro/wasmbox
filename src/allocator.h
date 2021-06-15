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

#include <stdlib.h>
#include <string.h>
#include "wasmbox/wasmbox.h"

#ifndef WASMBOX_ALLOCATOR_H
#define WASMBOX_ALLOCATOR_H

static inline void *wasmbox_malloc(wasm_u32_t size) {
    void *mem = malloc(size);
    bzero(mem, size);
    return mem;
}

static inline void wasmbox_free(void *ptr) {
    free(ptr);
}

#endif //WASMBOX_ALLOCATOR_H
