/*
 * Copyright (C) 2020 Masahiro Ide
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

#ifndef WASMBOX_COMPILER_H
#define WASMBOX_COMPILER_H

#include "wasmbox/wasmbox.h"
#include "input-stream.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*wasmbox_op_decode_func_t)(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                                        wasmbox_function_t *func, wasm_u8_t op);

typedef struct wasmbox_op_decorder_t {
    wasm_u8_t lower;
    wasm_u8_t upper;
    wasmbox_op_decode_func_t func;
} wasmbox_op_decorder_t;

/* unreachable, nop */
#define DUMMY_INST_EACH(OP_INST_0) \
    OP_INST_0(0x00, any, unreachable, OPCODE_UNREACHABLE) \
    OP_INST_0(0x01, any, nop,         OPCODE_NOP)

/* Parametric Instructions */
#define PARAMETRIC_INST_EACH(OP_INST_0) \
    OP_INST_0(0x1A, any, drop,   OPCODE_DROP) \
    OP_INST_0(0x1B, any, select, OPCODE_SELECT)

/* Variable Instruction */
#define VARIABLE_INST_EACH(OP_INST_PARAM1) \
    OP_INST_PARAM1(0x20, x:localidx,  any, local_get,  OPCODE_LOCAL_GET) \
    OP_INST_PARAM1(0x21, x:localidx,  any, local_set,  OPCODE_LOCAL_SET) \
    OP_INST_PARAM1(0x22, x:localidx,  any, local_tee,  OPCODE_LOCAL_TEE) \
    OP_INST_PARAM1(0x23, x:globalidx, any, global_get, OPCODE_GLOBAL_GET) \
    OP_INST_PARAM1(0x24, x:globalidx, any, global_set, OPCODE_GLOBAL_SET)

/* Memory Instructions */
#define MEMORY_INST_EACH(OP_INST_PARAM1) \
    OP_INST_PARAM1(0x28, i32, s32, load,  OPCODE_I32_LOAD) \
    OP_INST_PARAM1(0x29, i64, s32, load,  OPCODE_I64_LOAD) \
    OP_INST_PARAM1(0x2A, f32, s32, load,  OPCODE_F32_LOAD) \
    OP_INST_PARAM1(0x2B, f64, s32, load,  OPCODE_F64_LOAD) \
    OP_INST_PARAM1(0x2C, i32, s8,  load,  OPCODE_I32_LOAD8_S) \
    OP_INST_PARAM1(0x2D, i32, u8,  load,  OPCODE_I32_LOAD8_U) \
    OP_INST_PARAM1(0x2E, i32, s16, load,  OPCODE_I32_LOAD16_S) \
    OP_INST_PARAM1(0x2F, i32, u32, load,  OPCODE_I32_LOAD16_U) \
    OP_INST_PARAM1(0x30, i64, s8,  load,  OPCODE_I64_LOAD8_S) \
    OP_INST_PARAM1(0x31, i64, u8,  load,  OPCODE_I64_LOAD8_U) \
    OP_INST_PARAM1(0x32, i64, s16, load,  OPCODE_I64_LOAD16_S) \
    OP_INST_PARAM1(0x33, i64, u16, load,  OPCODE_I64_LOAD16_U) \
    OP_INST_PARAM1(0x34, i64, s32, load,  OPCODE_I64_LOAD32_S) \
    OP_INST_PARAM1(0x35, i64, u32, load,  OPCODE_I64_LOAD32_U) \
    OP_INST_PARAM1(0x36, i32, s32, store, OPCODE_I32_STORE) \
    OP_INST_PARAM1(0x37, i64, s32, store, OPCODE_I64_STORE) \
    OP_INST_PARAM1(0x38, f32, s32, store, OPCODE_F32_STORE) \
    OP_INST_PARAM1(0x39, f64, s32, store, OPCODE_F64_STORE) \
    OP_INST_PARAM1(0x3A, i32, s8,  store, OPCODE_I32_STORE8) \
    OP_INST_PARAM1(0x3B, i32, s16, store, OPCODE_I32_STORE16) \
    OP_INST_PARAM1(0x3C, i64, s8,  store, OPCODE_I64_STORE8) \
    OP_INST_PARAM1(0x3D, i64, s16, store, OPCODE_I64_STORE16) \
    OP_INST_PARAM1(0x3E, i64, s32, store, OPCODE_I64_STORE32)

