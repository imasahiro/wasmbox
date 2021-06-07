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

typedef int (*wasmbox_op_decode_func_t)(wasmbox_input_stream_t *ins, wasm_u8_t op);

typedef struct wasmbox_op_decorder_t {
    wasm_u8_t lower;
    wasm_u8_t upper;
    wasmbox_op_decode_func_t func;
} wasmbox_op_decorder_t;

/* unreachable, nop */
#define DUMMY_INST_EACH(OP_INST_0) \
    OP_INST_0(0x00, any, unreachable) \
    OP_INST_0(0x01, any, nop)

/* Parametric Instructions */
#define PARAMETRIC_INST_EACH(OP_INST_0) \
    OP_INST_0(0x1A, any, drop) \
    OP_INST_0(0x1B, any, select)

/* Variable Instruction */
#define VARIABLE_INST_EACH(OP_INST_PARAM1) \
    OP_INST_PARAM1(0x20, x:localidx, any, local_get) \
    OP_INST_PARAM1(0x21, x:localidx, any, local_set) \
    OP_INST_PARAM1(0x22, x:localidx, any, local_tee) \
    OP_INST_PARAM1(0x23, x:globalidx, any, global_get) \
    OP_INST_PARAM1(0x24, x:globalidx, any, global_set)

/* Memory Instructions */
#define MEMORY_INST_EACH(OP_INST_PARAM1) \
    OP_INST_PARAM1(0x28, m:memarg, i32, load) \
    OP_INST_PARAM1(0x29, m:memarg, i64, load) \
    OP_INST_PARAM1(0x2A, m:memarg, f32, load) \
    OP_INST_PARAM1(0x2B, m:memarg, f64, load) \
    OP_INST_PARAM1(0x2C, m:memarg, i32, load8_s) \
    OP_INST_PARAM1(0x2D, m:memarg, i32, load8_u) \
    OP_INST_PARAM1(0x2E, m:memarg, i32, load16_s) \
    OP_INST_PARAM1(0x2F, m:memarg, i32, load16_u) \
    OP_INST_PARAM1(0x30, m:memarg, i64, load8_s) \
    OP_INST_PARAM1(0x31, m:memarg, i64, load8_u) \
    OP_INST_PARAM1(0x32, m:memarg, i64, load16_s) \
    OP_INST_PARAM1(0x33, m:memarg, i64, load16_u) \
    OP_INST_PARAM1(0x34, m:memarg, i64, load32_s) \
    OP_INST_PARAM1(0x35, m:memarg, i64, load32_u) \
    OP_INST_PARAM1(0x36, m:memarg, i32, store) \
    OP_INST_PARAM1(0x37, m:memarg, i64, store) \
    OP_INST_PARAM1(0x38, m:memarg, f32, store) \
    OP_INST_PARAM1(0x39, m:memarg, f64, store) \
    OP_INST_PARAM1(0x3A, m:memarg, i32, store8) \
    OP_INST_PARAM1(0x3B, m:memarg, i32, store16) \
    OP_INST_PARAM1(0x3C, m:memarg, i64, store8) \
    OP_INST_PARAM1(0x3D, m:memarg, i64, store16) \
    OP_INST_PARAM1(0x3E, m:memarg, i64, store32)

#define MEMORY_OP_EACH(OP_INST_1) \
    OP_INST_1(0x3F, 0x00, any, memory_size) \
    OP_INST_1(0x40, 0x00, any, memory_grow)

/* Constant instructions */
#define CONST_OP_EACH(OP_INST_PARAM1) \
    OP_INST_PARAM1(0x41, i32, i32_const) \
    OP_INST_PARAM1(0x42, i64, i64_const) \
    OP_INST_PARAM1(0x43, f32, f32_const) \
    OP_INST_PARAM1(0x44, f64, f64_const)

