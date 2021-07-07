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

#include <stdlib.h> // exit
#include "wasmbox/wasmbox.h"
#include "opcodes.h"
#include "allocator.h"
#include "interpreter.h"

#define LOG(MSG) fprintf(stderr, "(%s:%d)" MSG, __FILE__, __LINE__)

#define NOT_IMPLEMENTED() do { \
    LOG("not-implemented");    \
    return;                    \
} while (0)

#ifdef TRACE_VM
static void dump_stack(wasmbox_value_t *stack);
#else
#endif

static wasm_u32_t wasmbox_runtime_memory_size(wasmbox_module_t *mod) {
    return mod->memory_block_size;
}

static wasm_u32_t wasmbox_runtime_memory_grow(wasmbox_module_t *mod, wasm_u32_t new_size) {
    wasm_u32_t current_page_size = wasmbox_runtime_memory_size(mod);
    if (new_size <= 0 || current_page_size + new_size > mod->memory_block_capacity) { // TODO Need to confirm WASM spec.
        return current_page_size;
    }
    wasm_u32_t new_block_size = WASMBOX_PAGE_SIZE * new_size;
    mod->memory_block = (wasmbox_memory_block_t *) wasmbox_malloc(sizeof(*mod->memory_block) + new_block_size);
    mod->memory_block_size = new_size;
    return current_page_size;
}

static wasm_u32_t wasmbox_runtime_clz32(wasm_u32_t v) {
    return __builtin_clz(v);
}

static wasm_u32_t wasmbox_runtime_ctz32(wasm_u32_t v) {
    return __builtin_ctz(v);
}

static wasm_u64_t wasmbox_runtime_clz64(wasm_u64_t v) {
    return __builtin_clzl(v);
}

static wasm_u64_t wasmbox_runtime_ctz64(wasm_u64_t v) {
    return __builtin_ctzl(v);
}

static wasm_u32_t wasmbox_runtime_rotl32(wasm_u32_t x, wasm_u32_t y) {
#ifdef __llvm__
    return __builtin_rotateleft32(x, y);
#else
    return (x << y) | (x >> (sizeof(x) * 8 - y));
#endif
}

static wasm_u32_t wasmbox_runtime_rotr32(wasm_u32_t x, wasm_u32_t y) {
#ifdef __llvm__
    return __builtin_rotateright32(x, y);
#else
    return (x >> y) | (x << (sizeof(x) * 8 - y));
#endif
}

static wasm_u64_t wasmbox_runtime_rotl64(wasm_u64_t x, wasm_u64_t y) {
#ifdef __llvm__
    return __builtin_rotateleft64(x, y);
#else
    return (x << y) | (x >> (sizeof(x) * 8 - y));
#endif
}

static wasm_u64_t wasmbox_runtime_rotr64(wasm_u64_t x, wasm_u64_t y) {
#ifdef __llvm__
    return __builtin_rotateright64(x, y);
#else
    return (x >> y) | (x << (sizeof(x) * 8 - y));
#endif
}

