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
#include "input-stream.h"
#include "leb128.h"
#include "opcodes.h"
#include "allocator.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define LOG(MSG) fprintf(stderr, "(%s:%d)" MSG, __FILE_NAME__, __LINE__)
static void wasmbox_dump_function(wasmbox_code_t *code, const char *indent);
static void wasmbox_eval_function(wasmbox_module_t *mod, wasmbox_code_t *code, wasmbox_value_t *stack);

/* Module API */
#define MODULE_TYPES_INIT_SIZE 4
static void wasmbox_module_register_new_type(wasmbox_module_t *mod, wasmbox_type_t *func_type)
{
    if (mod->types == NULL) {
        mod->types = (wasmbox_type_t **) wasmbox_malloc(sizeof(mod->types) * MODULE_TYPES_INIT_SIZE);
        mod->type_size = 0;
        mod->type_capacity = MODULE_TYPES_INIT_SIZE;
    }
    if (mod->type_size + 1 == mod->type_capacity) {
        mod->type_capacity *= 2;
        mod->types = (wasmbox_type_t **) realloc(mod->types, sizeof(mod->types) * mod->type_capacity);
    }
    mod->types[mod->type_size++] = func_type;
}

#define MODULE_FUNCTIONS_INIT_SIZE 4
static void wasmbox_module_register_new_function(wasmbox_module_t *mod, wasmbox_function_t *func)
{
    if (mod->functions == NULL) {
        mod->functions = (wasmbox_function_t **) wasmbox_malloc(sizeof(mod->functions) * MODULE_FUNCTIONS_INIT_SIZE);
        mod->function_size = 0;
        mod->function_capacity = MODULE_FUNCTIONS_INIT_SIZE;
    }
    if (mod->function_size + 1 == mod->function_capacity) {
        mod->function_capacity *= 2;
        mod->functions = (wasmbox_function_t **) realloc(mod->functions,
                                                         sizeof(mod->functions) * mod->function_capacity);
    }
    mod->functions[mod->function_size++] = func;
}

static int wasmbox_module_add_memory_page(wasmbox_module_t *mod, wasmbox_limit_t *memory_size)
{
    if (mod->memory_block != NULL) {
        LOG("only one memory block allowed");
        return -1;
    }
    if (memory_size->min > memory_size->max) {
        LOG("not supported");
        return -1;
    }
    wasm_u32_t block_size = WASMBOX_PAGE_SIZE * memory_size->min;
    mod->memory_block = (wasmbox_memory_block_t *) wasmbox_malloc(sizeof(*mod->memory_block) + block_size);
    mod->memory_block_size = memory_size->min;
    mod->memory_block_capacity = memory_size->max;
    return 0;
}

#define MODULE_CODE_INIT_SIZE 4
static void wasmbox_code_add(wasmbox_function_t *func, wasmbox_code_t *code) {
    if (func->code == NULL) {
        func->code = (wasmbox_code_t *) wasmbox_malloc(sizeof(*func->code) * MODULE_CODE_INIT_SIZE);
        func->code_size = 0;
        func->code_capacity = MODULE_CODE_INIT_SIZE;
    }
    if (func->code_size + sizeof(wasmbox_code_t) >= func->code_capacity) {
        func->code_capacity *= 2;
        func->code = (wasmbox_code_t *) realloc(func->code, sizeof(*func->code) * func->code_capacity);
    }
    memcpy(&func->code[func->code_size++], code, sizeof(*code));
}

static void wasmbox_code_add_const(wasmbox_function_t *func, int vmopcode, wasmbox_value_t v) {
    wasmbox_code_t code;
    code.h.opcode = vmopcode;
    code.op0.reg = func->stack_top++;
    code.op1.value = v;
    wasmbox_code_add(func, &code);
}

static void wasmbox_code_add_variable(wasmbox_function_t *func, int vmopcode, wasm_u32_t index) {
    wasmbox_code_t code;
    code.h.opcode = vmopcode;
    code.op0.reg = func->stack_top++;
    if (vmopcode == OPCODE_GLOBAL_GET || vmopcode == OPCODE_GLOBAL_SET) {
        code.op1.index = index;
    } else {
        code.op1.index = WASMBOX_FUNCTION_CALL_OFFSET + index;
    }
    wasmbox_code_add(func, &code);
}

static int wasmbox_code_add_binary_op(wasmbox_function_t *func, int vmopcode) {
    wasm_s32_t current_stack_top = func->stack_top;
    wasmbox_code_t code;
    code.h.opcode = vmopcode;
    code.op0.reg = func->stack_top++;
    code.op1.reg = current_stack_top - 2;
    code.op2.reg = current_stack_top - 1;
    wasmbox_code_add(func, &code);
    return 0;
}

static void wasmbox_code_add_move(wasmbox_function_t *func, wasm_s32_t from, wasm_s32_t to) {
    wasmbox_code_t code;
    code.h.opcode = OPCODE_MOVE;
    code.op0.reg = to;
    code.op1.reg = from;
    wasmbox_code_add(func, &code);
}

static void wasmbox_code_add_load(wasmbox_function_t *func, int vmopcode, wasm_u32_t offset) {
    wasmbox_code_t code;
    code.h.opcode = vmopcode;
    code.op0.reg = func->stack_top++;
    code.op1.index = offset;
    wasmbox_code_add(func, &code);
}

static void wasmbox_code_add_store(wasmbox_function_t *func, int vmopcode, wasm_u32_t offset) {
    wasmbox_code_t code;
    code.h.opcode = vmopcode;
    code.op0.index = offset;
    code.op1.reg = func->stack_top - 1;
    wasmbox_code_add(func, &code);
}

#define TYPE_EACH(FUNC) \
    FUNC(0x7f, WASM_TYPE_I32, I32) \
    FUNC(0x7e, WASM_TYPE_I64, I64) \
    FUNC(0x7d, WASM_TYPE_F32, F32) \
    FUNC(0x7c, WASM_TYPE_F64, F64)
static const char *value_type_to_string(wasmbox_value_type_t type) {
    switch (type) {
#define FUNC(opcode, type_enum, type_name) case type_enum: return #type_name;
        TYPE_EACH(FUNC)
#undef FUNC
        default:
            return "unreachable";
    }
}

static int print_function_type(wasmbox_type_t *func_type) {
    fprintf(stdout, "function-type: (");
    for (wasm_u32_t i = 0; i < func_type->argument_size; i++) {
        if (i != 0) {
            fprintf(stdout, ", ");
        }
        fprintf(stdout, "%s", value_type_to_string(func_type->args[i]));
    }
    fprintf(stdout, ") -> (");
    for (wasm_u32_t i = 0; i < func_type->return_size; i++) {
        if (i != 0) {
            fprintf(stdout, ", ");
        }
        fprintf(stdout, "%s", value_type_to_string(func_type->args[func_type->argument_size + i]));
    }
    fprintf(stdout, ")");
    return 0;
}

static int print_function(wasmbox_function_t *func, wasm_u32_t index) {
    if (func->name != NULL) {
        fprintf(stdout, "function %.*s:", func->name->len, func->name->value);
    } else {
        fprintf(stdout, "function func%u:", index);
    }
    if (func->type) {
        print_function_type(func->type);
    }
    return 0;
}

static int parse_magic(wasmbox_input_stream_t *ins) {
    return wasmbox_input_stream_read_u32(ins) == 0x0061736d /* 00 'a' 's' 'm' */;
}

