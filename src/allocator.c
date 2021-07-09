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
#include "allocator.h"

static wasm_s64_t allocated;
static wasm_s64_t freed;

void *wasmbox_malloc(wasm_u32_t size) {
  wasm_s32_t *mem = (wasm_s32_t *) malloc(size + sizeof(wasm_s32_t));
  bzero(&mem[1], size);
  mem[0] = (wasm_s32_t) size;
  allocated += size;
  return &mem[1];
}

void *wasmbox_realloc(void *ptr, wasm_u32_t size) {
  wasm_s32_t *mem = &((wasm_s32_t *) ptr)[-1];
  wasm_s32_t old = mem[0];
  mem = (wasm_s32_t *) realloc(mem, size + sizeof(wasm_s32_t));
  mem[0] = (wasm_s32_t) size;
  allocated += size - old;
  return &mem[1];
}

void wasmbox_free(void *ptr) {
  wasm_s32_t *mem = &((wasm_s32_t *) ptr)[-1];
  freed += mem[0];
  free(mem);
}

void wasmbox_allocator_report_statics() {
  fprintf(stdout, "allocated: %lld byte (%lld KB)\n", allocated, allocated / 1024);
  fprintf(stdout, "freed:     %lld byte (%lld KB)\n", freed, freed / 1024);
  if (allocated != freed) {
    fprintf(stderr, "allocated != freed");
    exit(-1);
  }
}