void wasmbox_eval_function(wasmbox_module_t *mod, wasmbox_code_t *code, wasmbox_value_t *stack)
{
    while (1) {
#ifdef TRACE_VM
        dump_stack(stack);
        wasmbox_dump_function(code, code + 1, ">");
#endif
        switch (code->h.opcode) {
            case OPCODE_UNREACHABLE:
                exit(-1);
            case OPCODE_NOP:
                break;
            case OPCODE_SELECT:
                if (stack[code->op1.reg].u32) {
                    stack[code->op0.reg].u64 = stack[code->op2.r.reg1].u64;
                } else {
                    stack[code->op0.reg].u64 = stack[code->op2.r.reg2].u64;
                }
                code++;
                break;
            case OPCODE_EXIT:
                return;
            case OPCODE_RETURN:
                code = (wasmbox_code_t *) stack[1].u64;
                stack = (wasmbox_value_t *) stack[0].u64;
                break;
            case OPCODE_MOVE:
                stack[code->op0.reg].u64 = stack[code->op1.reg].u64;
                code++;
                break;
            case OPCODE_JUMP:
                code = code->op0.code;
                break;
            case OPCODE_JUMP_IF:
                if (stack[code->op1.reg].u32) {
                    code = code->op0.code;
                } else {
                    code++;
                }
                break;
            case OPCODE_DYNAMIC_CALL: {
                wasmbox_function_t *func = mod->functions[code->op1.index];
                code->h.opcode = OPCODE_STATIC_CALL;
                code->op1.func = func;
                /* Re-execute */
                break;
            }
            case OPCODE_STATIC_CALL: {
                wasmbox_function_t *func = code->op1.func;
                wasmbox_value_t *stack_top = &stack[code->op0.reg] + code->op2.index;
                stack_top[0].u64 = (wasm_u64_t) (uintptr_t) stack;
                stack_top[1].u64 = (wasm_u64_t) (uintptr_t) (code + 1);
                stack = stack_top;
                code = func->code;
                break;
            }
            case OPCODE_LOCAL_GET:
                stack[code->op0.reg].u64 = stack[code->op1.reg].u64;
                code++;
                break;
            case OPCODE_LOCAL_TEE:
                NOT_IMPLEMENTED();
            case OPCODE_GLOBAL_GET:
                stack[code->op0.reg].u64 = mod->globals[code->op1.reg].u64;
                code++;
                break;
            case OPCODE_GLOBAL_SET:
                mod->globals[code->op0.reg].u64 = stack[code->op1.reg].u64;
                code++;
                break;

#define LOAD_OP(itype, otype, out_type) do { \
    stack[code->op0.reg].otype = (out_type) *(itype *)&mod->memory_block->data[code->op1.index]; \
    code++;                                                                                      \
} while (0)
            case OPCODE_I32_LOAD:
                LOAD_OP(wasm_u32_t, u32, wasm_u32_t);
                break;
            case OPCODE_I64_LOAD:
                LOAD_OP(wasm_u64_t, u64, wasm_u64_t);
                break;
            case OPCODE_F32_LOAD:
                LOAD_OP(wasm_f32_t, f32, wasm_f32_t);
                break;
            case OPCODE_F64_LOAD:
                LOAD_OP(wasm_f64_t, f64, wasm_f64_t);
                break;
            case OPCODE_I32_LOAD8_S:
                LOAD_OP(wasm_s8_t , s32, wasm_s32_t);
                break;
            case OPCODE_I32_LOAD8_U:
                LOAD_OP(wasm_u8_t, u32, wasm_u32_t);
                break;
            case OPCODE_I32_LOAD16_S:
                LOAD_OP(wasm_s16_t , s32, wasm_s32_t);
                break;
            case OPCODE_I32_LOAD16_U:
                LOAD_OP(wasm_u16_t, u32, wasm_u32_t);
                break;
            case OPCODE_I64_LOAD8_S:
                LOAD_OP(wasm_s8_t, s64, wasm_s64_t);
                break;
            case OPCODE_I64_LOAD8_U:
                LOAD_OP(wasm_u8_t, u64, wasm_u64_t);
                break;
            case OPCODE_I64_LOAD16_S:
                LOAD_OP(wasm_s16_t, s64, wasm_s64_t);
                break;
            case OPCODE_I64_LOAD16_U:
                LOAD_OP(wasm_u16_t, u64, wasm_u64_t);
                break;
            case OPCODE_I64_LOAD32_S:
                LOAD_OP(wasm_s32_t, s64, wasm_s64_t);
                break;
            case OPCODE_I64_LOAD32_U:
                LOAD_OP(wasm_u32_t, u64, wasm_u64_t);
                break;

#define STORE_OP(itype, otype) do { \
    *(otype *)&mod->memory_block->data[code->op0.index] = (otype) stack[code->op1.reg].itype; \
    code++;                                                                                   \
} while (0)
            case OPCODE_I32_STORE:
                STORE_OP(u32, wasm_u32_t);
                break;
            case OPCODE_I64_STORE:
                STORE_OP(u64, wasm_u64_t);
                break;
            case OPCODE_F32_STORE:
                STORE_OP(f32, wasm_f32_t);
                break;
            case OPCODE_F64_STORE:
                STORE_OP(f64, wasm_f64_t);
                break;
            case OPCODE_I32_STORE8:
                STORE_OP(u8, wasm_u32_t);
                break;
            case OPCODE_I32_STORE16:
                STORE_OP(u16, wasm_u32_t);
                break;
            case OPCODE_I64_STORE8:
                STORE_OP(u8, wasm_u64_t);
                break;
            case OPCODE_I64_STORE16:
                STORE_OP(u16, wasm_u64_t);
                break;
            case OPCODE_I64_STORE32:
                STORE_OP(u32, wasm_u64_t);
                break;
            case OPCODE_MEMORY_SIZE:
                stack[code->op0.reg].u32 = wasmbox_runtime_memory_size(mod);
                code++;
                break;
            case OPCODE_MEMORY_GROW:
                stack[code->op0.reg].u32 = wasmbox_runtime_memory_grow(mod, stack[code->op1.reg].u32);
                code++;
                break;
#define LOAD_CONST_OP(type) do { \
    stack[code->op0.reg].type = code->op1.value.type;                                              \
    code++;                                                                                        \
} while (0)
            case OPCODE_LOAD_CONST_I32:
                LOAD_CONST_OP(u32);
                break;
            case OPCODE_LOAD_CONST_I64:
                LOAD_CONST_OP(u64);
                break;
            case OPCODE_LOAD_CONST_F32:
                LOAD_CONST_OP(f32);
                break;
            case OPCODE_LOAD_CONST_F64:
                LOAD_CONST_OP(f64);
                break;
#define ARITHMETIC_OP(arg_type, operand) \
    ARITHMETIC_OP2(arg_type, arg_type, operand)

