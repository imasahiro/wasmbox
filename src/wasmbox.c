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

#define LOG(MSG) fprintf(stderr, "(%s:%d)" MSG, __FILE_NAME__, __LINE__);

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
        mod->types = (wasmbox_type_t **) realloc(mod->types, sizeof(mod->types) * mod->type_capacity * 2);
        mod->type_capacity *= 2;
    }
    mod->types[mod->type_size++] = func_type;
}

#define MODULE_FUNCTIONS_INIT_SIZE 4
static void wasmbox_module_register_new_function(wasmbox_module_t *mod, wasmbox_function_t *func)
{
    if (mod->functions == NULL) {
        mod->functions = (wasmbox_function_t **) wasmbox_malloc(sizeof(mod->functions) * MODULE_FUNCTIONS_INIT_SIZE);
        mod->function_size = 0;
        mod->type_capacity = MODULE_FUNCTIONS_INIT_SIZE;
    }
    if (mod->function_size + 1 == mod->function_capacity) {
        mod->functions = (wasmbox_function_t **) realloc(mod->types,
                                                         sizeof(mod->types) * mod->function_capacity * 2);
        mod->function_capacity *= 2;
    }
    mod->functions[mod->function_size++] = func;
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
    code.op1.reg = index;
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
    fprintf(stdout, " {}");
    return 0;
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
    if (mod->global_function) {
        print_function(mod->global_function, 0);
        fprintf(stdout, "\n");
    }
    for (wasm_u32_t i = 0; i < mod->function_size; ++i) {
        print_function(mod->functions[i], i);
        fprintf(stdout, "\n");
    }
    fprintf(stdout, "}\n");
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
    //fprintf(stdout, "custom %llu\n", section_size);
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
            LOG("unknown type")
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

static int parse_instruction(wasmbox_input_stream_t *ins, wasmbox_function_t *func);

// INST(0x02 bt:blocktype (in:instr)* 0x0B, block bt in* end)
// INST(0x03 bt:blocktype (in:instr)* 0x0B, loop bt in* end)
static int decode_block(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
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
    while (1) {
        wasm_u8_t next = wasmbox_input_stream_peek_u8(ins);
        if (next == 0x0B) {
            wasmbox_input_stream_read_u8(ins);
            break;
        }
        if (parse_instruction(ins, func)) {
            return -1;
        }
    }
    return 0;
}

// INST(0x05, end)
static int decode_block_end(wasmbox_input_stream_t *in, wasmbox_function_t *func, wasm_u8_t op) {
    wasmbox_code_t code;
    code.h.opcode = OPCODE_RETURN;
    code.op0.reg = 0;
    code.op1.reg = func->stack_top - 1;
    wasmbox_code_add(func, &code);
    return 0;
}

// INST(0x04 bt:blocktype (in:instr)* 0x0B, if bt in* end)
// INST(0x04 bt:blocktype (in:instr)* 0x05 (in2:instr)* 0x0B, if bt in1* else in2* end)
static int decode_if(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
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
        if (parse_instruction(ins, func)) {
            return -1;
        }
    }
    return 0;
}

// INST(0x0C l:labelidx, br l)
static int decode_br(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t labelidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "br %llu\n", labelidx);
    return 0;
}

// INST(0x0D l:labelidx, br_if l)
static int decode_br_if(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t labelidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "br_if %llu\n", labelidx);
    return 0;
}

// INST(0x0E l:vec(labelidx) lN:labelidx, br_table l* lN)
static int decode_br_table(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
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
static int decode_return(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
    fprintf(stdout, "return\n");
    return 0;
}

// BLOCK_INST(0x11, x:typeidx 0x00, call_indirect x)
static int decode_call(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t funcidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "call %llu\n", funcidx);
    return 0;
}

// INST(0x11 x:typeidx 0x00, call_indirect x)
static int decode_call_indirect(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t typeidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "call_indirect %llu\n", typeidx);
    return 0;
}


