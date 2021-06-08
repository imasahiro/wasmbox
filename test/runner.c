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
#include <stdio.h>
#include <string.h>

int main(int argc, char const *argv[]) {
    if (argc <= 1) {
        fprintf(stdout, "usage: %s a.wasm\n", argv[0]);
        return 0;
    }
    wasmbox_module_t mod = {};
    wasmbox_value_t stack[10] = {};
    if (wasmbox_load_module(&mod, argv[1], strlen(argv[1])) != 0) {
        fprintf(stdout, "Failed to load a module(%s).\n", argv[1]);
        return -1;
    }
    if (wasmbox_eval_module(&mod, stack, 10) < 0) {
        fprintf(stdout, "Failed to evaluate a module(%s).\n", argv[1]);
        return -1;
    }
    wasmbox_module_dispose(&mod);
    fprintf(stdout, "stack[0]=%llu\n", stack[0].u64);
    return 0;
}