#define MEMORY_OP_EACH(OP_INST_1) \
    OP_INST_1(0x3F, 0x00, any, memory_size, OPCODE_MEMORY_SIZE) \
    OP_INST_1(0x40, 0x00, any, memory_grow, OPCODE_MEMORY_GROW)

/* Constant instructions */
#define CONST_OP_EACH(OP_INST_PARAM1) \
    OP_INST_PARAM1(0x41, i32, i32_const, u32, OPCODE_LOAD_CONST_I32) \
    OP_INST_PARAM1(0x42, i64, i64_const, u64, OPCODE_LOAD_CONST_I64) \
    OP_INST_PARAM1(0x43, f32, f32_const, f32, OPCODE_LOAD_CONST_F32) \
    OP_INST_PARAM1(0x44, f64, f64_const, f64, OPCODE_LOAD_CONST_F64)

#define NUMERIC_INST_EACH(OP_INST_0) \
    /* i32 comparison operator instructions */ \
    OP_INST_0(0x45, i32, eqz, OPCODE_I32_EQZ) \
    OP_INST_0(0x46, i32, eq,  OPCODE_I32_EQ) \
    OP_INST_0(0x47, i32, ne,  OPCODE_I32_NE) \
    OP_INST_0(0x48, i32, lt_s, OPCODE_I32_LT_S) \
    OP_INST_0(0x49, i32, lt_u, OPCODE_I32_LT_U) \
    OP_INST_0(0x4A, i32, gt_s, OPCODE_I32_GT_S) \
    OP_INST_0(0x4B, i32, gt_u, OPCODE_I32_GT_U) \
    OP_INST_0(0x4C, i32, le_s, OPCODE_I32_LE_S) \
    OP_INST_0(0x4D, i32, le_u, OPCODE_I32_LE_U) \
    OP_INST_0(0x4E, i32, ge_s, OPCODE_I32_GE_S) \
    OP_INST_0(0x4F, i32, ge_u, OPCODE_I32_GE_U) \
    /* i64 comparison operator instructions */ \
    OP_INST_0(0x50, i64, eqz,  OPCODE_I64_EQZ) \
    OP_INST_0(0x51, i64, eq,   OPCODE_I64_EQ) \
    OP_INST_0(0x52, i64, ne,   OPCODE_I64_NE) \
    OP_INST_0(0x53, i64, lt_s, OPCODE_I64_LT_S) \
    OP_INST_0(0x54, i64, lt_u, OPCODE_I64_LT_U) \
    OP_INST_0(0x55, i64, gt_s, OPCODE_I64_GT_S) \
    OP_INST_0(0x56, i64, gt_u, OPCODE_I64_GT_U) \
    OP_INST_0(0x57, i64, le_s, OPCODE_I64_LE_S) \
    OP_INST_0(0x58, i64, le_u, OPCODE_I64_LE_U) \
    OP_INST_0(0x59, i64, ge_s, OPCODE_I64_GE_S) \
    OP_INST_0(0x5A, i64, ge_u, OPCODE_I64_GE_U) \
    /* f32 comparison operator instructions */ \
    OP_INST_0(0x5B, f32, eq, OPCODE_F32_EQ) \
    OP_INST_0(0x5C, f32, ne, OPCODE_F32_NE) \
    OP_INST_0(0x5D, f32, lt, OPCODE_F32_LT) \
    OP_INST_0(0x5E, f32, gt, OPCODE_F32_GT) \
    OP_INST_0(0x5F, f32, le, OPCODE_F32_LE) \
    OP_INST_0(0x60, f32, ge, OPCODE_F32_GE) \
    /* f64 comparison operator instructions */ \
    OP_INST_0(0x61, f64, eq, OPCODE_F64_EQ) \
    OP_INST_0(0x62, f64, ne, OPCODE_F64_NE) \
    OP_INST_0(0x63, f64, lt, OPCODE_F64_LT) \
    OP_INST_0(0x64, f64, gt, OPCODE_F64_GT) \
    OP_INST_0(0x65, f64, le, OPCODE_F64_LE) \
    OP_INST_0(0x66, f64, ge, OPCODE_F64_GE) \
    /* i32 basic operator instructions */ \
    OP_INST_0(0x67, i32, clz,    OPCODE_I32_CLZ) \
    OP_INST_0(0x68, i32, ctz,    OPCODE_I32_CTZ) \
    OP_INST_0(0x69, i32, popcnt, OPCODE_I32_POPCNT) \
    OP_INST_0(0x6A, i32, add,    OPCODE_I32_ADD) \
    OP_INST_0(0x6B, i32, sub,    OPCODE_I32_SUB) \
    OP_INST_0(0x6C, i32, mul,    OPCODE_I32_MUL) \
    OP_INST_0(0x6D, i32, div_s,  OPCODE_I32_DIV_S) \
    OP_INST_0(0x6E, i32, div_u,  OPCODE_I32_DIV_U) \
    OP_INST_0(0x6F, i32, rem_s,  OPCODE_I32_REM_S) \
    OP_INST_0(0x70, i32, rem_u,  OPCODE_I32_REM_U) \
    OP_INST_0(0x71, i32, and,    OPCODE_I32_AND) \
    OP_INST_0(0x72, i32, or,     OPCODE_I32_OR) \
    OP_INST_0(0x73, i32, xor,    OPCODE_I32_XOR) \
    OP_INST_0(0x74, i32, shl,    OPCODE_I32_SHL) \
    OP_INST_0(0x75, i32, shr_s,  OPCODE_I32_SHR_S) \
    OP_INST_0(0x76, i32, shr_u,  OPCODE_I32_SHR_U) \
    OP_INST_0(0x77, i32, rotl,   OPCODE_I32_ROTL) \
    OP_INST_0(0x78, i32, rotr,   OPCODE_I32_ROTR) \
    /* i64 basic operator instructions */ \
    OP_INST_0(0x79, i64, clz,    OPCODE_I64_CLZ) \
    OP_INST_0(0x7A, i64, ctz,    OPCODE_I64_CTZ) \
    OP_INST_0(0x7B, i64, popcnt, OPCODE_I64_POPCNT) \
    OP_INST_0(0x7C, i64, add,    OPCODE_I64_ADD) \
    OP_INST_0(0x7D, i64, sub,    OPCODE_I64_SUB) \
    OP_INST_0(0x7E, i64, mul,    OPCODE_I64_MUL) \
    OP_INST_0(0x7F, i64, div_s,  OPCODE_I64_DIV_S) \
    OP_INST_0(0x80, i64, div_u,  OPCODE_I64_DIV_U) \
    OP_INST_0(0x81, i64, rem_s,  OPCODE_I64_REM_S) \
    OP_INST_0(0x82, i64, rem_u,  OPCODE_I64_REM_U) \
    OP_INST_0(0x83, i64, and,    OPCODE_I64_AND) \
    OP_INST_0(0x84, i64, or ,    OPCODE_I64_OR) \
    OP_INST_0(0x85, i64, xor,    OPCODE_I64_XOR) \
    OP_INST_0(0x86, i64, shl,    OPCODE_I64_SHL) \
    OP_INST_0(0x87, i64, shr_s,  OPCODE_I64_SHR_S) \
    OP_INST_0(0x88, i64, shr_u,  OPCODE_I64_SHR_U) \
    OP_INST_0(0x89, i64, rotl,   OPCODE_I64_ROTL) \
    OP_INST_0(0x8A, i64, rotr,   OPCODE_I64_ROTR) \
    /* f32 basic operator instructions */ \
    OP_INST_0(0x8B, f32, abs,      OPCODE_F32_ABS) \
    OP_INST_0(0x8C, f32, neg,      OPCODE_F32_NEG) \
    OP_INST_0(0x8D, f32, ceil,     OPCODE_F32_CEIL) \
    OP_INST_0(0x8E, f32, floor,    OPCODE_F32_FLOOR) \
    OP_INST_0(0x8F, f32, trunc,    OPCODE_F32_TRUNC) \
    OP_INST_0(0x90, f32, nearest,  OPCODE_F32_NEAREST) \
    OP_INST_0(0x91, f32, sqrt,     OPCODE_F32_SQRT) \
    OP_INST_0(0x92, f32, add,      OPCODE_F32_ADD) \
    OP_INST_0(0x93, f32, sub,      OPCODE_F32_SUB) \
    OP_INST_0(0x94, f32, mul,      OPCODE_F32_MUL) \
    OP_INST_0(0x95, f32, div,      OPCODE_F32_DIV) \
    OP_INST_0(0x96, f32, min,      OPCODE_F32_MIN) \
    OP_INST_0(0x97, f32, max,      OPCODE_F32_MAX) \
    OP_INST_0(0x98, f32, copysign, OPCODE_F32_COPYSIGN) \
    /* f64 basic operator instructions */ \
    OP_INST_0(0x99, f64, abs,      OPCODE_F64_ABS) \
    OP_INST_0(0x9A, f64, neg,      OPCODE_F64_NEG) \
    OP_INST_0(0x9B, f64, ceil,     OPCODE_F64_CEIL) \
    OP_INST_0(0x9C, f64, floor,    OPCODE_F64_FLOOR) \
    OP_INST_0(0x9D, f64, trunc,    OPCODE_F64_TRUNC) \
    OP_INST_0(0x9E, f64, nearest,  OPCODE_F64_NEAREST) \
    OP_INST_0(0x9F, f64, sqrt,     OPCODE_F64_SQRT) \
    OP_INST_0(0xA0, f64, add,      OPCODE_F64_ADD) \
    OP_INST_0(0xA1, f64, sub,      OPCODE_F64_SUB) \
    OP_INST_0(0xA2, f64, mul,      OPCODE_F64_MUL) \
    OP_INST_0(0xA3, f64, div,      OPCODE_F64_DIV) \
    OP_INST_0(0xA4, f64, min,      OPCODE_F64_MIN) \
    OP_INST_0(0xA5, f64, max,      OPCODE_F64_MAX) \
    OP_INST_0(0xA6, f64, copysign, OPCODE_F64_COPYSIGN) \
    /* Type cast operator instructions */ \
    OP_INST_0(0xA7, i32, wrap_i64,      OPCODE_WRAP_I64) \
    OP_INST_0(0xA8, i32, trunc_f32_s,   OPCODE_I32_TRUNC_F32_S) \
    OP_INST_0(0xA9, i32, trunc_f32_u,   OPCODE_I32_TRUNC_F32_U) \
    OP_INST_0(0xAA, i32, trunc_f64_s,   OPCODE_I32_TRUNC_F64_S) \
    OP_INST_0(0xAB, i32, trunc_f64_u,   OPCODE_I32_TRUNC_F64_U) \
    OP_INST_0(0xAC, i64, extend_i32_s,  OPCODE_I64_EXTEND_I32_S) \
    OP_INST_0(0xAD, i64, extend_i32_u,  OPCODE_I64_EXTEND_I32_U) \
    OP_INST_0(0xAE, i64, trunc_f32_s,   OPCODE_I64_TRUNC_F32_S) \
    OP_INST_0(0xAF, i64, trunc_f32_u,   OPCODE_I64_TRUNC_F32_U) \
    OP_INST_0(0xB0, i64, trunc_f64_s,   OPCODE_I64_TRUNC_F64_S) \
    OP_INST_0(0xB1, i64, trunc_f64_u,   OPCODE_I64_TRUNC_F64_U) \
    OP_INST_0(0xB2, f32, convert_i32_s, OPCODE_F32_CONVERT_I32_S) \
    OP_INST_0(0xB3, f32, convert_i32_u, OPCODE_F32_CONVERT_I32_U) \
    OP_INST_0(0xB4, f32, convert_i64_s, OPCODE_F32_CONVERT_I64_S) \
    OP_INST_0(0xB5, f32, convert_i64_u, OPCODE_F32_CONVERT_I64_U) \
    OP_INST_0(0xB6, f32, demote_f64,    OPCODE_F32_DEMOTE_F64) \
    OP_INST_0(0xB7, f64, convert_i32_s, OPCODE_F64_CONVERT_I32_S) \
    OP_INST_0(0xB8, f64, convert_i32_u, OPCODE_F64_CONVERT_I32_U) \
    OP_INST_0(0xB9, f64, convert_i64_s, OPCODE_F64_CONVERT_I64_S) \
    OP_INST_0(0xBA, f64, convert_i64_u, OPCODE_F64_CONVERT_I64_U) \
    OP_INST_0(0xBB, f64, promote_f32,   OPCODE_F64_PROMOTE_F32) \
    OP_INST_0(0xBC, i32, reinterpret_f32, OPCODE_I32_REINTERPRET_F32) \
    OP_INST_0(0xBD, i64, reinterpret_f64, OPCODE_I64_REINTERPRET_F64) \
    OP_INST_0(0xBE, f32, reinterpret_i32, OPCODE_F32_REINTERPRET_I32) \
    OP_INST_0(0xBF, f64, reinterpret_i64, OPCODE_F64_REINTERPRET_I64) \
    OP_INST_0(0xC0, i32, extend8_s,  OPCODE_I32_EXTEND8_S) \
    OP_INST_0(0xC1, i32, extend16_s, OPCODE_I32_EXTEND16_S) \
    OP_INST_0(0xC2, i64, extend8_s,  OPCODE_I64_EXTEND8_S) \
    OP_INST_0(0xC3, i64, extend16_s, OPCODE_I64_EXTEND16_S) \
    OP_INST_0(0xC4, i64, extend32_s, OPCODE_I64_EXTEND32_S)

