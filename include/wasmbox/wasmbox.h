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
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t wasm_u8_t;
typedef int8_t wasm_s8_t;
typedef uint16_t wasm_u16_t;
typedef int16_t wasm_s16_t;
typedef uint32_t wasm_u32_t;
typedef int32_t wasm_s32_t;
typedef uint64_t wasm_u64_t;
typedef int64_t wasm_s64_t;
typedef float wasm_f32_t;
typedef double wasm_f64_t;

#define WASM_U8_MAX (UCHAR_MAX)
#define WASM_S8_MAX (SCHAR_MAX)
#define WASM_U16_MAX (USHRT_MAX)
#define WASM_S16_MAX (SHRT_MAX)
#define WASM_U32_MAX (UINT_MAX)
#define WASM_S32_MAX (INT_MAX)
#define WASM_U64_MAX (ULONG_MAX)
#define WASM_S64_MAX (LONG_MAX)

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

typedef enum wasmbox_value_type_t {
    WASM_TYPE_UNDEFINED = 0,
    WASM_TYPE_I32 = 1,
    WASM_TYPE_I64 = 2,
    WASM_TYPE_F32 = 3,
    WASM_TYPE_F64 = 4,
    WASM_TYPE_FUNCREF = 5,
    WASM_TYPE_EXTERNREF = 6
} wasmbox_value_type_t;

typedef struct wasmbox_name_t {
    wasm_u32_t len;
    wasm_u8_t value[0];
} wasmbox_name_t;

typedef struct wasmbox_limit_t {
    wasm_u32_t min;
    wasm_u32_t max;
} wasmbox_limit_t;

typedef struct wasmbox_type_t {
    wasm_u16_t return_size;
    wasm_u16_t argument_size;
    wasmbox_value_type_t args[0];
} wasmbox_type_t;

typedef struct wasmbox_code_t wasmbox_code_t;

typedef struct wasmbox_function_t {
    wasmbox_code_t *code;
    wasmbox_type_t *type;
    wasmbox_name_t *name;
    wasm_u16_t locals;
    wasm_u16_t code_size;
    wasm_u16_t code_capacity;
} wasmbox_function_t;

typedef struct wasmbox_mutable_function_t {
    wasmbox_function_t base;
    wasm_s16_t stack_top;
    wasm_s16_t stack_size;
    wasm_u16_t stack_capacity;
    wasm_s16_t *operand_stack;
} wasmbox_mutable_function_t;

union wasmbox_code_operands {
    wasmbox_value_t value;
    wasm_u32_t index;
    wasm_s32_t reg;
    struct registers {
        wasm_s32_t reg1;
        wasm_s32_t reg2;
    } r;
    wasmbox_function_t *func;
};

/**
 * WasmBox stack from design.
 * We share single stack for operand stack and value stack. When
 * we call a function which has A+1 arguments and R+1 returns, and
 * has L local values, like following, the stack will be used as following table.
 * <pre>{@code
 * function func(arg0, arg1, ..., argA) {
 *      var local0, local1, ..., localL;
 *      return (return0, return1, ..., returnR);
 * }
 * }</pre>
 * <pre>
 * +-------------+-------------+----------------+
 * +  Caller     + Callee      +                +
 * +-------------+-------------+----------------+
 * | stack-R     | stack-2-R   | returnR        |
 * |    ...      |    ...      |                |
 * | stack-2     | stack-4     | return1        |
 * | stack-1     | stack-3     | return0        |
 * | stack_top   | stack-2     | &prev_stack_top|
 * | stack+1     | stack-1     | next_pc        |
 * | stack+2     | stack_top   | arg0           |
 * | stack+3     | stack+1     | arg1           |
 * | stack+4     | stack+2     | arg2           |
 * |    ...      |    ...      |                |
 * | stack+A+2   | stack+A     | argA           |
 * | stack+A+3   | stack+A+1   | local0         |
 * |    ...      |    ...      |                |
 * | stack+A+L+2 | stack+A+L   | localL         |
 * | stack+A+L+3 | stack+A+L+1 | stack_top      |
 * +-------------+-------------+----------------+
 * </pre>
 */
typedef wasmbox_value_t wasmbox_stack_t;

#define WASMBOX_FUNCTION_CALL_OFFSET (2)

#define WASMBOX_ADD_ARGUMENT(STACK, INDEX, TYPE, V) do {            \
    (STACK)[WASMBOX_FUNCTION_CALL_OFFSET + 1 + (INDEX)].TYPE = (V); \
} while (0)

struct wasmbox_code_t {
    union header {
        wasm_u16_t opcode;
        void *label;
    } h;
    union wasmbox_code_operands op0;
    union wasmbox_code_operands op1;
    union wasmbox_code_operands op2;
};

#define WASMBOX_PAGE_SIZE (65536)
typedef struct wasmbox_memory_block_t {
    wasm_u8_t data[0/* WASMBOX_PAGE_SIZE * page_size */];
} wasmbox_memory_block_t;

typedef struct wasmbox_module_t {
    wasmbox_function_t **functions;
    wasm_u32_t function_size;
    wasm_u32_t function_capacity;
    wasmbox_value_t *globals;
    wasm_u32_t global_size;
    wasmbox_memory_block_t *memory_block;
    wasm_u32_t memory_block_size;
    wasm_u32_t memory_block_capacity;
    wasmbox_function_t *global_function;
    wasmbox_type_t **types;
    wasm_u32_t type_size;
    wasm_u32_t type_capacity;
} wasmbox_module_t;

int wasmbox_load_module(wasmbox_module_t *mod, const char *file_name,
                        wasm_u16_t file_name_len);

int wasmbox_eval_module(wasmbox_module_t *mod, wasmbox_value_t stack[],
                        wasm_u16_t result_stack_size);

int wasmbox_module_dispose(wasmbox_module_t *mod);

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