static int parse_version(wasmbox_input_stream_t *ins) {
    return wasmbox_input_stream_read_u32(ins) == 0x01000000;
}

static int dump_binary(wasmbox_input_stream_t *ins, wasm_u64_t size) {
    for (wasm_u64_t i = 0; i < size; i++) {
        fprintf(stdout, "%02x", ins->data[ins->index + i]);
        if (i % 16 == 15) {
            fprintf(stdout, "\n");
        }
    }
    fprintf(stdout, "\n");
    return 0;
}

static int parse_custom_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    // XXX Current we just skip custom section since it would not affect to other sections.
    dump_binary(ins, section_size);
    ins->index += section_size;
    return 0;
}

static int parse_value_type(wasmbox_input_stream_t *ins, wasmbox_value_type_t *type) {
    wasm_u8_t v = wasmbox_input_stream_read_u8(ins);
    switch (v) {
#define FUNC(opcode, type_enum, type_name) \
    case opcode:                           \
        *type = type_enum;                 \
        return 0;
        TYPE_EACH(FUNC)
#undef FUNC
        default:
            // unreachable
            LOG("unknown type");
            return -1;
    }
}

static int parse_type_vector(wasmbox_input_stream_t *ins, wasm_u32_t len, wasmbox_value_type_t *type) {
    for (wasm_u32_t i = 0; i < len; i++) {
        if (parse_value_type(ins, &type[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static wasmbox_type_t *parse_function_type(wasmbox_input_stream_t *ins, wasmbox_module_t *mod) {
    wasmbox_value_type_t types[16];
    assert(wasmbox_input_stream_read_u8(ins) == 0x60);
    wasm_u32_t args_size = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    assert(args_size < 16);
    if (parse_type_vector(ins, args_size, types) != 0) {
        return NULL;
    }
    wasm_u32_t ret_size = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    assert(ret_size < 16);
    wasmbox_type_t *func_type = (wasmbox_type_t *) wasmbox_malloc(
            sizeof(*func_type) + sizeof(wasmbox_value_type_t *) * (args_size + ret_size));
    func_type->argument_size = args_size;
    func_type->return_size = ret_size;
    for (wasm_u32_t i = 0; i < args_size; ++i) {
        func_type->args[i] = types[i];
    }

    if (parse_type_vector(ins, func_type->return_size, func_type->args + args_size) != 0) {
        return NULL;
    }
    return func_type;
}

/* Control Instructions */
typedef enum wasm_block_type_t {
    WASMBOX_BLOCK_TYPE_NONE = 0,
    WASMBOX_BLOCK_TYPE_VAL = 1,
    WASMBOX_BLOCK_TYPE_INDEX = 2,
} wasm_block_type_t;

typedef struct wasmbox_blocktype_t {
    wasm_block_type_t type;
    // `t` if type is WASMBOX_BLOCK_TYPE_VAL and `x` if type is WASMBOX_BLOCK_TYPE_INDEX.
    union wasmbox_blocktype_value_t {
        wasmbox_value_type_t t;
        wasm_s64_t x;
    } v;
} wasmbox_blocktype_t;

static int parse_blocktype(wasmbox_input_stream_t *ins, wasmbox_blocktype_t *type) {
    wasm_u8_t t = wasmbox_input_stream_peek_u8(ins);
    switch (t) {
        case 0x40:
            wasmbox_input_stream_read_u8(ins);
            type->type = WASMBOX_BLOCK_TYPE_NONE;
            type->v.x = 0;
            return 0;
        case 0x7f:
        case 0x7e:
        case 0x7d:
        case 0x7c:
            type->type = WASMBOX_BLOCK_TYPE_VAL;
            return parse_value_type(ins, &type->v.t);
        default:
            type->type = WASMBOX_BLOCK_TYPE_INDEX;
            type->v.x = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
            return 0;
    }
}

static int parse_instruction(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func);

static int parse_expression(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func)
{
    while (1) {
        wasm_u8_t next = wasmbox_input_stream_peek_u8(ins);
        if (next == 0x0B) {
            wasmbox_input_stream_read_u8(ins);
            break;
        }
        if (parse_instruction(ins, mod, func)) {
            return -1;
        }
    }
    return 0;
}

// INST(0x02 bt:blocktype (in:instr)* 0x0B, block bt in* end)
// INST(0x03 bt:blocktype (in:instr)* 0x0B, loop bt in* end)
static int decode_block(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func, wasm_u8_t op) {
    wasmbox_blocktype_t blocktype;
    if (parse_blocktype(ins, &blocktype)) {
        return -1;
    }
    switch (op) {
        case 0x02:
            fprintf(stdout, "block %d %llu\n", blocktype.type, blocktype.v.x);
            break;
        case 0x03:
            fprintf(stdout, "loop %d %llu\n", blocktype.type, blocktype.v.x);
            break;
        default:
            return -1;
    }
    return parse_expression(ins, mod, func);
}


// INST(0x05, end)
static int decode_block_end(wasmbox_input_stream_t *in, wasmbox_module_t *mod, wasmbox_function_t *func, wasm_u8_t op) {
    wasmbox_code_t code;
    wasmbox_code_add_move(func, func->stack_top - 1, -1);
    code.h.opcode = OPCODE_RETURN;
    wasmbox_code_add(func, &code);
    return 0;
}

// INST(0x04 bt:blocktype (in:instr)* 0x0B, if bt in* end)
// INST(0x04 bt:blocktype (in:instr)* 0x05 (in2:instr)* 0x0B, if bt in1* else in2* end)
static int decode_if(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func, wasm_u8_t op) {
    wasmbox_blocktype_t blocktype;
    if (parse_blocktype(ins, &blocktype)) {
        return -1;
    }
    fprintf(stdout, "if %d %llu\n", blocktype.type, blocktype.v.x);

    while (1) {
        wasm_u8_t next = wasmbox_input_stream_peek_u8(ins);
        if (next == 0x0B) {
            wasmbox_input_stream_read_u8(ins);
            break;
        }
        if (next == 0x05) {
            fprintf(stdout, "else\n");
            wasmbox_input_stream_read_u8(ins);
            continue;
        }
        if (parse_instruction(ins, mod, func)) {
            return -1;
        }
    }
    return 0;
}

// INST(0x0C l:labelidx, br l)
static int decode_br(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t labelidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "br %llu\n", labelidx);
    return 0;
}

// INST(0x0D l:labelidx, br_if l)
static int decode_br_if(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t labelidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "br_if %llu\n", labelidx);
    return 0;
}

// INST(0x0E l:vec(labelidx) lN:labelidx, br_table l* lN)
static int decode_br_table(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func, wasm_u8_t op) {
    fprintf(stdout, "br_table l:(");
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    for (wasm_u64_t i = 0; i < len; i++) {
        wasm_u64_t labelidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
        if (i != 0) {
            fprintf(stdout, ", ");
        }
        fprintf(stdout, "%llu", labelidx);
    }
    wasm_u64_t defaultLabel = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, ") ln:%llu\n", defaultLabel);
    return 0;
}

// INST(0x0F, return)
static int decode_return(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func, wasm_u8_t op) {
    fprintf(stdout, "return\n");
    return 0;
}

// BLOCK_INST(0x11, x:typeidx 0x00, call_indirect x)
static int decode_call(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t funcidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    wasmbox_function_t *call = mod->functions[funcidx];
    if (call == NULL) {
        LOG("Failed to find function\n");
        return -1;
    }
    wasm_u16_t argument_from = func->stack_top - call->type->argument_size;
    wasm_u16_t argument_to = func->stack_top + call->type->return_size + WASMBOX_FUNCTION_CALL_OFFSET;
    for (int i = 0; i < call->type->argument_size; ++i) {
        wasmbox_code_add_move(func, argument_from - i, argument_to + i);
    }
    wasmbox_code_t code;
    code.h.opcode = OPCODE_STATIC_CALL;
    code.op0.reg = func->stack_top;
    func->stack_top += call->type->return_size;
    code.op1.func = mod->functions[funcidx];
    wasmbox_code_add(func, &code);
    return 0;
}

// INST(0x11 x:typeidx 0x00, call_indirect x)
static int decode_call_indirect(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                                wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t typeidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "call_indirect %llu\n", typeidx);
    return 0;
}


static int decode_variable_inst(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                                wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t idx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    switch (op) {
        case 0x20: // local.get
            wasmbox_code_add_move(func, WASMBOX_FUNCTION_CALL_OFFSET + idx, func->stack_top++);
            return 0;
        case 0x21: // local.set
            wasmbox_code_add_move(func, func->stack_top - 1, WASMBOX_FUNCTION_CALL_OFFSET + idx);
            return 0;
        case 0x22: // local.tee
            wasmbox_code_add_variable(func, OPCODE_LOCAL_TEE, idx);
            return 0;
        case 0x23: // global.get
            wasmbox_code_add_variable(func, OPCODE_GLOBAL_GET, idx);
            return 0;
        case 0x24: // global.set
            wasmbox_code_add_variable(func, OPCODE_GLOBAL_SET, idx);
            return 0;
        default:
            return -1;
    }
    return -1;
}

static int parse_memarg(wasmbox_input_stream_t *ins, wasm_u32_t *align, wasm_u32_t *offset) {
    *align = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    *offset = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    return 0;
}

static int decode_memory_inst(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                              wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u32_t align;
    wasm_u32_t offset;
    if (parse_memarg(ins, &align, &offset)) {
        return -1;
    }
    switch (op) {
#define FUNC(opcode, out_type, in_type, inst, vmopcode) case (opcode): {                       \
    fprintf(stdout, "" #out_type #inst #in_type "(align:%u, offset:%u)\n", align, offset); \
    wasmbox_code_add_##inst(func, vmopcode, offset);                                            \
    break; \
}
            MEMORY_INST_EACH(FUNC)
#undef FUNC
        default:
            return -1;
    }
    return 0;
}

static int decode_memory_size_and_grow(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                                       wasmbox_function_t *func, wasm_u8_t op) {
    if (wasmbox_input_stream_read_u8(ins) != 0x00) {
        return -1;
    }
    wasmbox_code_t code;
    switch (op) {
        case 0x3F: // memory.size
            code.h.opcode = OPCODE_MEMORY_SIZE;
            break;
        case 0x40: // memory.grow
            code.h.opcode = OPCODE_MEMORY_SIZE;
            code.op1.reg = func->stack_top - 1;
            break;
        default:
            return -1;
    }
    code.op0.reg = func->stack_top++;
    wasmbox_code_add(func, &code);
    return 0;
}

static void parse_i32_const(wasmbox_input_stream_t *ins, wasmbox_value_t *v)
{
    (*v).s32 = wasmbox_parse_signed_leb128(ins->data + ins->index, &ins->index, ins->length);
}

static void parse_i64_const(wasmbox_input_stream_t *ins, wasmbox_value_t *v)
{
    (*v).s64 = wasmbox_parse_signed_leb128(ins->data + ins->index, &ins->index, ins->length);
}

static void parse_f32_const(wasmbox_input_stream_t *ins, wasmbox_value_t *v)
{
    (*v).f32 = *(wasm_f32_t *) (ins->data + ins->index);
    ins->index += sizeof(wasm_f32_t);
}

static void parse_f64_const(wasmbox_input_stream_t *ins, wasmbox_value_t *v)
{
    (*v).f64 = *(wasm_f64_t *) (ins->data + ins->index);
    ins->index += sizeof(wasm_f64_t);
}

/* Numeric Instructions */
static int decode_constant_inst(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                                wasmbox_function_t *func, wasm_u8_t op) {
    wasmbox_value_t v;
    switch (op) {
#define FUNC(opcode, type, inst, attr, vmopcode) case (opcode): { \
    parse_##inst(ins, &v);                                        \
    wasmbox_code_add_const(func, vmopcode, v);                    \
    return 0; \
}
        CONST_OP_EACH(FUNC)
#undef FUNC
        default:
            return -1;
    }
    return -1;
}