#define ARITHMETIC_OP2(arg_type, ret_type, operand) do { \
    stack[code->op0.reg].ret_type = stack[code->op1.reg].arg_type operand stack[code->op2.reg].arg_type;            \
    code++;                                                                                             \
} while (0)
            case OPCODE_I32_EQZ:
                stack[code->op0.reg].u32 = stack[code->op1.reg].u32 == 0;
                code++;
                break;
            case OPCODE_I32_EQ:
                ARITHMETIC_OP(s32, ==);
                break;
            case OPCODE_I32_NE:
                ARITHMETIC_OP(s32, !=);
                break;
            case OPCODE_I32_LT_S:
                ARITHMETIC_OP(s32, <);
                break;
            case OPCODE_I32_LT_U:
                ARITHMETIC_OP(u32, <);
                break;
            case OPCODE_I32_GT_S:
                ARITHMETIC_OP(s32, >);
                break;
            case OPCODE_I32_GT_U:
                ARITHMETIC_OP(u32, >);
                break;
            case OPCODE_I32_LE_S:
                ARITHMETIC_OP(s32, <=);
                break;
            case OPCODE_I32_LE_U:
                ARITHMETIC_OP(u32, <=);
                break;
            case OPCODE_I32_GE_S:
                ARITHMETIC_OP(s32, >=);
                break;
            case OPCODE_I32_GE_U:
                ARITHMETIC_OP(u32, >=);
                break;
            case OPCODE_I64_EQZ:
                stack[code->op0.reg].u64 = stack[code->op1.reg].u64 == 0;
                code++;
                break;
            case OPCODE_I64_EQ:
                ARITHMETIC_OP2(u64, s32, ==);
                break;
            case OPCODE_I64_NE:
                ARITHMETIC_OP2(u64, s32, !=);
                break;
            case OPCODE_I64_LT_S:
                ARITHMETIC_OP2(s64, s32, <);
                break;
            case OPCODE_I64_LT_U:
                ARITHMETIC_OP2(u64, s32, <);
                break;
            case OPCODE_I64_GT_S:
                ARITHMETIC_OP2(s64, s32, >);
                break;
            case OPCODE_I64_GT_U:
                ARITHMETIC_OP2(u64, s32, >);
                break;
            case OPCODE_I64_LE_S:
                ARITHMETIC_OP2(s64, s32, <=);
                break;
            case OPCODE_I64_LE_U:
                ARITHMETIC_OP2(u64, s32, <=);
                break;
            case OPCODE_I64_GE_S:
                ARITHMETIC_OP2(s64, s32, >=);
                break;
            case OPCODE_I64_GE_U:
                ARITHMETIC_OP2(u64, s32, >=);
                break;
            case OPCODE_F32_EQ:
                ARITHMETIC_OP2(f32, s32, ==);
                break;
            case OPCODE_F32_NE:
                ARITHMETIC_OP2(f32, s32, !=);
                break;
            case OPCODE_F32_LT:
                ARITHMETIC_OP2(f32, s32, <);
                break;
            case OPCODE_F32_GT:
                ARITHMETIC_OP2(f32, s32, >);
                break;
            case OPCODE_F32_LE:
                ARITHMETIC_OP2(f32, s32, <=);
                break;
            case OPCODE_F32_GE:
                ARITHMETIC_OP2(f32, s32, >=);
                break;
            case OPCODE_F64_EQ:
                ARITHMETIC_OP2(f64, s32, ==);
                break;
            case OPCODE_F64_NE:
                ARITHMETIC_OP2(f64, s32, !=);
                break;
            case OPCODE_F64_LT:
                ARITHMETIC_OP2(f64, s32, <);
                break;
            case OPCODE_F64_GT:
                ARITHMETIC_OP2(f64, s32, >);
                break;
            case OPCODE_F64_LE:
                ARITHMETIC_OP2(f64, s32, <=);
                break;
            case OPCODE_F64_GE:
                ARITHMETIC_OP2(f64, s32, >=);
                break;
            case OPCODE_I32_CLZ:
                stack[code->op0.reg].u32 = wasmbox_runtime_clz32(stack[code->op1.reg].u64);
                code++;
                break;
            case OPCODE_I32_CTZ:
                stack[code->op0.reg].u32 = wasmbox_runtime_ctz32(stack[code->op1.reg].u64);
                code++;
                break;
            case OPCODE_I32_POPCNT:
                NOT_IMPLEMENTED();
            case OPCODE_I32_ADD:
                ARITHMETIC_OP(u32, +);
                break;
            case OPCODE_I32_SUB:
                ARITHMETIC_OP(u32, -);
                break;
            case OPCODE_I32_MUL:
                ARITHMETIC_OP(u32, *);
                break;
            case OPCODE_I32_DIV_S:
                ARITHMETIC_OP(s32, /);
                break;
            case OPCODE_I32_DIV_U:
                ARITHMETIC_OP(u32, /);
                break;
            case OPCODE_I32_REM_S:
                ARITHMETIC_OP(s32, %);
                break;
            case OPCODE_I32_REM_U:
                ARITHMETIC_OP(u32, %);
                break;
            case OPCODE_I32_AND:
                ARITHMETIC_OP(u32, &);
                break;
            case OPCODE_I32_OR:
                ARITHMETIC_OP(u32, |);
                break;
            case OPCODE_I32_XOR:
                ARITHMETIC_OP(u32, ^);
                break;
            case OPCODE_I32_SHL:
                ARITHMETIC_OP(u32, <<);
                break;
            case OPCODE_I32_SHR_S:
                ARITHMETIC_OP(s32, >>);
                break;
            case OPCODE_I32_SHR_U:
                ARITHMETIC_OP(u32, >>);
                break;
            case OPCODE_I32_ROTL:
                stack[code->op0.reg].u32 = wasmbox_runtime_rotl32(stack[code->op1.reg].u32, stack[code->op2.reg].u32);
                code++;
                break;
            case OPCODE_I32_ROTR:
                stack[code->op0.reg].u32 = wasmbox_runtime_rotr32(stack[code->op1.reg].u32, stack[code->op2.reg].u32);
                code++;
                break;
            case OPCODE_I64_CLZ:
                stack[code->op0.reg].u64 = wasmbox_runtime_clz64(stack[code->op1.reg].u64);
                code++;
                break;
            case OPCODE_I64_CTZ:
                stack[code->op0.reg].u64 = wasmbox_runtime_ctz64(stack[code->op1.reg].u64);
                code++;
                break;
            case OPCODE_I64_POPCNT: NOT_IMPLEMENTED();
            case OPCODE_I64_ADD:
                ARITHMETIC_OP2(s64, s64, +);
                break;
            case OPCODE_I64_SUB:
                ARITHMETIC_OP2(s64, s64, -);
                break;
            case OPCODE_I64_MUL:
                ARITHMETIC_OP2(s64, s64, *);
                break;
            case OPCODE_I64_DIV_S:
                ARITHMETIC_OP2(s64, s64, /);
                break;
            case OPCODE_I64_DIV_U:
                ARITHMETIC_OP2(u64, u64, /);
                break;
            case OPCODE_I64_REM_S:
                ARITHMETIC_OP2(s64, s64, %);
                break;
            case OPCODE_I64_REM_U:
                ARITHMETIC_OP2(u64, u64, %);
                break;
            case OPCODE_I64_AND:
                ARITHMETIC_OP2(u64, u64, &);
                break;
            case OPCODE_I64_OR:
                ARITHMETIC_OP2(u64, u64, |);
                break;
            case OPCODE_I64_XOR:
                ARITHMETIC_OP2(u64, u64, ^);
                break;
            case OPCODE_I64_SHL:
                ARITHMETIC_OP2(u64, u64, <<);
                break;
            case OPCODE_I64_SHR_S:
                ARITHMETIC_OP2(s64, s64, >>);
                break;
            case OPCODE_I64_SHR_U:
                ARITHMETIC_OP2(u64, u64, >>);
                break;
            case OPCODE_I64_ROTL:
                stack[code->op0.reg].u64 = wasmbox_runtime_rotl64(stack[code->op1.reg].u64, stack[code->op2.reg].u64);
                code++;
                break;
            case OPCODE_I64_ROTR:
                stack[code->op0.reg].u64 = wasmbox_runtime_rotr64(stack[code->op1.reg].u64, stack[code->op2.reg].u64);
                code++;
                break;
            case OPCODE_F32_ABS: NOT_IMPLEMENTED();
            case OPCODE_F32_NEG: NOT_IMPLEMENTED();
            case OPCODE_F32_CEIL: NOT_IMPLEMENTED();
            case OPCODE_F32_FLOOR: NOT_IMPLEMENTED();
            case OPCODE_F32_TRUNC: NOT_IMPLEMENTED();
            case OPCODE_F32_NEAREST: NOT_IMPLEMENTED();
            case OPCODE_F32_SQRT: NOT_IMPLEMENTED();
            case OPCODE_F32_ADD:
                ARITHMETIC_OP2(f32, f32, +);
                break;
            case OPCODE_F32_SUB:
                ARITHMETIC_OP2(f32, f32, -);
                break;
            case OPCODE_F32_MUL:
                ARITHMETIC_OP2(f32, f32, *);
                break;
            case OPCODE_F32_DIV:
                ARITHMETIC_OP2(f32, f32, /);
                break;
            case OPCODE_F32_MIN: NOT_IMPLEMENTED();
            case OPCODE_F32_MAX: NOT_IMPLEMENTED();
            case OPCODE_F32_COPYSIGN: NOT_IMPLEMENTED();
            case OPCODE_F64_ABS: NOT_IMPLEMENTED();
            case OPCODE_F64_NEG: NOT_IMPLEMENTED();
            case OPCODE_F64_CEIL: NOT_IMPLEMENTED();
            case OPCODE_F64_FLOOR: NOT_IMPLEMENTED();
            case OPCODE_F64_TRUNC: NOT_IMPLEMENTED();
            case OPCODE_F64_NEAREST: NOT_IMPLEMENTED();
            case OPCODE_F64_SQRT: NOT_IMPLEMENTED();
            case OPCODE_F64_ADD:
                ARITHMETIC_OP2(f64, f64, +);
                break;
            case OPCODE_F64_SUB:
                ARITHMETIC_OP2(f64, f64, -);
                break;
            case OPCODE_F64_MUL:
                ARITHMETIC_OP2(f64, f64, *);
                break;
            case OPCODE_F64_DIV:
                ARITHMETIC_OP2(f64, f64, /);
                break;
            case OPCODE_F64_MIN: NOT_IMPLEMENTED();
            case OPCODE_F64_MAX: NOT_IMPLEMENTED();
            case OPCODE_F64_COPYSIGN: NOT_IMPLEMENTED();
            case OPCODE_WRAP_I64: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F64_U: NOT_IMPLEMENTED();