/* Saturating truncation instruction */
#define SATURATING_TRUNCATION_INST_EACH(OP_INST_1) \
    OP_INST_1(0xFC, 0x00, i32, trunc_sat_f32_s, OPCODE_I32_TRUNC_SAT_F32_S) \
    OP_INST_1(0xFC, 0x01, i32, trunc_sat_f32_u, OPCODE_I32_TRUNC_SAT_F32_U) \
    OP_INST_1(0xFC, 0x02, i32, trunc_sat_f64_s, OPCODE_I32_TRUNC_SAT_F64_S) \
    OP_INST_1(0xFC, 0x03, i32, trunc_sat_f64_u, OPCODE_I32_TRUNC_SAT_F64_U) \
    OP_INST_1(0xFC, 0x04, i64, trunc_sat_f32_s, OPCODE_I64_TRUNC_SAT_F32_S) \
    OP_INST_1(0xFC, 0x05, i64, trunc_sat_f32_u, OPCODE_I64_TRUNC_SAT_F32_U) \
    OP_INST_1(0xFC, 0x06, i64, trunc_sat_f64_s, OPCODE_I64_TRUNC_SAT_F64_S) \
    OP_INST_1(0xFC, 0x07, i64, trunc_sat_f64_u, OPCODE_I64_TRUNC_SAT_F64_U)