static int decode_op0_inst(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func, wasm_u8_t op) {
    switch (op) {
#define FUNC(opcode, type, inst, vmopcode) case (opcode): { \
    fprintf(stdout, "" #type "." # inst "\n"); \
    return 0; \
}
        DUMMY_INST_EACH(FUNC)
        PARAMETRIC_INST_EACH(FUNC)
#undef FUNC

#define FUNC(op, type, inst, vmopcode) case (op): return wasmbox_code_add_binary_op(func, vmopcode);
        NUMERIC_INST_EACH(FUNC)
#undef FUNC
        default:
            return -1;
    }
    return -1;
}

static int decode_truncation_inst(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                                  wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u8_t op1 = wasmbox_input_stream_read_u8(ins);
    switch (op1) {
#define FUNC(opcode0, opcode1, type, inst, vmopcode) case opcode1: { \
    fprintf(stdout, "" #type "." # inst "\n"); \
    return 0; \
}
        SATURATING_TRUNCATION_INST_EACH(FUNC)
#undef FUNC
        default:
            return -1;
    }
    return -1;
}

static const wasmbox_op_decorder_t decoders[] = {
        {0x00, 0x01, decode_op0_inst},
        {0x02, 0x03, decode_block},
        {0x04, 0x04, decode_if},
        {0x0B, 0x0B, decode_block_end},
        {0x0C, 0x0C, decode_br},
        {0x0D, 0x0D, decode_br_if},
        {0x0E, 0x0E, decode_br_table},
        {0x0F, 0x0F, decode_return},
        {0x10, 0x10, decode_call},
        {0x11, 0x11, decode_call_indirect},
        {0x1A, 0x1B, decode_op0_inst},
        {0x20, 0x24, decode_variable_inst},
        {0x28, 0x3E, decode_memory_inst},
        {0x3F, 0x40, decode_memory_size_and_grow},
        {0x41, 0x44, decode_constant_inst},
        {0x45, 0xC4, decode_op0_inst},
        {0xFC, 0xFC, decode_truncation_inst}
};

static int parse_instruction(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasmbox_function_t *func) {
    wasm_u8_t op = wasmbox_input_stream_read_u8(ins);
    for (int i = 0; i < sizeof(decoders) / sizeof(decoders[0]); i++) {
        const wasmbox_op_decorder_t d = decoders[i];
        if (d.lower <= op && op <= d.upper) {
            return d.func(ins, mod, func, op);
        }
    }
    return -1;
}

static int parse_code(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                      wasmbox_function_t *func, wasm_u64_t codelen) {
    wasm_u64_t end = ins->index + codelen;
    while (ins->index < end) {
        if (parse_instruction(ins, mod, func)) {
            return -1;
        }
    }
    return 0;
}

static int parse_local_variable(wasmbox_input_stream_t *ins, wasm_u64_t *index, wasmbox_value_type_t *valtype) {
    *index = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    return parse_value_type(ins, valtype);
}