#define CONVERT_OP(arg_type, ret_type, operand) do { \
    stack[code->op0.reg].ret_type = (operand) stack[code->op1.reg].arg_type;                                \
    code++;                                                                                                 \
} while (0)
            case OPCODE_I64_EXTEND_I32_S:
                CONVERT_OP(s32, s64, wasm_s64_t);
                break;
            case OPCODE_I64_EXTEND_I32_U:
                CONVERT_OP(u32, u64, wasm_u64_t);
                break;
            case OPCODE_I64_TRUNC_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F64_U: NOT_IMPLEMENTED();

            case OPCODE_F32_CONVERT_I32_S:
                CONVERT_OP(s32, f32, wasm_f32_t);
                break;
            case OPCODE_F32_CONVERT_I32_U:
                CONVERT_OP(u32, f32, wasm_f32_t);
                break;
            case OPCODE_F32_CONVERT_I64_S:
                CONVERT_OP(s64, f32, wasm_f32_t);
                break;
            case OPCODE_F32_CONVERT_I64_U:
                CONVERT_OP(u64, f32, wasm_f32_t);
                break;
            case OPCODE_F32_DEMOTE_F64: NOT_IMPLEMENTED();
            case OPCODE_F64_CONVERT_I32_S:
                CONVERT_OP(s32, f64, wasm_f64_t);
                break;
            case OPCODE_F64_CONVERT_I32_U:
                CONVERT_OP(u32, f64, wasm_f64_t);
                break;
            case OPCODE_F64_CONVERT_I64_S:
                CONVERT_OP(s64, f64, wasm_f64_t);
                break;
            case OPCODE_F64_CONVERT_I64_U:
                CONVERT_OP(u64, f64, wasm_f32_t);
                break;
            case OPCODE_F64_PROMOTE_F32: NOT_IMPLEMENTED();
            case OPCODE_I32_REINTERPRET_F32:
                stack[code->op0.reg].u32 = stack[code->op1.reg].u32;
                code++;
                break;
            case OPCODE_I64_REINTERPRET_F64:
                stack[code->op0.reg].u64 = stack[code->op1.reg].u64;
                code++;
                break;
            case OPCODE_F32_REINTERPRET_I32:
                stack[code->op0.reg].f32 = stack[code->op1.reg].f32;
                code++;
                break;
            case OPCODE_F64_REINTERPRET_I64:
                stack[code->op0.reg].f64 = stack[code->op1.reg].f64;
                code++;
                break;
#define EXTEND_OP(arg_type, ret_type, operand) do {                                     \
    stack[code->op0.reg].ret_type = (operand) stack[code->op1.reg].arg_type;            \
    code++;                                                                             \
} while (0)
            case OPCODE_I32_EXTEND8_S:
                EXTEND_OP(s8, s32, wasm_s32_t);
                break;
            case OPCODE_I32_EXTEND16_S:
                EXTEND_OP(s16, s32, wasm_s32_t);
                break;
            case OPCODE_I64_EXTEND8_S:
                EXTEND_OP(s8, s64, wasm_s64_t);
                break;
            case OPCODE_I64_EXTEND16_S:
                EXTEND_OP(s16, s64, wasm_s64_t);
                break;
            case OPCODE_I64_EXTEND32_S:
                EXTEND_OP(s32, s64, wasm_s64_t);
                break;
            case OPCODE_I32_TRUNC_SAT_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_SAT_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_SAT_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_SAT_F64_U: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_SAT_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_SAT_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_SAT_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_SAT_F64_U: NOT_IMPLEMENTED();

            default:
                LOG("unknown opcode");
                return;
        }
    }
}

