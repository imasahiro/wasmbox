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

#include "allocator.h"
#include "wasmbox/wasmbox.h"

#include <float.h>
#include <opcodes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int compare_float(float x, float y) {
  float diff = x > y ? (x - y) : (y - x);
  return diff < FLT_EPSILON;
}

static int compare_double(double x, double y) {
  double diff = x > y ? (x - y) : (y - x);
  return diff < DBL_EPSILON;
}

static int check_result(int result_length, wasmbox_value_t stack[],
                        wasmbox_value_t expected[],
                        wasmbox_value_type_t expected_types[]) {
  int equal = 1;
  for (int i = 0; i < result_length; ++i) {
    switch (expected_types[i]) {
      case WASM_TYPE_I32:
        equal &= expected[i].s32 == stack[i].s32;
        fprintf(stdout, "expected(%d):(%d) %s actual(%d)\n", i, expected[i].s32,
                equal ? "==" : "!=", stack[i].s32);
        break;
      case WASM_TYPE_I64:
        equal &= expected[i].s64 == stack[i].s64;
        fprintf(stdout, "expected(%d):(%lld) %s actual(%lld)\n", i,
                expected[i].s64, equal ? "==" : "!=", stack[i].s64);
        break;
      case WASM_TYPE_F32:
        equal &= compare_float(expected[i].f32, stack[i].f32);
        fprintf(stdout, "expected(%d):(%f) %s actual(%f)\n", i, expected[i].f32,
                equal ? "==" : "!=", stack[i].f32);
        break;
      case WASM_TYPE_F64:
        equal &= compare_double(expected[i].f64, stack[0].f64);
        fprintf(stdout, "expected(%d):(%g) %s actual(%g)\n", i, expected[i].f64,
                equal ? "==" : "!=", stack[0].f64);
        break;
      default:
        fprintf(stderr, "unexpected type: %d\n", expected_types[i]);
        equal = 0;
        break;
    }
  }
  return !equal;
}

int main(int argc, char const *argv[]) {
  if (argc <= 2) {
    fprintf(stdout, "usage: %s a.wasm a.wasm.result\n", argv[0]);
    return 0;
  }
  FILE *fp = fopen(argv[2], "r");
  wasmbox_module_t mod = {};
  int stack_index = 0;
  wasmbox_value_t stack[1024] = {};
  int expected_index = 0;
  wasmbox_value_t expected[10] = {};
  wasmbox_value_type_t expected_type[10] = {};

  for (int i = 0; i < 10; ++i) {
    int io = fgetc(fp);
    if (io == EOF) {
      break;
    }
    int j = 0;
    char buf[1024] = {};
    int value_type = fgetc(fp);
    int ch = fgetc(fp);
    while (ch != EOF && ch != '\n') {
      buf[j++] = (char) ch;
      ch = fgetc(fp);
    }
    wasmbox_value_t v;
    wasmbox_value_type_t type;
    switch (value_type) {
      case 'i': // i32
        v.s32 = atoi(buf);
        type = WASM_TYPE_I32;
        break;
      case 'I': // i64
        v.s64 = atol(buf);
        type = WASM_TYPE_I64;
        break;
      case 'f': // f32
        v.f32 = atof(buf);
        type = WASM_TYPE_F32;
        break;
      case 'F': // f64
        v.f64 = atof(buf);
        type = WASM_TYPE_F64;
        break;
      default:
        fprintf(stderr, "unexpected char: '%c'(%d)\n", ch, ch);
        return -1;
    }
    if (io == '>') {
      WASMBOX_ADD_ARGUMENT(stack, stack_index++, u64, v.u64);
    } else if (io == '<') {
      expected[expected_index] = v;
      expected_type[expected_index++] = type;
    } else {
      fprintf(stderr, "invalid format\n");
      break;
    }
  }

  if (wasmbox_load_module(&mod, argv[1], strlen(argv[1])) != 0) {
    fprintf(stdout, "Failed to load a module(%s).\n", argv[1]);
    return -1;
  }
  if (wasmbox_eval_module(&mod, stack) < 0) {
    fprintf(stdout, "Failed to evaluate a module(%s).\n", argv[1]);
    return -1;
  }
  wasmbox_module_dispose(&mod);
  wasmbox_allocator_report_statics();
  return check_result(expected_index, stack, expected, expected_type);
}