static int parse_local_variables(wasmbox_input_stream_t *ins, wasmbox_function_t *func) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    for (wasm_u64_t i = 0; i < len; i++) {
        wasm_u64_t localidx;
        wasmbox_value_type_t type;
        if (parse_local_variable(ins, &localidx, &type)) {
            return -1;
        }
        fprintf(stdout, "%llu: local%llu(type=%d)\n", i, localidx, type);
        func->locals += localidx;
    }
    func->stack_top += func->locals;
    return 0;
}

static int parse_function(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasm_u32_t funcindex) {
    wasmbox_function_t *func = mod->functions[funcindex];
    wasm_u64_t size = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    wasm_u64_t index = ins->index;
    fprintf(stdout, "code(size:%llu)\n", size);
    dump_binary(ins, size);
    if (parse_local_variables(ins, func)) {
        return -1;
    }
    size -= ins->index - index;
    return parse_code(ins, mod, func, size);
}

static int parse_type_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    for (wasm_u64_t i = 0; i < len; i++) {
        wasmbox_type_t *func_type = NULL;
        if ((func_type = parse_function_type(ins, mod)) == NULL) {
            return -1;
        }
        wasmbox_module_register_new_type(mod, func_type);
    }
    return 0;
}

static int parse_name(wasmbox_input_stream_t *ins, wasmbox_name_t **name) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    wasmbox_name_t *n = (wasmbox_name_t *) wasmbox_malloc(sizeof(wasmbox_name_t) + len);
    n->len = len;
    for (wasm_u64_t i = 0; i < len; i++) {
        n->value[i] = wasmbox_input_stream_read_u8(ins);
    }
    *name = n;
    return 0;
}

static int parse_limit(wasmbox_input_stream_t *ins, wasmbox_limit_t *limit) {
    wasm_u8_t has_upper_limit = wasmbox_input_stream_read_u8(ins);
    limit->min = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    if (has_upper_limit) {
        limit->max = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    } else {
        limit->max = WASM_U32_MAX;
    }
    return 0;
}

static int parse_import_description(wasmbox_input_stream_t *ins, wasmbox_module_t *mod) {
    wasm_u8_t type = wasmbox_input_stream_read_u8(ins);
    wasm_u32_t v;
    wasmbox_limit_t limit;
    wasmbox_value_type_t value_type;
    switch (type) {
        case 0x00: // func x:typeidx
            v = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
            return 0;
        case 0x01: // table x:tabletype
            assert(wasmbox_input_stream_read_u8(ins) == 0x70);
            return parse_limit(ins, &limit);
        case 0x02: // mem x:memtype
            return parse_limit(ins, &limit);
        case 0x03: // global x:globaltype
            if (parse_value_type(ins, &value_type)) {
                return -1;
            }
            type = wasmbox_input_stream_read_u8(ins);
            assert(type == 0x00 /* const */ || type == 0x01 /* var */ || type == 0x02 /* mutable */);
            return 0;
        default:
            return -1;
    }
}

static int parse_import(wasmbox_input_stream_t *ins, wasmbox_module_t *mod) {
    wasmbox_name_t *module_name;
    wasmbox_name_t *ns_name;
    if (parse_name(ins, &module_name)) {
        return -1;
    }
    if (parse_name(ins, &ns_name)) {
        return -1;
    }
    if (parse_import_description(ins, mod)) {
        return -1;
    }
    fprintf(stdout, "import(%.*s:%.*s)\n", module_name->len, module_name->value, ns_name->len, ns_name->value);
    wasmbox_free(module_name);
    wasmbox_free(ns_name);
    return 0;
}

static int parse_import_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "import(num:%llu)\n", len);
    for (wasm_u64_t i = 0; i < len; i++) {
        if (parse_import(ins, mod)) {
            return -1;
        }
    }
    return 0;
}

static int parse_function_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    for (wasm_u64_t i = 0; i < len; i++) {
        wasm_u32_t v = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
        wasmbox_function_t *func = (wasmbox_function_t *) wasmbox_malloc(sizeof(*func));
        func->type = mod->types[v];
        func->stack_top = WASMBOX_FUNCTION_CALL_OFFSET + func->type->argument_size;
        wasmbox_module_register_new_function(mod, func);
    }
    return 0;
}

static int parse_table_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    fprintf(stdout, "table\n");
    dump_binary(ins, section_size);
    ins->index += section_size;
    return 0;
}

static int parse_memory_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "memory (num:%llu)\n", len);
    for (wasm_u64_t i = 0; i < len; i++) {
        wasmbox_limit_t limit;
        if (parse_limit(ins, &limit) != 0) {
            return -1;
        }
        if (wasmbox_module_add_memory_page(mod, &limit) < 0) {
            return -1;
        }
    }
    return 0;
}

static int parse_global_variable(wasmbox_input_stream_t *ins, wasmbox_module_t *mod) {
    wasmbox_value_type_t valtype;
    if (parse_value_type(ins, &valtype) != 0) {
        return -1;
    }
    wasm_u8_t mut = wasmbox_input_stream_read_u8(ins);
    switch (mut) {
        case 0x00: // const
            break;
        case 0x01: // var
            break;
        default:
            LOG("unreachable");
            return -1;
    }
    int is_const = mut == 0x01;
    if (0) {
        fprintf(stdout, "- %s type:%s\n", is_const ? "const" : "var", value_type_to_string(valtype));
    }
    wasmbox_function_t *global = mod->global_function;
    if (global == NULL) {
        global = mod->global_function = (wasmbox_function_t *) wasmbox_malloc(sizeof(*mod->global_function));
        wasm_u32_t len = strlen("__global__");
        global->name = (wasmbox_name_t *) wasmbox_malloc(sizeof(*global->name) + len);
        global->name->len = len;
        memcpy(global->name->value, "__global__", len);
    }
    // We already compiled another global section. Erase last EXIT op to merge a global function to previous one.
    if (global->code != NULL && global->code[global->code_size - 1].h.opcode == OPCODE_EXIT) {
        global->code_size -= 1;
    }
    if (parse_expression(ins, mod, global) < 0) {
        return -1;
    }
    wasmbox_code_t code;
    code.h.opcode = OPCODE_EXIT;
    wasmbox_code_add(global, &code);
    return 0;
}

static int parse_global_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    if (0) {
        fprintf(stdout, "global (num:%llu)\n", len);
    }
    if (len > 0) {
        mod->globals = wasmbox_malloc(sizeof(*mod->globals) * len);
    }
    for (wasm_u64_t i = 0; i < len; i++) {
        if (parse_global_variable(ins, mod) != 0) {
            return -1;
        }
    }
    return 0;
}

static int parse_export_entry(wasmbox_input_stream_t *ins, wasmbox_module_t *mod) {
    wasmbox_name_t *name;
    if (parse_name(ins, &name)) {
        return -1;
    }
    wasm_u8_t type = wasmbox_input_stream_read_u8(ins);
    const char *debug_name;
    switch (type) {
        case 0x00: // function
            debug_name = "func";
            break;
        case 0x01: // table
            debug_name = "table";
            break;
        case 0x02: // memory
            debug_name = "memory";
            break;
        case 0x03: // global
            debug_name = "global";
            break;
        default:
            LOG("unreachable");
            return -1;
    }
    wasm_u32_t index = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    if (type != 0x00) {
        fprintf(stdout, "- export '%.*s' %s(%d)\n", name->len, name->value, debug_name, index);
        wasmbox_free(name);
    } else {
        mod->functions[index]->name = name;
    }
    return 0;
}