#define NUMERIC_INST_EACH(OP_INST_0) \
    /* i32 comparison operator instructions */ \
    OP_INST_0(0x45, i32, eqz) \
    OP_INST_0(0x46, i32, eq) \
    OP_INST_0(0x47, i32, ne) \
    OP_INST_0(0x48, i32, lt_s) \
    OP_INST_0(0x49, i32, lt_u) \
    OP_INST_0(0x4A, i32, gt_s) \
    OP_INST_0(0x4B, i32, gt_u) \
    OP_INST_0(0x4C, i32, le_s) \
    OP_INST_0(0x4D, i32, le_u) \
    OP_INST_0(0x4E, i32, ge_s) \
    OP_INST_0(0x4F, i32, ge_u) \
    /* i64 comparison operator instructions */ \
    OP_INST_0(0x50, i64, eqz) \
    OP_INST_0(0x51, i64, eq) \
    OP_INST_0(0x52, i64, ne) \
    OP_INST_0(0x53, i64, lt_s) \
    OP_INST_0(0x54, i64, lt_u) \
    OP_INST_0(0x55, i64, gt_s) \
    OP_INST_0(0x56, i64, gt_u) \
    OP_INST_0(0x57, i64, le_s) \
    OP_INST_0(0x58, i64, le_u) \
    OP_INST_0(0x59, i64, ge_s) \
    OP_INST_0(0x5A, i64, ge_u) \
    /* f32 comparison operator instructions */ \
    OP_INST_0(0x5B, f32, eq) \
    OP_INST_0(0x5C, f32, ne) \
    OP_INST_0(0x5D, f32, lt) \
    OP_INST_0(0x5E, f32, gt) \
    OP_INST_0(0x5F, f32, le) \
    OP_INST_0(0x60, f32, ge) \
    /* f64 comparison operator instructions */ \
    OP_INST_0(0x61, f64, eq) \
    OP_INST_0(0x62, f64, ne) \
    OP_INST_0(0x63, f64, lt) \
    OP_INST_0(0x64, f64, gt) \
    OP_INST_0(0x65, f64, le) \
    OP_INST_0(0x66, f64, ge) \
    /* i32 basic operator instructions */ \
    OP_INST_0(0x67, i32, clz) \
    OP_INST_0(0x68, i32, ctz) \
    OP_INST_0(0x69, i32, popcnt) \
    OP_INST_0(0x6A, i32, add) \
    OP_INST_0(0x6B, i32, sub) \
    OP_INST_0(0x6C, i32, mul) \
    OP_INST_0(0x6D, i32, div_s) \
    OP_INST_0(0x6E, i32, div_u) \
    OP_INST_0(0x6F, i32, rem_s) \
    OP_INST_0(0x70, i32, rem_u) \
    OP_INST_0(0x71, i32, and) \
    OP_INST_0(0x72, i32, or) \
    OP_INST_0(0x73, i32, xor) \
    OP_INST_0(0x74, i32, shl) \
    OP_INST_0(0x75, i32, shr_s) \
    OP_INST_0(0x76, i32, shr_u) \
    OP_INST_0(0x77, i32, rotl) \
    OP_INST_0(0x78, i32, rotr) \
    /* i64 basic operator instructions */ \
    OP_INST_0(0x79, i64, clz) \
    OP_INST_0(0x7A, i64, ctz) \
    OP_INST_0(0x7B, i64, popcnt) \
    OP_INST_0(0x7C, i64, add) \
    OP_INST_0(0x7D, i64, sub) \
    OP_INST_0(0x7E, i64, mul) \
    OP_INST_0(0x7F, i64, div_s) \
    OP_INST_0(0x80, i64, div_u) \
    OP_INST_0(0x81, i64, rem_s) \
    OP_INST_0(0x82, i64, rem_u) \
    OP_INST_0(0x83, i64, and) \
    OP_INST_0(0x84, i64, or) \
    OP_INST_0(0x85, i64, xor) \
    OP_INST_0(0x86, i64, shl) \
    OP_INST_0(0x87, i64, shr_s) \
    OP_INST_0(0x88, i64, shr_u) \
    OP_INST_0(0x89, i64, rotl) \
    OP_INST_0(0x8A, i64, rotr) \
    /* f32 basic operator instructions */ \
    OP_INST_0(0x8B, f32, abs) \
    OP_INST_0(0x8C, f32, neg) \
    OP_INST_0(0x8D, f32, ceil) \
    OP_INST_0(0x8E, f32, floor) \
    OP_INST_0(0x8F, f32, trunc) \
    OP_INST_0(0x90, f32, nearest) \
    OP_INST_0(0x91, f32, sqrt) \
    OP_INST_0(0x92, f32, add) \
    OP_INST_0(0x93, f32, sub) \
    OP_INST_0(0x94, f32, mul) \
    OP_INST_0(0x95, f32, div) \
    OP_INST_0(0x96, f32, min) \
    OP_INST_0(0x97, f32, max) \
    OP_INST_0(0x98, f32, copysign) \
    /* f64 basic operator instructions */ \
    OP_INST_0(0x99, f64, abs) \
    OP_INST_0(0x9A, f64, neg) \
    OP_INST_0(0x9B, f64, ceil) \
    OP_INST_0(0x9C, f64, floor) \
    OP_INST_0(0x9D, f64, trunc) \
    OP_INST_0(0x9E, f64, nearest) \
    OP_INST_0(0x9F, f64, sqrt) \
    OP_INST_0(0xA0, f64, add) \
    OP_INST_0(0xA1, f64, sub) \
    OP_INST_0(0xA2, f64, mul) \
    OP_INST_0(0xA3, f64, div) \
    OP_INST_0(0xA4, f64, min) \
    OP_INST_0(0xA5, f64, max) \
    OP_INST_0(0xA6, f64, copysign) \
    /* Type cast operator instructions */ \
    OP_INST_0(0xA7, i32, wrap_i64) \
    OP_INST_0(0xA8, i32, trunc_f32_s) \
    OP_INST_0(0xA9, i32, trunc_f32_u) \
    OP_INST_0(0xAA, i32, trunc_f64_s) \
    OP_INST_0(0xAB, i32, trunc_f64_u) \
    OP_INST_0(0xAC, i64, extend_i32_s) \
    OP_INST_0(0xAD, i64, extend_i32_u) \
    OP_INST_0(0xAE, i64, trunc_f32_s) \
    OP_INST_0(0xAF, i64, trunc_f32_s) \
    OP_INST_0(0xB0, i64, trunc_f64_s) \
    OP_INST_0(0xB1, i64, trunc_f64_u) \
    OP_INST_0(0xB2, f32, convert_i32_s) \
    OP_INST_0(0xB3, f32, convert_i32_u) \
    OP_INST_0(0xB4, f32, convert_i64_s) \
    OP_INST_0(0xB5, f32, convert_i64_u) \
    OP_INST_0(0xB6, f32, demote_f64) \
    OP_INST_0(0xB7, f64, convert_i32_s) \
    OP_INST_0(0xB8, f64, convert_i32_u) \
    OP_INST_0(0xB9, f64, convert_i64_s) \
    OP_INST_0(0xBA, f64, convert_i64_u) \
    OP_INST_0(0xBB, f64, promote_f32) \
    OP_INST_0(0xBC, i32, reinterpret_f32) \
    OP_INST_0(0xBD, i64, reinterpret_f64) \
    OP_INST_0(0xBE, f32, reinterpret_i32) \
    OP_INST_0(0xBF, f64, reinterpret_i64) \
    OP_INST_0(0xC0, i32, extend8_s) \
    OP_INST_0(0xC1, i32, extend16_s) \
    OP_INST_0(0xC2, i32, extend8_s) \
    OP_INST_0(0xC3, i32, extend16_s) \
    OP_INST_0(0xC4, i32, extend8_s)

/* Saturating truncation instruction */
#define SATURATING_TRUNCATION_INST_EACH(OP_INST_1) \
    OP_INST_1(0xFC, 0x00, i32, trunc_sat_f32_s) \
    OP_INST_1(0xFC, 0x01, i32, trunc_sat_f32_u) \
    OP_INST_1(0xFC, 0x02, i32, trunc_sat_f64_s) \
    OP_INST_1(0xFC, 0x03, i32, trunc_sat_f64_u) \
    OP_INST_1(0xFC, 0x04, i64, trunc_sat_f32_s) \
    OP_INST_1(0xFC, 0x05, i64, trunc_sat_f32_u) \
    OP_INST_1(0xFC, 0x06, i64, trunc_sat_f64_s) \
    OP_INST_1(0xFC, 0x07, i64, trunc_sat_f64_u)
#ifdef __cplusplus
}
#endif

#endif /* end of include guard */