void wasmbox_dump_function(wasmbox_code_t *code_start, wasmbox_code_t *code_end, const char *indent)
{
    wasmbox_code_t *code = code_start;
    while (code < code_end) {
        fprintf(stdout, "[%03ld:%p] ", code - code_start, code);
        switch (code->h.opcode) {
            case OPCODE_UNREACHABLE:
            case OPCODE_NOP:
            case OPCODE_SELECT:
                fprintf(stdout, "%sstack[%d].u64 = stack[%d].u64 ? stack[%d].u64 : stack[%d].u64\n",
                        indent, code->op0.reg, code->op1.reg, code->op2.r.reg1, code->op2.r.reg2);
                break;
            case OPCODE_EXIT:
                fprintf(stdout, "%sexit\n", indent);
                break;
            case OPCODE_RETURN:
                fprintf(stdout, "%sreturn;\n", indent);
                break;
            case OPCODE_MOVE:
                fprintf(stdout, "%sstack[%d].u64= stack[%d].u64\n", indent, code->op0.reg, code->op1.reg);
                break;
            case OPCODE_JUMP:
                fprintf(stdout, "%sjump to %p\n", indent, code->op0.code);
                break;
            case OPCODE_JUMP_IF:
                fprintf(stdout, "%sjump to %p if stack[%d].u32\n", indent, code->op0.code, code->op1.reg);
                break;
            case OPCODE_DYNAMIC_CALL:
                fprintf(stdout, "%sstack[%d].u64= func%u()\n", indent, code->op0.reg, code->op1.index);
                break;
            case OPCODE_STATIC_CALL:
                fprintf(stdout, "%sstack[%d].u64= func%p([args:%d, returns:%d])\n", indent,
                        code->op0.reg, code->op1.func,code->op1.func->type->argument_size,
                        code->op1.func->type->argument_size);
                break;
            case OPCODE_LOCAL_GET:
                fprintf(stdout, "%sstack[%d].u64= stack[%d].u64\n", indent, code->op0.reg, code->op1.reg);
                break;
            case OPCODE_LOCAL_TEE:
                NOT_IMPLEMENTED();
            case OPCODE_GLOBAL_GET:
                fprintf(stdout, "%sstack[%d].u64= global[%d].u64\n", indent, code->op0.reg, code->op1.reg);
                break;
            case OPCODE_GLOBAL_SET:
                fprintf(stdout, "%sglobal[%d].u64= stack[%d].u64\n", indent, code->op0.reg, code->op1.reg);
                break;
#define DUMP_LOAD_OP(itype, otype) do { \
    fprintf(stdout, "stack[%d]." # otype " = (" #otype ") *(" #itype " *) &memory[%d]\n", \
            code->op0.reg, code->op1.index);                                              \
} while (0)
            case OPCODE_I32_LOAD:
                DUMP_LOAD_OP(u32, u32);
                break;
            case OPCODE_I64_LOAD:
                DUMP_LOAD_OP(u64, u64);
                break;
            case OPCODE_F32_LOAD:
                DUMP_LOAD_OP(f32, f32);
                break;
            case OPCODE_F64_LOAD:
                DUMP_LOAD_OP(f64, f64);
                break;
            case OPCODE_I32_LOAD8_S:
                DUMP_LOAD_OP(s8, s32);
                break;
            case OPCODE_I32_LOAD8_U:
                DUMP_LOAD_OP(u8, u32);
                break;
            case OPCODE_I32_LOAD16_S:
                DUMP_LOAD_OP(s16, s32);
                break;
            case OPCODE_I32_LOAD16_U:
                DUMP_LOAD_OP(u16, u32);
                break;
            case OPCODE_I64_LOAD8_S:
                DUMP_LOAD_OP(s8, s64);
                break;
            case OPCODE_I64_LOAD8_U:
                DUMP_LOAD_OP(u8, u64);
                break;
            case OPCODE_I64_LOAD16_S:
                DUMP_LOAD_OP(s16, s64);
                break;
            case OPCODE_I64_LOAD16_U:
                DUMP_LOAD_OP(u16, u64);
                break;
            case OPCODE_I64_LOAD32_S:
                DUMP_LOAD_OP(s32, s64);
                break;
            case OPCODE_I64_LOAD32_U:
                DUMP_LOAD_OP(u32, u64);
                break;
#define DUMP_STORE_OP(itype, otype) do { \
    fprintf(stdout, "*(" #otype " *) &memory[%d] = (" #otype ") = stack[%d]." # itype "\n", \
            code->op0.index, code->op1.reg);                                                \
} while (0)
            case OPCODE_I32_STORE:
                DUMP_STORE_OP(u32, u32);
                break;
            case OPCODE_I64_STORE:
                DUMP_STORE_OP(u64, u64);
                break;
            case OPCODE_F32_STORE:
                DUMP_STORE_OP(f32, f32);
                break;
            case OPCODE_F64_STORE:
                DUMP_STORE_OP(f64, f64);
                break;
            case OPCODE_I32_STORE8:
                DUMP_STORE_OP(u32, u8);
                break;
            case OPCODE_I32_STORE16:
                DUMP_STORE_OP(u32, u16);
                break;
            case OPCODE_I64_STORE8:
                DUMP_STORE_OP(u64, u8);
                break;
            case OPCODE_I64_STORE16:
                DUMP_STORE_OP(u64, u16);
                break;
            case OPCODE_I64_STORE32:
                DUMP_STORE_OP(u64, u32);
                break;
            case OPCODE_MEMORY_SIZE:
                fprintf(stdout, "%sstack[%d].u32 = memory.size\n", indent, code->op0.reg);
                break;
            case OPCODE_MEMORY_GROW:
                fprintf(stdout, "%sstack[%d].u32 = memory.grow(stack[%d].u32)\n", indent, code->op0.reg, code->op1.reg);
                break;
#define DUMP_LOAD_CONST_OP(type, formatter) \
    fprintf(stdout, "%sstack[%d]." # type "= " formatter "\n", indent, code->op0.reg, code->op1.value.type)
            case OPCODE_LOAD_CONST_I32:
                DUMP_LOAD_CONST_OP(u32, "%d");
                break;
            case OPCODE_LOAD_CONST_I64:
                DUMP_LOAD_CONST_OP(u64, "%lld");
                break;
            case OPCODE_LOAD_CONST_F32:
                DUMP_LOAD_CONST_OP(f32, "%f");
                break;
            case OPCODE_LOAD_CONST_F64:
                DUMP_LOAD_CONST_OP(f64, "%g");
                break;
#define DUMP_ARITHMETIC_OP(type, operand_str) \
    fprintf(stdout, "%sstack[%d]." # type "= stack[%d]." # type " " operand_str " stack[%d]." #type "\n", \
            indent, code->op0.reg, code->op2.reg, code->op1.reg)

            case OPCODE_I32_EQZ:
                fprintf(stdout, "%sstack[%d].u32= stack[%d].u32 == 0\n", indent, code->op0.reg, code->op1.reg);
                break;
            case OPCODE_I32_EQ:
                DUMP_ARITHMETIC_OP(s32, "==");
                break;
            case OPCODE_I32_NE:
                DUMP_ARITHMETIC_OP(s32, "!=");
                break;
            case OPCODE_I32_LT_S:
                DUMP_ARITHMETIC_OP(s32, "<");
                break;
            case OPCODE_I32_LT_U:
                DUMP_ARITHMETIC_OP(u32, "<");
                break;
            case OPCODE_I32_GT_S:
                DUMP_ARITHMETIC_OP(s32, ">");
                break;
            case OPCODE_I32_GT_U:
                DUMP_ARITHMETIC_OP(u32, ">");
                break;
            case OPCODE_I32_LE_S:
                DUMP_ARITHMETIC_OP(s32, "<=");
                break;
            case OPCODE_I32_LE_U:
                DUMP_ARITHMETIC_OP(u32, "<=");
                break;
            case OPCODE_I32_GE_S:
                DUMP_ARITHMETIC_OP(s32, ">=");
                break;
            case OPCODE_I32_GE_U:
                DUMP_ARITHMETIC_OP(u32, ">=");
                break;
            case OPCODE_I64_EQZ:
                fprintf(stdout, "%sstack[%d].u64= stack[%d].u64 == 0\n", indent, code->op0.reg, code->op1.reg);
                break;
            case OPCODE_I64_EQ:
                DUMP_ARITHMETIC_OP(s64, "==");
                break;
            case OPCODE_I64_NE:
                DUMP_ARITHMETIC_OP(s64, "!=");
                break;
            case OPCODE_I64_LT_S:
                DUMP_ARITHMETIC_OP(s64, "<");
                break;
            case OPCODE_I64_LT_U:
                DUMP_ARITHMETIC_OP(u64, "<");
                break;
            case OPCODE_I64_GT_S:
                DUMP_ARITHMETIC_OP(s64, ">");
                break;
            case OPCODE_I64_GT_U:
                DUMP_ARITHMETIC_OP(u64, ">");
                break;
            case OPCODE_I64_LE_S:
                DUMP_ARITHMETIC_OP(s64, "<=");
                break;
            case OPCODE_I64_LE_U:
                DUMP_ARITHMETIC_OP(u64, "<=");
                break;
            case OPCODE_I64_GE_S:
                DUMP_ARITHMETIC_OP(s64, ">=");
                break;
            case OPCODE_I64_GE_U:
                DUMP_ARITHMETIC_OP(u64, ">=");
                break;
            case OPCODE_F32_EQ:
                DUMP_ARITHMETIC_OP(f32, "==");
                break;
            case OPCODE_F32_NE:
                DUMP_ARITHMETIC_OP(f32, "!=");
                break;
            case OPCODE_F32_LT:
                DUMP_ARITHMETIC_OP(f32, "<");
                break;
            case OPCODE_F32_GT:
                DUMP_ARITHMETIC_OP(f32, ">");
                break;
            case OPCODE_F32_LE:
                DUMP_ARITHMETIC_OP(f32, "<=");
                break;
            case OPCODE_F32_GE:
                DUMP_ARITHMETIC_OP(f32, ">=");
                break;
            case OPCODE_F64_EQ:
                DUMP_ARITHMETIC_OP(f64, "==");
                break;
            case OPCODE_F64_NE:
                DUMP_ARITHMETIC_OP(f64, "!=");
                break;
            case OPCODE_F64_LT:
                DUMP_ARITHMETIC_OP(f64, "<");
                break;
            case OPCODE_F64_GT:
                DUMP_ARITHMETIC_OP(f64, ">");
                break;
            case OPCODE_F64_LE:
                DUMP_ARITHMETIC_OP(f64, "<=");
                break;
            case OPCODE_F64_GE:
                DUMP_ARITHMETIC_OP(f64, ">=");
                break;
            case OPCODE_I32_CLZ:
                fprintf(stdout, "stack[%d].u32 = clz(stack[%d].u32)\n",code->op0.reg, code->op1.reg);
                break;
            case OPCODE_I32_CTZ:
                fprintf(stdout, "stack[%d].u32 = ctz(stack[%d].u32)\n",code->op0.reg, code->op1.reg);
                break;
            case OPCODE_I32_POPCNT:
                NOT_IMPLEMENTED();
            case OPCODE_I32_ADD:
                DUMP_ARITHMETIC_OP(u32, "+");
                break;
            case OPCODE_I32_SUB:
                DUMP_ARITHMETIC_OP(u32, "-");
                break;
            case OPCODE_I32_MUL:
                DUMP_ARITHMETIC_OP(u32, "*");
                break;
            case OPCODE_I32_DIV_S:
                DUMP_ARITHMETIC_OP(s32, "/");
                break;
            case OPCODE_I32_DIV_U:
                DUMP_ARITHMETIC_OP(u32, "/");
                break;
            case OPCODE_I32_REM_S:
                DUMP_ARITHMETIC_OP(s32, "%%");
                break;
            case OPCODE_I32_REM_U:
                DUMP_ARITHMETIC_OP(u32, "%%");
                break;
            case OPCODE_I32_AND:
                DUMP_ARITHMETIC_OP(u32, "&");
                break;
            case OPCODE_I32_OR:
                DUMP_ARITHMETIC_OP(u64, "|");
                break;
            case OPCODE_I32_XOR:
                DUMP_ARITHMETIC_OP(u32, "^");
                break;
            case OPCODE_I32_SHL:
                DUMP_ARITHMETIC_OP(u32, "<<");
                break;
            case OPCODE_I32_SHR_S:
                DUMP_ARITHMETIC_OP(s32, ">>");
                break;
            case OPCODE_I32_SHR_U:
                DUMP_ARITHMETIC_OP(u32, ">>");
                break;
            case OPCODE_I32_ROTL:
                fprintf(stdout, "%sstack[%d].u32 = rotl(stack[%d].u32, stack[%d].u32)\n",
                        indent, code->op0.reg, code->op1.reg, code->op2.reg);
                break;
            case OPCODE_I32_ROTR:
                fprintf(stdout, "%sstack[%d].u32 = rotr(stack[%d].u32, stack[%d].u32)\n",
                        indent, code->op0.reg, code->op1.reg, code->op2.reg);
                break;
            case OPCODE_I64_CLZ:
                fprintf(stdout, "stack[%d].u64 = clz(stack[%d].u64)\n",code->op0.reg, code->op1.reg);
                break;
            case OPCODE_I64_CTZ:
                fprintf(stdout, "stack[%d].u64 = ctz(stack[%d].u64)\n",code->op0.reg, code->op1.reg);
                break;
            case OPCODE_I64_POPCNT: NOT_IMPLEMENTED();
            case OPCODE_I64_ADD:
                DUMP_ARITHMETIC_OP(u64, "+");
                break;
            case OPCODE_I64_SUB:
                DUMP_ARITHMETIC_OP(u64, "-");
                break;
            case OPCODE_I64_MUL:
                DUMP_ARITHMETIC_OP(u64, "*");
                break;
            case OPCODE_I64_DIV_S:
                DUMP_ARITHMETIC_OP(s64, "-");
                break;
            case OPCODE_I64_DIV_U:
                DUMP_ARITHMETIC_OP(u64, "/");
                break;
            case OPCODE_I64_REM_S:
                DUMP_ARITHMETIC_OP(s64, "%%");
                break;
            case OPCODE_I64_REM_U:
                DUMP_ARITHMETIC_OP(u64, "%%");
                break;
            case OPCODE_I64_AND:
                DUMP_ARITHMETIC_OP(u64, "&");
                break;
            case OPCODE_I64_OR:
                DUMP_ARITHMETIC_OP(u64, "|");
                break;
            case OPCODE_I64_XOR:
                DUMP_ARITHMETIC_OP(u64, "^");
                break;
            case OPCODE_I64_SHL:
                DUMP_ARITHMETIC_OP(u64, "<<");
                break;
            case OPCODE_I64_SHR_S:
                DUMP_ARITHMETIC_OP(s64, ">>");
                break;
            case OPCODE_I64_SHR_U:
                DUMP_ARITHMETIC_OP(u64, ">>");
                break;
            case OPCODE_I64_ROTL:
                fprintf(stdout, "%sstack[%d].u64 = rotl(stack[%d].u64, stack[%d].u64)\n",
                        indent, code->op0.reg, code->op1.reg, code->op2.reg);
                break;
            case OPCODE_I64_ROTR:
                fprintf(stdout, "%sstack[%d].u64 = rotr(stack[%d].u64, stack[%d].u64)\n",
                        indent, code->op0.reg, code->op1.reg, code->op2.reg);
                break;
            case OPCODE_F32_ABS: NOT_IMPLEMENTED();
            case OPCODE_F32_NEG: NOT_IMPLEMENTED();
            case OPCODE_F32_CEIL: NOT_IMPLEMENTED();
            case OPCODE_F32_FLOOR: NOT_IMPLEMENTED();
            case OPCODE_F32_TRUNC: NOT_IMPLEMENTED();
            case OPCODE_F32_NEAREST: NOT_IMPLEMENTED();
            case OPCODE_F32_SQRT: NOT_IMPLEMENTED();
            case OPCODE_F32_ADD:
                DUMP_ARITHMETIC_OP(f32, "+");
                break;
            case OPCODE_F32_SUB:
                DUMP_ARITHMETIC_OP(f32, "-");
                break;
            case OPCODE_F32_MUL:
                DUMP_ARITHMETIC_OP(f32, "*");
                break;
            case OPCODE_F32_DIV:
                DUMP_ARITHMETIC_OP(f32, "/");
                break;
            case OPCODE_F32_MIN: NOT_IMPLEMENTED();
            case OPCODE_F32_MAX: NOT_IMPLEMENTED();
            case OPCODE_F32_COPYSIGN: NOT_IMPLEMENTED();
            case OPCODE_F64_ABS: NOT_IMPLEMENTED();
            case OPCODE_F64_NEG: NOT_IMPLEMENTED();
            case OPCODE_F64_CEIL: NOT_IMPLEMENTED();
            case OPCODE_F64_FLOOR: NOT_IMPLEMENTED();
            case OPCODE_F64_TRUNC: NOT_IMPLEMENTED();
            case OPCODE_F64_NEAREST: NOT_IMPLEMENTED();
            case OPCODE_F64_SQRT: NOT_IMPLEMENTED();
            case OPCODE_F64_ADD:
                DUMP_ARITHMETIC_OP(f64, "+");
                break;
            case OPCODE_F64_SUB:
                DUMP_ARITHMETIC_OP(f64, "-");
                break;
            case OPCODE_F64_MUL:
                DUMP_ARITHMETIC_OP(f64, "*");
                break;
            case OPCODE_F64_DIV:
                DUMP_ARITHMETIC_OP(f64, "/");
                break;
            case OPCODE_F64_MIN: NOT_IMPLEMENTED();
            case OPCODE_F64_MAX: NOT_IMPLEMENTED();
            case OPCODE_F64_COPYSIGN: NOT_IMPLEMENTED();
            case OPCODE_WRAP_I64: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F64_U: NOT_IMPLEMENTED();
#define DUMP_CONVERT_OP(arg_type, ret_type) do { \
    fprintf(stdout, "stack[%d]." # ret_type " = (" # ret_type ") stack[%d]." # arg_type "\n", \
            code->op0.reg, code->op1.reg);                                   \
} while (0)
            case OPCODE_I64_EXTEND_I32_S:
                DUMP_CONVERT_OP(s32, s64);
                break;
            case OPCODE_I64_EXTEND_I32_U:
                DUMP_CONVERT_OP(u32, u64);
                break;
            case OPCODE_I64_TRUNC_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F64_U: NOT_IMPLEMENTED();

            case OPCODE_F32_CONVERT_I32_S:
                DUMP_CONVERT_OP(s32, f32);
                break;
            case OPCODE_F32_CONVERT_I32_U:
                DUMP_CONVERT_OP(u32, f32);
                break;
            case OPCODE_F32_CONVERT_I64_S:
                DUMP_CONVERT_OP(s64, f32);
                break;
            case OPCODE_F32_CONVERT_I64_U:
                DUMP_CONVERT_OP(u64, f32);
                break;
            case OPCODE_F32_DEMOTE_F64: NOT_IMPLEMENTED();
            case OPCODE_F64_CONVERT_I32_S:
                DUMP_CONVERT_OP(s32, f64);
                break;
            case OPCODE_F64_CONVERT_I32_U:
                DUMP_CONVERT_OP(u32, f64);
                break;
            case OPCODE_F64_CONVERT_I64_S:
                DUMP_CONVERT_OP(s64, f64);
                break;
            case OPCODE_F64_CONVERT_I64_U:
                DUMP_CONVERT_OP(u64, f64);
                break;
            case OPCODE_F64_PROMOTE_F32: NOT_IMPLEMENTED();
            case OPCODE_I32_REINTERPRET_F32:
                fprintf(stdout, "%sstack[%d].u32 = reinterpret_cast(stack[%d].f32)\n",
                        indent, code->op0.reg, code->op1.reg);
                break;
            case OPCODE_I64_REINTERPRET_F64:
                fprintf(stdout, "%sstack[%d].u64 = reinterpret_cast(stack[%d].f64)\n",
                        indent, code->op0.reg, code->op1.reg);
                break;
            case OPCODE_F32_REINTERPRET_I32:
                fprintf(stdout, "%sstack[%d].f32 = reinterpret_cast(stack[%d].u32)\n",
                        indent, code->op0.reg, code->op1.reg);
                break;
            case OPCODE_F64_REINTERPRET_I64:
                fprintf(stdout, "%sstack[%d].f64 = reinterpret_cast(stack[%d].u64)\n",
                        indent, code->op0.reg, code->op1.reg);
                break;
            case OPCODE_I32_EXTEND8_S:
                DUMP_CONVERT_OP(s32, s8);
                break;
            case OPCODE_I32_EXTEND16_S:
                DUMP_CONVERT_OP(s32, s16);
                break;
            case OPCODE_I64_EXTEND8_S:
                DUMP_CONVERT_OP(s64, s8);
                break;
            case OPCODE_I64_EXTEND16_S:
                DUMP_CONVERT_OP(s64, s16);
                break;
            case OPCODE_I64_EXTEND32_S:
                DUMP_CONVERT_OP(s64, s32);
                break;
            case OPCODE_I32_TRUNC_SAT_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_SAT_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_SAT_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_SAT_F64_U: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_SAT_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_SAT_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_SAT_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_SAT_F64_U: NOT_IMPLEMENTED();

            default:
                LOG("unknown opcode");
                return;
        }
        code++;
    }
}