static int parse_export_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "export (num:%llu)\n", len);
    for (wasm_u64_t i = 0; i < len; i++) {
        if (parse_export_entry(ins, mod) != 0) {
            return -1;
        }
    }
    return 0;
}

static int parse_start_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    fprintf(stdout, "start\n");
    dump_binary(ins, section_size);
    ins->index += section_size;
    return 0;
}

static int parse_element_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    fprintf(stdout, "element\n");
    dump_binary(ins, section_size);
    ins->index += section_size;
    return 0;
}

static int parse_code_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    wasm_u32_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    for (wasm_u32_t i = 0; i < len; i++) {
        if (parse_function(ins, mod, i)) {
            return -1;
        }
    }
    return 0;
}

static int parse_data(wasmbox_input_stream_t *ins, wasmbox_module_t *mod) {
    wasm_u8_t type = wasmbox_input_stream_read_u8(ins);
    wasm_u32_t index = 0;
    wasm_u32_t len = 0;
    assert(mod->memory_block_size > 0);
    wasmbox_function_t func = {};
    wasmbox_code_t code;
    code.h.opcode = OPCODE_EXIT;
    wasmbox_value_t stack[8];
    wasm_u32_t offset = 0;
    switch (type) {
        case 0x02: // active with memory index
            index = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
            assert(index == 0);
            /* fallthrough */
        case 0x00: // active without memory index
            if (parse_expression(ins, mod, &func) < 0) {
                return -1;
            }
            wasmbox_code_add_move(&func, func.stack_top - 1, -1);
            wasmbox_code_add(&func, &code);
            wasmbox_eval_function(mod, func.code, stack + 1);
            offset = stack[0].u32;
            /* fallthrough */
        case 0x01: // passive
            len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
            memcpy(mod->memory_block->data + offset, ins->data + ins->index, len);
            ins->index += len;
            break;
        default:
            return -1;
    }
    if (func.code_size > 0) {
        wasmbox_free(func.code);
    }
    return 0;
}

static int parse_data_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    wasm_u32_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    for (wasm_u32_t i = 0; i < len; i++) {
        if (parse_data(ins, mod)) {
            return -1;
        }
    }
    return 0;
}

typedef int (*section_parse_func_t)(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod);

struct section_parser {
    const char *name;
    section_parse_func_t func;
};

static const struct section_parser section_parser[] = {
        { "custom", parse_custom_section },
        { "type", parse_type_section },
        { "import", parse_import_section },
        { "function", parse_function_section },
        { "table", parse_table_section },
        { "memory", parse_memory_section },
        { "global", parse_global_section },
        { "export", parse_export_section },
        { "start", parse_start_section },
        { "element", parse_element_section },
        { "code", parse_code_section },
        { "data", parse_data_section },
};

static int parse_section(wasmbox_input_stream_t *ins, wasmbox_module_t *mod) {
    wasm_u8_t section_type = wasmbox_input_stream_read_u8(ins);
    assert(0 <= section_type && section_type <= 11);
    wasm_u64_t section_size = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "type=%d(%s), section_size=%llu\n", section_type, section_parser[section_type].name, section_size);
    return section_parser[section_type].func(ins, section_size, mod);
}

static int parse_module(wasmbox_input_stream_t *ins, wasmbox_module_t *module) {
    if (parse_magic(ins) != 0) {
        LOG("Invalid magic number");
        return -1;
    }
    if (parse_version(ins) != 0) {
        LOG("Invalid version number");
        return -1;
    }
    while (!wasmbox_input_stream_is_end_of_stream(ins)) {
        if (parse_section(ins, module) != 0) {
            return -1;
        }
    }
    return 0;
}

#define NOT_IMPLEMENTED() do { \
    LOG("not-implemented");    \
    return;                    \
} while (0)

static void dump_stack(wasmbox_value_t *stack);

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