enum wasmbox_opcode {
#define FUNC4(opcode, type, inst, vmopcode) vmopcode,
    DUMMY_INST_EACH(FUNC4)
    PARAMETRIC_INST_EACH(FUNC4)
    NUMERIC_INST_EACH(FUNC4)
#undef FUNC4
#define FUNC5(opcode, param, type, inst, vmopcode) vmopcode,
    VARIABLE_INST_EACH(FUNC5)
    MEMORY_INST_EACH(FUNC5)
    MEMORY_OP_EACH(FUNC5)
    CONST_OP_EACH(FUNC5)
    SATURATING_TRUNCATION_INST_EACH(FUNC5)
#undef FUNC5
    /**
     * Exist from virtual machine.
     */
    OPCODE_EXIT,
    /**
     * Leaves from currently executing function and resume callee code.
     */
    OPCODE_RETURN,
    OPCODE_MOVE,
    OPCODE_DYNAMIC_CALL,
    OPCODE_STATIC_CALL
};

#define WASMBOX_VM_DEBUG 1
#ifdef WASMBOX_VM_DEBUG
static const char *debug_opcodes[] = {
#define FUNC4(opcode, type, inst, vmopcode) #vmopcode,
    DUMMY_INST_EACH(FUNC4)
    PARAMETRIC_INST_EACH(FUNC4)
    NUMERIC_INST_EACH(FUNC4)
#undef FUNC4
#define FUNC5(opcode, param, type, inst, vmopcode) #vmopcode,
    VARIABLE_INST_EACH(FUNC5)
    MEMORY_INST_EACH(FUNC5)
    MEMORY_OP_EACH(FUNC5)
    CONST_OP_EACH(FUNC5)
    SATURATING_TRUNCATION_INST_EACH(FUNC5)
#undef FUNC5
    "OPCODE_EXIT",
    "OPCODE_RETURN",
    "OPCODE_MOVE",
    "OPCODE_DYNAMIC_CALL",
    "OPCODE_STATIC_CALL"
};
#endif /* WASMBOX_VM_DEBUG */

#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