#ifdef TRACE_VM
static wasmbox_value_t *global_stack;
static void dump_stack(wasmbox_value_t *stack) {
    if (1 && global_stack != NULL) {
        fprintf(stdout, "-----------------\n");
        int diff = stack - global_stack;
        for (int i = 0; i < 10; ++i) {
            fprintf(stdout, "stack[%d] = %d (u64:%llu, %p)\n", (i - diff), global_stack[i].s32, global_stack[i].u64,
                    (void *)(intptr_t)global_stack[i].u64);
        }
        fprintf(stdout, "-----------------\n");
    }
}
#endif

static wasmbox_function_t *wasmbox_module_find_entrypoint(wasmbox_module_t *mod)
{
    for (wasm_u32_t i = 0; i < mod->function_size; ++i) {
        wasmbox_function_t *func = mod->functions[i];
        wasm_u32_t start_len = strlen("_start");
        if (func->name != NULL && func->name->len == start_len &&
            strncmp((const char *) func->name->value, "_start", start_len) == 0) {
            return func;
        }
    }
    return NULL;
}

int wasmbox_eval_module(wasmbox_module_t *mod, wasmbox_value_t stack[],
                        wasm_u16_t result_stack_size) {
    wasmbox_function_t *func = wasmbox_module_find_entrypoint(mod);
    if (func == NULL) {
        LOG("_start function not found");
        return -1;
    }
#ifdef TRACE_VM
    global_stack = stack;
#endif
    wasmbox_value_t *stack_top = stack + func->type->return_size;
    wasmbox_code_t code[1] = {};
    code[0].h.opcode = OPCODE_EXIT;
    stack_top[0].u64 = (wasm_u64_t) (uintptr_t) stack_top;
    stack_top[1].u64 = (wasm_u64_t) (uintptr_t) code;
#ifdef TRACE_VM
    dump_stack(stack_top);
#endif
    wasmbox_eval_function(mod, func->code, stack_top);
    return 0;
}