static void wasmbox_eval_function(wasmbox_module_t *mod, wasmbox_code_t *code, wasmbox_value_t *stack)
{
    while (1) {
        dump_stack(stack);
        fprintf(stdout, "op:%s (%p)\n", debug_opcodes[code->h.opcode], code);
        switch (code->h.opcode) {
            case OPCODE_UNREACHABLE:
                fprintf(stdout, "unreachable\n");
                exit(-1);
                break;
            case OPCODE_NOP:
                fprintf(stdout, "nop\n");
                break;
            case OPCODE_DROP:
                NOT_IMPLEMENTED();
            case OPCODE_SELECT:
                NOT_IMPLEMENTED();
            case OPCODE_EXIT:
                fprintf(stdout, "exit\n");
                return;
            case OPCODE_RETURN:
                fprintf(stdout, "return; stack:%p->%p (diff:%d), code:%p->%p\n",
                        stack, (wasmbox_value_t *) stack[0].u64,
                        (int) (stack - (wasmbox_value_t *) (uintptr_t) stack[0].u64),
                        code, (wasmbox_code_t *) stack[1].u64);
                code = (wasmbox_code_t *) stack[1].u64;
                stack = (wasmbox_value_t *) stack[0].u64;
                break;
            case OPCODE_MOVE:
                fprintf(stdout, "stack[%d].u64= stack[%d].u64\n", code->op0.reg, code->op1.reg);
                stack[code->op0.reg].u64 = stack[code->op1.reg].u64;
                code++;
                break;
            case OPCODE_DYNAMIC_CALL: {
                fprintf(stdout, "stack[%d].u64= func%u()\n", code->op0.reg, code->op1.index);
                wasmbox_function_t *func = mod->functions[code->op1.index];
                code->h.opcode = OPCODE_STATIC_CALL;
                code->op1.func = func;
                /* Re-execute */
                break;
            }
            case OPCODE_STATIC_CALL: {
                fprintf(stdout, "stack[%d].u64= func%p()\n", code->op0.reg, code->op1.func);
                wasmbox_function_t *func = code->op1.func;
                wasmbox_value_t *stack_top = &stack[code->op0.reg] + func->type->return_size;
                stack_top[0].u64 = (wasm_u64_t) (uintptr_t) stack;
                stack_top[1].u64 = (wasm_u64_t) (uintptr_t) (code + 1);
                fprintf(stdout, "call stack:%p->%p, code:%p->%p\n",stack, stack_top + WASMBOX_FUNCTION_CALL_OFFSET,
                        code, func->code);
                stack = stack_top;
                code = func->code;
                break;
            }
            case OPCODE_LOCAL_GET:
                fprintf(stdout, "stack[%d].u64 = stack[%d].u64(%llu)\n",
                        code->op0.reg, code->op1.reg, stack[code->op1.reg].u64);
                stack[code->op0.reg].u64 = stack[code->op1.reg].u64;
                code++;
                break;
            case OPCODE_LOCAL_TEE:
                NOT_IMPLEMENTED();
            case OPCODE_GLOBAL_GET:
                fprintf(stdout, "stack[%d].u64= global[%d].u64\n", code->op0.reg, code->op1.reg);
                stack[code->op0.reg].u64 = mod->globals[code->op1.reg].u64;
                code++;
                break;
            case OPCODE_GLOBAL_SET:
                fprintf(stdout, "global[%d].u64= stack[%d].u64\n", code->op0.reg, code->op1.reg);
                mod->globals[code->op0.reg].u64 = stack[code->op1.reg].u64;
                code++;
                break;

#define LOAD_OP(itype, otype, out_type) do { \
    fprintf(stdout, "stack[%d]." # otype " = (" #out_type ") *(" #itype " *) &memory[%d]\n",     \
            code->op0.reg, code->op1.index);                                                     \
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
#define STORE_OP(itype, otype, out_type) do { \
    fprintf(stdout, "*(" #otype " *) &memory[%d] = (" #otype ") = stack[%d]." # itype "\n",         \
            code->op0.index, code->op1.reg);                                                        \
    *(out_type *)&mod->memory_block->data[code->op0.index] = (out_type) stack[code->op1.reg].itype; \
    code++;                                                                                         \
} while (0)
            case OPCODE_I32_STORE:
                STORE_OP(u32, u32, wasm_u32_t);
                break;
            case OPCODE_I64_STORE:
                STORE_OP(u64, u64, wasm_u64_t);
                break;
            case OPCODE_F32_STORE:
                STORE_OP(f32, f32, wasm_f32_t);
                break;
            case OPCODE_F64_STORE:
                STORE_OP(f64, f64, wasm_f64_t);
                break;
            case OPCODE_I32_STORE8:
                STORE_OP(u8, u32, wasm_u32_t);
                break;
            case OPCODE_I32_STORE16:
                STORE_OP(u16, u32, wasm_u32_t);
                break;
            case OPCODE_I64_STORE8:
                STORE_OP(u8, u64, wasm_u64_t);
                break;
            case OPCODE_I64_STORE16:
                STORE_OP(u16, u64, wasm_u64_t);
                break;
            case OPCODE_I64_STORE32:
                STORE_OP(u32, u64, wasm_u64_t);
                break;
            case OPCODE_MEMORY_SIZE:
                fprintf(stdout, "stack[%d].u32 = memory.size\n", code->op0.reg);
                stack[code->op0.reg].u32 = wasmbox_runtime_memory_size(mod);
                code++;
                break;
            case OPCODE_MEMORY_GROW:
                fprintf(stdout, "stack[%d].u32 = memory.grow(stack[%d].u32)\n", code->op0.reg, code->op1.reg);
                stack[code->op0.reg].u32 = wasmbox_runtime_memory_grow(mod, stack[code->op1.reg].u32);
                code++;
                break;
#define LOAD_CONST_OP(type, formatter) do { \
    fprintf(stdout, "stack[%d]." # type "= " formatter "\n", code->op0.reg, code->op1.value.type); \
    stack[code->op0.reg].type = code->op1.value.type;                                              \
    code++;                                                                                        \
} while (0)
            case OPCODE_LOAD_CONST_I32:
                LOAD_CONST_OP(u32, "%d");
                break;
            case OPCODE_LOAD_CONST_I64:
                LOAD_CONST_OP(u64, "%lld");
                break;
            case OPCODE_LOAD_CONST_F32:
                LOAD_CONST_OP(f32, "%f");
                break;
            case OPCODE_LOAD_CONST_F64:
                LOAD_CONST_OP(f64, "%g");
                break;
#define ARITHMETIC_OP(arg_type, operand, operand_str) \
    ARITHMETIC_OP2(arg_type, arg_type, operand, operand_str, "%d", "%d")

#define ARITHMETIC_OP2(arg_type, ret_type, operand, operand_str, arg_fmt, ret_fmt) do { \
    fprintf(stdout, "stack[%d]." # ret_type " = stack[%d]." # arg_type "(" arg_fmt ") " operand_str \
            " stack[%d]." #arg_type "(" arg_fmt ")\n", \
            code->op0.reg, code->op1.reg, stack[code->op1.reg].arg_type,                                             \
            code->op2.reg, stack[code->op2.reg].arg_type);                                               \
    stack[code->op0.reg].ret_type = stack[code->op1.reg].arg_type operand stack[code->op2.reg].arg_type;            \
    code++;                                                                                             \
} while (0)
            case OPCODE_I32_EQZ:
                fprintf(stdout, "stack[%d].u32= stack[%d].u32 == 0\n",code->op0.reg, code->op1.reg);
                stack[code->op0.reg].u32 = stack[code->op1.reg].u32 == 0;
                code++;
                break;
            case OPCODE_I32_EQ:
                ARITHMETIC_OP(s32, ==, "==");
                break;
            case OPCODE_I32_NE:
                ARITHMETIC_OP(s32, !=, "!=");
                break;
            case OPCODE_I32_LT_S:
                ARITHMETIC_OP(s32, <, "<");
                break;
            case OPCODE_I32_LT_U:
                ARITHMETIC_OP(u32, <, "<");
                break;
            case OPCODE_I32_GT_S:
                ARITHMETIC_OP(s32, >, ">");
                break;
            case OPCODE_I32_GT_U:
                ARITHMETIC_OP(u32, >, ">");
                break;
            case OPCODE_I32_LE_S:
                ARITHMETIC_OP(s32, <=, "<=");
                break;
            case OPCODE_I32_LE_U:
                ARITHMETIC_OP(u32, <=, "<=");
                break;
            case OPCODE_I32_GE_S:
                ARITHMETIC_OP(s32, >=, ">=");
                break;
            case OPCODE_I32_GE_U:
                ARITHMETIC_OP(u32, >=, ">=");
                break;
            case OPCODE_I64_EQZ:
                fprintf(stdout, "stack[%d].u64 = stack[%d].u64 == 0\n",code->op0.reg, code->op1.reg);
                stack[code->op0.reg].u64 = stack[code->op1.reg].u64 == 0;
                code++;
                break;
            case OPCODE_I64_EQ:
                ARITHMETIC_OP2(u64, s32, ==, "==", "%llu", "%d");
                break;
            case OPCODE_I64_NE:
                ARITHMETIC_OP2(u64, s32, !=, "!=", "%llu", "%d");
                break;
            case OPCODE_I64_LT_S:
                ARITHMETIC_OP2(s64, s32, <, "<", "%lld", "%d");
                break;
            case OPCODE_I64_LT_U:
                ARITHMETIC_OP2(u64, s32, <, "<", "%lld", "%d");
                break;
            case OPCODE_I64_GT_S:
                ARITHMETIC_OP2(s64, s32, >, ">", "%lld", "%d");
                break;
            case OPCODE_I64_GT_U:
                ARITHMETIC_OP2(u64, s32, >, ">", "%lld", "%d");
                break;
            case OPCODE_I64_LE_S:
                ARITHMETIC_OP2(s64, s32, <=, "<=", "%lld", "%d");
                break;
            case OPCODE_I64_LE_U:
                ARITHMETIC_OP2(u64, s32, <=, "<=", "%lld", "%d");
                break;
            case OPCODE_I64_GE_S:
                ARITHMETIC_OP2(s64, s32, >=, ">=", "%lld", "%d");
                break;
            case OPCODE_I64_GE_U:
                ARITHMETIC_OP2(u64, s32, >=, ">=", "%lld", "%d");
                break;
            case OPCODE_F32_EQ:
                ARITHMETIC_OP2(f32, s32, ==, "==", "%f", "%d");
                break;
            case OPCODE_F32_NE:
                ARITHMETIC_OP2(f32, s32, !=, "!=", "%f", "%d");
                break;
            case OPCODE_F32_LT:
                ARITHMETIC_OP2(f32, s32, <, "<", "%f", "%d");
                break;
            case OPCODE_F32_GT:
                ARITHMETIC_OP2(f32, s32, >, ">", "%f", "%d");
                break;
            case OPCODE_F32_LE:
                ARITHMETIC_OP2(f32, s32, <=, "<=", "%f", "%d");
                break;
            case OPCODE_F32_GE:
                ARITHMETIC_OP2(f32, s32, >=, ">=", "%f", "%d");
                break;
            case OPCODE_F64_EQ:
                ARITHMETIC_OP2(f64, s32, ==, "==", "%f", "%d");
                break;
            case OPCODE_F64_NE:
                ARITHMETIC_OP2(f64, s32, !=, "!=", "%f", "%d");
                break;
            case OPCODE_F64_LT:
                ARITHMETIC_OP2(f64, s32, <, "<", "%f", "%d");
                break;
            case OPCODE_F64_GT:
                ARITHMETIC_OP2(f64, s32, >, ">", "%f", "%d");
                break;
            case OPCODE_F64_LE:
                ARITHMETIC_OP2(f64, s32, <=, "<=", "%f", "%d");
                break;
            case OPCODE_F64_GE:
                ARITHMETIC_OP2(f64, s32, >=, ">=", "%f", "%d");
                break;
            case OPCODE_I32_CLZ:
                NOT_IMPLEMENTED();
            case OPCODE_I32_CTZ:
                NOT_IMPLEMENTED();
            case OPCODE_I32_POPCNT:
                NOT_IMPLEMENTED();
            case OPCODE_I32_ADD:
                ARITHMETIC_OP(u32, +, "+");
                break;
            case OPCODE_I32_SUB:
                ARITHMETIC_OP(u32, -, "-");
                break;
            case OPCODE_I32_MUL:
                ARITHMETIC_OP(u32, *, "*");
                break;
            case OPCODE_I32_DIV_S:
                ARITHMETIC_OP(s32, /, "/");
                break;
            case OPCODE_I32_DIV_U:
                ARITHMETIC_OP(u32, /, "/");
                break;
            case OPCODE_I32_REM_S:
                ARITHMETIC_OP(s32, %, "%%");
                break;
            case OPCODE_I32_REM_U:
                ARITHMETIC_OP(u32, %, "%%");
                break;
            case OPCODE_I32_AND:
                ARITHMETIC_OP(u32, &, "&");
                break;
            case OPCODE_I32_OR:
                ARITHMETIC_OP(u32, |, "|");
                break;
            case OPCODE_I32_XOR:
                ARITHMETIC_OP(u32, ^, "^");
                break;
            case OPCODE_I32_SHL:
                ARITHMETIC_OP(u32, <<, "<<");
                break;
            case OPCODE_I32_SHR_S:
                ARITHMETIC_OP(s32, >>, ">>");
                break;
            case OPCODE_I32_SHR_U:
                ARITHMETIC_OP(u32, >>, ">>");
                break;
            case OPCODE_I32_ROTL: NOT_IMPLEMENTED();
            case OPCODE_I32_ROTR: NOT_IMPLEMENTED();
            case OPCODE_I64_CLZ: NOT_IMPLEMENTED();
            case OPCODE_I64_CTZ: NOT_IMPLEMENTED();
            case OPCODE_I64_POPCNT: NOT_IMPLEMENTED();
            case OPCODE_I64_ADD:
                ARITHMETIC_OP2(s64, s64, +, "+", "%lld", "%lld");
                break;
            case OPCODE_I64_SUB:
                ARITHMETIC_OP2(s64, s64, -, "+", "%lld", "%lld");
                break;
            case OPCODE_I64_MUL:
                ARITHMETIC_OP2(s64, s64, *, "*", "%lld", "%lld");
                break;
            case OPCODE_I64_DIV_S:
                ARITHMETIC_OP2(s64, s64, /, "/", "%lld", "%lld");
                break;
            case OPCODE_I64_DIV_U:
                ARITHMETIC_OP2(u64, u64, /, "/", "%llu", "%llu");
                break;
            case OPCODE_I64_REM_S:
                ARITHMETIC_OP2(s64, s64, %, "%%", "%lld", "%lld");
                break;
            case OPCODE_I64_REM_U:
                ARITHMETIC_OP2(u64, u64, %, "%%", "%llu", "%llu");
                break;
            case OPCODE_I64_AND:
                ARITHMETIC_OP2(u64, u64, &, "&", "%llu", "%llu");
                break;
            case OPCODE_I64_OR:
                ARITHMETIC_OP2(u64, u64, |, "|", "%llu", "%llu");
                break;
            case OPCODE_I64_XOR:
                ARITHMETIC_OP2(u64, u64, ^, "^", "%llu", "%llu");
                break;
            case OPCODE_I64_SHL:
                ARITHMETIC_OP2(u64, u64, <<, "<<", "%llu", "%llu");
                break;
            case OPCODE_I64_SHR_S:
                ARITHMETIC_OP2(s64, s64, >>, ">>", "%llu", "%llu");
                break;
            case OPCODE_I64_SHR_U:
                ARITHMETIC_OP2(u64, u64, >>, ">>", "%llu", "%llu");
                break;
            case OPCODE_I64_ROTL: NOT_IMPLEMENTED();
            case OPCODE_I64_ROTR: NOT_IMPLEMENTED();
            case OPCODE_F32_ABS: NOT_IMPLEMENTED();
            case OPCODE_F32_NEG: NOT_IMPLEMENTED();
            case OPCODE_F32_CEIL: NOT_IMPLEMENTED();
            case OPCODE_F32_FLOOR: NOT_IMPLEMENTED();
            case OPCODE_F32_TRUNC: NOT_IMPLEMENTED();
            case OPCODE_F32_NEAREST: NOT_IMPLEMENTED();
            case OPCODE_F32_SQRT: NOT_IMPLEMENTED();
            case OPCODE_F32_ADD:
                ARITHMETIC_OP2(f32, f32, +, "+", "%f", "%f");
                break;
            case OPCODE_F32_SUB:
                ARITHMETIC_OP2(f32, f32, -, "-", "%f", "%f");
                break;
            case OPCODE_F32_MUL:
                ARITHMETIC_OP2(f32, f32, *, "*", "%f", "%f");
                break;
            case OPCODE_F32_DIV:
                ARITHMETIC_OP2(f32, f32, /, "/", "%f", "%f");
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
                ARITHMETIC_OP2(f64, f64, +, "+", "%g", "%g");
                break;
            case OPCODE_F64_SUB:
                ARITHMETIC_OP2(f64, f64, -, "-", "%g", "%g");
                break;
            case OPCODE_F64_MUL:
                ARITHMETIC_OP2(f64, f64, *, "*", "%g", "%g");
                break;
            case OPCODE_F64_DIV:
                ARITHMETIC_OP2(f64, f64, /, "/", "%g", "%g");
                break;
            case OPCODE_F64_MIN: NOT_IMPLEMENTED();
            case OPCODE_F64_MAX: NOT_IMPLEMENTED();
            case OPCODE_F64_COPYSIGN: NOT_IMPLEMENTED();
            case OPCODE_WRAP_I64: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F64_U: NOT_IMPLEMENTED();
#define CONVERT_OP(arg_type, ret_type, operand, arg_fmt) do { \
    fprintf(stdout, "stack[%d]." # ret_type " = (" # ret_type ") stack[%d]." # arg_type "(" arg_fmt ") \n", \
            code->op0.reg, code->op1.reg, stack[code->op1.reg].arg_type);                                   \
    stack[code->op0.reg].ret_type = (operand) stack[code->op1.reg].arg_type;                                \
    code++;                                                                                                 \
} while (0)
            case OPCODE_I64_EXTEND_I32_S:
                CONVERT_OP(s32, s64, wasm_s64_t, "%d");
                break;
            case OPCODE_I64_EXTEND_I32_U:
                CONVERT_OP(u32, u64, wasm_u64_t, "%d");
                break;
            case OPCODE_I64_TRUNC_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F64_U: NOT_IMPLEMENTED();

            case OPCODE_F32_CONVERT_I32_S:
                CONVERT_OP(s32, f32, wasm_f32_t, "%d");
                break;
            case OPCODE_F32_CONVERT_I32_U:
                CONVERT_OP(u32, f32, wasm_f32_t, "%u");
                break;
            case OPCODE_F32_CONVERT_I64_S:
                CONVERT_OP(s64, f32, wasm_f32_t, "%lld");
                break;
            case OPCODE_F32_CONVERT_I64_U:
                CONVERT_OP(u64, f32, wasm_f32_t, "%llu");
                break;
            case OPCODE_F32_DEMOTE_F64: NOT_IMPLEMENTED();
            case OPCODE_F64_CONVERT_I32_S:
                CONVERT_OP(s32, f64, wasm_f64_t, "%d");
                break;
            case OPCODE_F64_CONVERT_I32_U:
                CONVERT_OP(u32, f64, wasm_f64_t, "%u");
                break;
            case OPCODE_F64_CONVERT_I64_S:
                CONVERT_OP(s64, f64, wasm_f64_t, "%llu");
                break;
            case OPCODE_F64_CONVERT_I64_U:
                CONVERT_OP(u64, f64, wasm_f32_t, "%llu");
                break;
            case OPCODE_F64_PROMOTE_F32: NOT_IMPLEMENTED();
            case OPCODE_I32_REINTERPRET_F32:
                fprintf(stdout, "stack[%d].u32 = reinterpret_cast(stack[%d].f32 (%f))\n",
                        code->op0.reg, code->op1.reg, stack[code->op1.reg].f32);
                stack[code->op0.reg].u32 = stack[code->op1.reg].u32;
                code++;
                break;
            case OPCODE_I64_REINTERPRET_F64:
                fprintf(stdout, "stack[%d].u32 = reinterpret_cast(stack[%d].f64 (%g))\n",
                        code->op0.reg, code->op1.reg, stack[code->op1.reg].f64);
                stack[code->op0.reg].u64 = stack[code->op1.reg].u64;
                code++;
                break;
            case OPCODE_F32_REINTERPRET_I32:
                fprintf(stdout, "stack[%d].f32 = reinterpret_cast(stack[%d].u32 (%u))\n",
                        code->op0.reg, code->op1.reg, stack[code->op1.reg].u32);
                stack[code->op0.reg].f32 = stack[code->op1.reg].f32;
                code++;
                break;
            case OPCODE_F64_REINTERPRET_I64:
                fprintf(stdout, "stack[%d].f64 = reinterpret_cast(stack[%d].u64 (%llu))\n",
                        code->op0.reg, code->op1.reg, stack[code->op1.reg].u64);
                stack[code->op0.reg].f64 = stack[code->op1.reg].f64;
                code++;
                break;