static int decode_variable_inst(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t idx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    switch (op) {
#define FUNC(opcode, param, type, inst, vmopcode) case (opcode): { \
    wasmbox_code_add_variable(func, vmopcode, idx);                \
    return 0; \
}
        VARIABLE_INST_EACH(FUNC)
#undef FUNC
        default:
            return -1;
    }
    return -1;
}

static int parse_memarg(wasmbox_input_stream_t *ins, wasm_u64_t *align, wasm_u64_t *offset) {
    *align = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    *offset = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    return 0;
}

static int decode_memory_inst(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
    wasm_u64_t align;
    wasm_u64_t offset;
    if (parse_memarg(ins, &align, &offset)) {
        return -1;
    }
    switch (op) {
#define FUNC(opcode, param, type, inst, vmopcode) case (opcode): { \
    fprintf(stdout, "" #type #inst "(align:%llu, offset:%llu)\n", align, offset); \
    break; \
}
        MEMORY_INST_EACH(FUNC)
#undef FUNC
        default:
            return -1;
    }
    return 0;
}

static int decode_memory_size_and_grow(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
    if (wasmbox_input_stream_read_u8(ins) != 0x00) {
        return -1;
    }
    switch (op) {
#define FUNC(opcode, param, type, inst, vmopcode) case (opcode): { \
    fprintf(stdout, "" #type #inst "\n"); \
    break; \
}
        MEMORY_OP_EACH(FUNC)
#undef FUNC
        default:
            return -1;
    }
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
static int decode_constant_inst(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
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

static int decode_op0_inst(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
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

static int decode_truncation_inst(wasmbox_input_stream_t *ins, wasmbox_function_t *func, wasm_u8_t op) {
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

static int parse_instruction(wasmbox_input_stream_t *ins, wasmbox_function_t *func) {
    wasm_u8_t op = wasmbox_input_stream_read_u8(ins);
    for (int i = 0; i < sizeof(decoders) / sizeof(decoders[0]); i++) {
        const wasmbox_op_decorder_t d = decoders[i];
        if (d.lower <= op && op <= d.upper) {
            return d.func(ins, func, op);
        }
    }
    return -1;
}

static int parse_code(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                      wasmbox_function_t *func, wasm_u64_t codelen) {
    wasm_u64_t end = ins->index + codelen;
    while (ins->index < end) {
        if (parse_instruction(ins, func)) {
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
    func->locals = len;
    for (wasm_u64_t i = 0; i < len; i++) {
        wasm_u64_t localidx;
        wasmbox_value_type_t type;
        if (parse_local_variable(ins, &localidx, &type)) {
            return -1;
        }
        fprintf(stdout, "%llu: local%llu(type=%d)\n", i, localidx, type);
    }
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
        func->stack_top = func->type->argument_size;
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
        if (limit.max == WASM_U32_MAX) {
            fprintf(stdout, "\tmem(%d)\n", limit.min);
        } else {
            fprintf(stdout, "\tmem(%d, %d)\n", limit.min, limit.max);
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
    fprintf(stdout, "- %s type:%s\n", is_const ? "const" : "var", value_type_to_string(valtype));
    wasmbox_function_t *global = mod->global_function;
    if (global == NULL) {
        global = mod->global_function = (wasmbox_function_t *) wasmbox_malloc(sizeof(*mod->global_function));
        wasm_u32_t len = strlen("__global__");
        global->name = (wasmbox_name_t *) wasmbox_malloc(sizeof(*global->name) + len);
        global->name->len = len;
        memcpy(global->name->value, "__global__", len);
    }
    while (1) {
        wasm_u8_t next = wasmbox_input_stream_peek_u8(ins);
        if (next == 0x0B) {
            wasmbox_input_stream_read_u8(ins);
            break;
        }
        if (parse_instruction(ins, global)) {
            return -1;
        }
    }
    return 0;
}

static int parse_global_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "global (num:%llu)\n", len);
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

static int parse_data_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    fprintf(stdout, "data\n");
    dump_binary(ins, section_size);
    ins->index += section_size;
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
        LOG("Invalid magic number")
        return -1;
    }
    if (parse_version(ins) != 0) {
        LOG("Invalid version number")
        return -1;
    }
    while (!wasmbox_input_stream_is_end_of_stream(ins)) {
        if (parse_section(ins, module) != 0) {
            return -1;
        }
    }
    return 0;
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

#define NOT_IMPLEMENTED() do { \
    LOG("not-implemented");    \
    return;                    \
} while (0)

static void wasmbox_eval_function(wasmbox_module_t *mod, wasmbox_code_t *code, wasmbox_value_t *stack)
{
    while (1) {
        //fprintf(stdout, "opcode: %s\n", debug_opcodes[code->h.opcode]);
        switch (code->h.opcode) {
            case OPCODE_UNREACHABLE:
                NOT_IMPLEMENTED();
            case OPCODE_NOP:
                NOT_IMPLEMENTED();
            case OPCODE_DROP:
                NOT_IMPLEMENTED();
            case OPCODE_SELECT:
                NOT_IMPLEMENTED();
            case OPCODE_RETURN:
                fprintf(stdout, "stack[%d].u64= stack[%d].u64\n", code->op0.reg, code->op1.reg);
                stack[code->op0.reg].u64 = stack[code->op1.reg].u64;
                return;
            case OPCODE_LOCAL_GET:
                fprintf(stdout, "stack[%d].u64= stack[%d].u64\n", code->op0.reg, code->op1.reg);
                stack[code->op0.reg].u64 = stack[code->op1.reg].u64;
                code++;
                break;
            case OPCODE_LOCAL_SET:
                NOT_IMPLEMENTED();
            case OPCODE_LOCAL_TEE:
                NOT_IMPLEMENTED();
            case OPCODE_GLOBAL_GET:
                NOT_IMPLEMENTED();
            case OPCODE_GLOBAL_SET:
                NOT_IMPLEMENTED();
            case OPCODE_I32_LOAD:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LOAD:
                NOT_IMPLEMENTED();
            case OPCODE_F32_LOAD:
                NOT_IMPLEMENTED();
            case OPCODE_F64_LOAD:
                NOT_IMPLEMENTED();
            case OPCODE_I32_LOAD8_S:
                NOT_IMPLEMENTED();
            case OPCODE_I32_LOAD8_U:
                NOT_IMPLEMENTED();
            case OPCODE_I32_LOAD16_S:
                NOT_IMPLEMENTED();
            case OPCODE_I32_LOAD16_U:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LOAD8_S:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LOAD8_U:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LOAD16_S:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LOAD16_U:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LOAD32_S:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LOAD32_U:
                NOT_IMPLEMENTED();
            case OPCODE_I32_STORE:
                NOT_IMPLEMENTED();
            case OPCODE_I64_STORE:
                NOT_IMPLEMENTED();
            case OPCODE_F32_STORE:
                NOT_IMPLEMENTED();
            case OPCODE_F64_STORE:
                NOT_IMPLEMENTED();
            case OPCODE_I32_STORE8:
                NOT_IMPLEMENTED();
            case OPCODE_I32_STORE16:
                NOT_IMPLEMENTED();
            case OPCODE_I64_STORE8:
                NOT_IMPLEMENTED();
            case OPCODE_I64_STORE16:
                NOT_IMPLEMENTED();
            case OPCODE_I64_STORE32:
                NOT_IMPLEMENTED();
            case OPCODE_MEMORY_SIZE:
                NOT_IMPLEMENTED();
            case OPCODE_MEMORY_GROW:
                NOT_IMPLEMENTED();
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
#define ARITHMETIC_OP(type, operand, operand_str) do { \
    fprintf(stdout, "stack[%d]." # type "= stack[%d]." # type " " operand_str " stack[%d]." #type "\n", \
            code->op0.reg, code->op2.reg, code->op1.reg);                                               \
    stack[code->op0.reg].type = stack[code->op1.reg].type operand stack[code->op2.reg].type;            \
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
                NOT_IMPLEMENTED();
            case OPCODE_I64_EQ:
                NOT_IMPLEMENTED();
            case OPCODE_I64_NE:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LT_S:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LT_U:
                NOT_IMPLEMENTED();
            case OPCODE_I64_GT_S:
                NOT_IMPLEMENTED();
            case OPCODE_I64_GT_U:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LE_S:
                NOT_IMPLEMENTED();
            case OPCODE_I64_LE_U:
                NOT_IMPLEMENTED();
            case OPCODE_I64_GE_S:
                NOT_IMPLEMENTED();
            case OPCODE_I64_GE_U:
                NOT_IMPLEMENTED();
            case OPCODE_F32_EQ:
                NOT_IMPLEMENTED();
            case OPCODE_F32_NE:
                NOT_IMPLEMENTED();
            case OPCODE_F32_LT:
                NOT_IMPLEMENTED();
            case OPCODE_F32_GT:
                NOT_IMPLEMENTED();
            case OPCODE_F32_LE:
                NOT_IMPLEMENTED();
            case OPCODE_F32_GE:
                NOT_IMPLEMENTED();
            case OPCODE_F64_EQ:
                NOT_IMPLEMENTED();
            case OPCODE_F64_NE:
                NOT_IMPLEMENTED();
            case OPCODE_F64_LT:
                NOT_IMPLEMENTED();
            case OPCODE_F64_GT:
                NOT_IMPLEMENTED();
            case OPCODE_F64_LE:
                NOT_IMPLEMENTED();
            case OPCODE_F64_GE:
                NOT_IMPLEMENTED();
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
            case OPCODE_I32_AND: NOT_IMPLEMENTED();
            case OPCODE_I32_OR: NOT_IMPLEMENTED();
            case OPCODE_I32_XOR: NOT_IMPLEMENTED();
            case OPCODE_I32_SHL: NOT_IMPLEMENTED();
            case OPCODE_I32_SHR_S: NOT_IMPLEMENTED();
            case OPCODE_I32_SHR_U: NOT_IMPLEMENTED();
            case OPCODE_I32_ROTL: NOT_IMPLEMENTED();
            case OPCODE_I32_ROTR: NOT_IMPLEMENTED();
            case OPCODE_I64_CLZ: NOT_IMPLEMENTED();
            case OPCODE_I64_CTZ: NOT_IMPLEMENTED();
            case OPCODE_I64_POPCNT: NOT_IMPLEMENTED();
            case OPCODE_I64_ADD: NOT_IMPLEMENTED();
            case OPCODE_I64_SUB: NOT_IMPLEMENTED();
            case OPCODE_I64_MUL: NOT_IMPLEMENTED();
            case OPCODE_I64_DIV_S: NOT_IMPLEMENTED();
            case OPCODE_I64_DIV_U: NOT_IMPLEMENTED();
            case OPCODE_I64_REM_S: NOT_IMPLEMENTED();
            case OPCODE_I64_REM_U: NOT_IMPLEMENTED();
            case OPCODE_I64_AND: NOT_IMPLEMENTED();
            case OPCODE_I64_OR: NOT_IMPLEMENTED();
            case OPCODE_I64_XOR: NOT_IMPLEMENTED();
            case OPCODE_I64_SHL: NOT_IMPLEMENTED();
            case OPCODE_I64_SHR_S: NOT_IMPLEMENTED();
            case OPCODE_I64_SHR_U: NOT_IMPLEMENTED();
            case OPCODE_I64_ROTL: NOT_IMPLEMENTED();
            case OPCODE_I64_ROTR: NOT_IMPLEMENTED();
            case OPCODE_F32_ABS: NOT_IMPLEMENTED();
            case OPCODE_F32_NEG: NOT_IMPLEMENTED();
            case OPCODE_F32_CEIL: NOT_IMPLEMENTED();
            case OPCODE_F32_FLOOR: NOT_IMPLEMENTED();
            case OPCODE_F32_TRUNC: NOT_IMPLEMENTED();
            case OPCODE_F32_NEAREST: NOT_IMPLEMENTED();
            case OPCODE_F32_SQRT: NOT_IMPLEMENTED();
            case OPCODE_F32_ADD: NOT_IMPLEMENTED();
            case OPCODE_F32_SUB: NOT_IMPLEMENTED();
            case OPCODE_F32_MUL: NOT_IMPLEMENTED();
            case OPCODE_F32_DIV: NOT_IMPLEMENTED();
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
            case OPCODE_F64_ADD: NOT_IMPLEMENTED();
            case OPCODE_F64_SUB: NOT_IMPLEMENTED();
            case OPCODE_F64_MUL: NOT_IMPLEMENTED();
            case OPCODE_F64_DIV: NOT_IMPLEMENTED();
            case OPCODE_F64_MIN: NOT_IMPLEMENTED();
            case OPCODE_F64_MAX: NOT_IMPLEMENTED();
            case OPCODE_F64_COPYSIGN: NOT_IMPLEMENTED();
            case OPCODE_WRAP_I64: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I32_TRUNC_F64_U: NOT_IMPLEMENTED();
            case OPCODE_I64_EXTEND_I32_S: NOT_IMPLEMENTED();
            case OPCODE_I64_EXTEND_I32_U: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F32_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F32_U: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F64_S: NOT_IMPLEMENTED();
            case OPCODE_I64_TRUNC_F64_U: NOT_IMPLEMENTED();
            case OPCODE_F32_CONVERT_I32_S: NOT_IMPLEMENTED();
            case OPCODE_F32_CONVERT_I32_U: NOT_IMPLEMENTED();
            case OPCODE_F32_CONVERT_I64_S: NOT_IMPLEMENTED();
            case OPCODE_F32_CONVERT_I64_U: NOT_IMPLEMENTED();
            case OPCODE_F32_DEMOTE_F64: NOT_IMPLEMENTED();
            case OPCODE_F64_CONVERT_I32_S: NOT_IMPLEMENTED();
            case OPCODE_F64_CONVERT_I32_U: NOT_IMPLEMENTED();
            case OPCODE_F64_CONVERT_I64_S: NOT_IMPLEMENTED();
            case OPCODE_F64_CONVERT_I64_U: NOT_IMPLEMENTED();
            case OPCODE_F64_PROMOTE_F32: NOT_IMPLEMENTED();
            case OPCODE_I32_REINTERPRET_F32: NOT_IMPLEMENTED();
            case OPCODE_I64_REINTERPRET_F64: NOT_IMPLEMENTED();
            case OPCODE_F32_REINTERPRET_I32: NOT_IMPLEMENTED();
            case OPCODE_F64_REINTERPRET_I64: NOT_IMPLEMENTED();
            case OPCODE_I32_EXTEND8_S: NOT_IMPLEMENTED();
            case OPCODE_I32_EXTEND16_S: NOT_IMPLEMENTED();
            case OPCODE_I64_EXTEND8_S: NOT_IMPLEMENTED();
            case OPCODE_I64_EXTEND16_S: NOT_IMPLEMENTED();
            case OPCODE_I64_EXTEND32_S: NOT_IMPLEMENTED();
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

int wasmbox_eval_module(wasmbox_module_t *mod, wasmbox_value_t stack[],
                        wasm_u16_t result_stack_size) {
    wasmbox_function_t *func = wasmbox_module_find_entrypoint(mod);
    if (func == NULL) {
        LOG("_start function not found");
        return -1;
    }
    int num_of_returns = func->type->return_size;
    int num_of_arguments = func->type->argument_size;
    // +-------------+-----------------+
    // | stack - N   | return_val[N-1] |
    // |    ...      |                 |
    // | stack - 2   | return_val[1]   |
    // | stack - 1   | return_val[0]   |
    // | stack       | arguments[0]    |
    // | stack + 1   | arguments[1]    |
    // | stack + 2   | arguments[2]    |
    // |    ...      |                 |
    // | stack + N   | arguments[N]    |
    // | stack + N+1 | local_val[0]    |
    // |    ...      |                 |
    // | stack + M+M | local_val[M-1]  |
    // | stack + N+M | stack_top       |
    // +-------------+-----------------+
    wasmbox_value_t *stack_top = stack + func->locals;
    wasmbox_eval_function(mod, func->code, stack_top);
    return 0;
}
