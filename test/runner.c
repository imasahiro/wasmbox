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
#include <stdlib.h>

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
    FILE *fp = fopen(argv[2], "r");
    wasmbox_value_t expected;
    int ch = fgetc(fp);
    const char buf[128];
    fread((void *) buf, 128, 1, fp);

    int equal;
    switch (ch) {
        case 'i': // i32
            expected.s32 = atoi(buf);
            equal = expected.s32 == stack[0].s32;
            fprintf(stdout, "expected:(%d) %s actual(%d)\n", expected.s32, equal? "==" : "!=", stack[0].s32);
            break;
        case 'I': // i64
            expected.s64 = atol(buf);
            equal = expected.s64 == stack[0].s64;
            fprintf(stdout, "expected:(%lld) %s actual(%lld)\n", expected.s64, equal? "==" : "!=", stack[0].s64);
            break;
        case 'f': // f32
            expected.f32 = atof(buf);
            equal = expected.f32 == stack[0].f32;
            fprintf(stdout, "expected:(%f) %s actual(%f)\n", expected.f32, equal? "==" : "!=", stack[0].f32);
            break;
        case 'F': // f64
            expected.f64 = atof(buf);
            equal = expected.f64 == stack[0].f64;
            fprintf(stdout, "expected:(%g) %s actual(%g)\n", expected.f64, equal? "==" : "!=", stack[0].f64);
            break;
        default:
            fprintf(stderr, "unexpected: %c\n", ch);
            return -1;
    }
    return !equal;
}