#define EXTEND_OP(arg_type, ret_type, operand) do {                                     \
    fprintf(stdout, "stack[%d]." #ret_type " = extend8(stack[%d]." #arg_type " (%d))\n",\
            code->op0.reg, code->op1.reg, stack[code->op1.reg].arg_type);               \
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

static void wasmbox_dump_function(wasmbox_code_t *code, const char *indent)
{
    while (1) {
        switch (code->h.opcode) {
            case OPCODE_UNREACHABLE:
            case OPCODE_NOP:
            case OPCODE_DROP:
            case OPCODE_SELECT:
            case OPCODE_EXIT:
                fprintf(stdout, "%s%p %s ", indent, code, debug_opcodes[code->h.opcode]);
                return;
            case OPCODE_RETURN:
                fprintf(stdout, "%sreturn;\n", indent);
                return;
            case OPCODE_MOVE:
                fprintf(stdout, "%sstack[%d].u64= stack[%d].u64\n", indent, code->op0.reg, code->op1.reg);
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
                NOT_IMPLEMENTED();
            case OPCODE_I32_CTZ:
                NOT_IMPLEMENTED();
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
            case OPCODE_I32_ROTL: NOT_IMPLEMENTED();
            case OPCODE_I32_ROTR: NOT_IMPLEMENTED();
            case OPCODE_I64_CLZ: NOT_IMPLEMENTED();
            case OPCODE_I64_CTZ: NOT_IMPLEMENTED();
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
            case OPCODE_I64_ROTL: NOT_IMPLEMENTED();
            case OPCODE_I64_ROTR: NOT_IMPLEMENTED();
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

static void wasmbox_module_dump(wasmbox_module_t *mod)
{
    fprintf(stdout, "module %p {\n", mod);
    if (0) {
        for (wasm_u32_t i = 0; i < mod->type_size; ++i) {
            print_function_type(mod->types[i]);
            fprintf(stdout, "\n");
        }
    }
    if (mod->memory_block_size > 0) {
        fprintf(stdout, "  mem(%p, current=%u, max=%u)\n", mod->memory_block,
                mod->memory_block_size, mod->memory_block_capacity);
    }
    if (mod->global_function) {
        print_function(mod->global_function, 0);
        fprintf(stdout, "\n");
    }
    for (wasm_u32_t i = 0; i < mod->function_size; ++i) {
        print_function(mod->functions[i], i);
        fprintf(stdout, " {\n");
        wasmbox_dump_function(mod->functions[i]->code, "  ");
        fprintf(stdout, "}\n");
    }
    fprintf(stdout, "}\n");
}

int wasmbox_load_module(wasmbox_module_t *mod, const char *file_name,
                        wasm_u16_t file_name_len) {
    wasmbox_input_stream_t stream = {};
    wasmbox_input_stream_t *ins = wasmbox_input_stream_open(&stream, file_name);
    if (ins == NULL) {
        LOG("Failed to load file");
        return -1;
    }
    int parsed = parse_module(ins, mod);
    if (parsed == 0) {
        wasmbox_module_dump(mod);
        if (mod->global_function != NULL && mod->global_function->code != NULL) {
            wasmbox_eval_function(mod, mod->global_function->code, mod->globals);
        }
    }
    wasmbox_input_stream_close(ins);
    return parsed;
}

int wasmbox_module_dispose(wasmbox_module_t *mod) {
    for (wasm_u32_t i = 0; i < mod->type_size; ++i) {
        wasmbox_free(mod->types[i]);
    }
    wasmbox_free(mod->types);
    for (wasm_u32_t i = 0; i < mod->function_size; ++i) {
        wasmbox_free(mod->functions[i]->name);
        wasmbox_free(mod->functions[i]);
    }
    wasmbox_free(mod->functions);
    wasmbox_free(mod->global_function);
    wasmbox_free(mod->memory_block);
    return 0;
}

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

int wasmbox_eval_module(wasmbox_module_t *mod, wasmbox_value_t stack[],
                        wasm_u16_t result_stack_size) {
    wasmbox_function_t *func = wasmbox_module_find_entrypoint(mod);
    if (func == NULL) {
        LOG("_start function not found");
        return -1;
    }
    global_stack = stack;
    wasmbox_value_t *stack_top = stack + func->type->return_size;
    wasmbox_code_t code[1] = {};
    code[0].h.opcode = OPCODE_EXIT;
    stack_top[0].u64 = (wasm_u64_t) (uintptr_t) stack_top;
    stack_top[1].u64 = (wasm_u64_t) (uintptr_t) code;
    dump_stack(stack_top);
    wasmbox_eval_function(mod, func->code, stack_top);
    return 0;
}
