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

#ifndef WASMBOX_INTERPRETER_H
#define WASMBOX_INTERPRETER_H

#include "opcodes.h"
#include "wasmbox/wasmbox.h"

#include <stdlib.h> // exit

#ifdef __cplusplus
extern "C" {
#endif

void wasmbox_dump_function(wasmbox_code_t *code_start, wasmbox_code_t *code_end,
                           const char *indent);
void wasmbox_eval_function(wasmbox_module_t *mod, wasmbox_code_t *code,
                           wasmbox_value_t *stack);
void wasmbox_virtual_machine_init(wasmbox_module_t *mod);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
