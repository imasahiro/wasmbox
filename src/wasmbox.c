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
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG(MSG) fprintf(stderr, "(%s:%d)" MSG, __FILE_NAME__, __LINE__);

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
        case 0x7f:
            *type = WASM_TYPE_I32;
            return 0;
        case 0x7e:
            *type = WASM_TYPE_I64;
            return 0;
        case 0x7d:
            *type = WASM_TYPE_F32;
            return 0;
        case 0x7c:
            *type = WASM_TYPE_F64;
            return 0;
        default:
            // unreachable
            LOG("unknown type")
            return -1;
    }
}

static const char *value_type_to_string(wasmbox_value_type_t type) {
    switch (type) {
        case WASM_TYPE_I32:
            return "I32";
        case WASM_TYPE_I64:
            return "I64";
        case WASM_TYPE_F32:
            return "F32";
        case WASM_TYPE_F64:
            return "F64";
        default:
            return "unreachable";
    }
}

static int parse_type_vector(wasmbox_input_stream_t *ins) {
    wasmbox_value_type_t type;
    fprintf(stdout, "(");
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    for (wasm_u64_t i = 0; i < len; i++) {
        if (parse_value_type(ins, &type) != 0) {
            return -1;
        }
        if (i != 0) {
            fprintf(stdout, ", ");
        }
        fprintf(stdout, "%s", value_type_to_string(type));
    }
    fprintf(stdout, ")");
    return 0;
}

static int parse_function_type(wasmbox_input_stream_t *ins, wasmbox_module_t *mod) {
    assert(wasmbox_input_stream_read_u8(ins) == 0x60);
    parse_type_vector(ins);
    fprintf(stdout, " -> ");
    parse_type_vector(ins);
    fprintf(stdout, "\n");
    return 0;
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

static int parse_instruction(wasmbox_input_stream_t *ins);

// INST(0x02 bt:blocktype (in:instr)* 0x0B, block bt in* end)
// INST(0x03 bt:blocktype (in:instr)* 0x0B, loop bt in* end)
static int decode_block(wasmbox_input_stream_t *ins, wasm_u8_t op) {
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
        if (parse_instruction(ins)) {
            return -1;
        }
    }
    return 0;
}

// INST(0x05, end)
static int decode_block_end(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    fprintf(stdout, "end\n");
    return 0;
}

// INST(0x04 bt:blocktype (in:instr)* 0x0B, if bt in* end)
// INST(0x04 bt:blocktype (in:instr)* 0x05 (in2:instr)* 0x0B, if bt in1* else in2* end)
static int decode_if(wasmbox_input_stream_t *ins, wasm_u8_t op) {
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
        if (parse_instruction(ins)) {
            return -1;
        }
    }
    return 0;
}

// INST(0x0C l:labelidx, br l)
static int decode_br(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    wasm_u64_t labelidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "br %llu\n", labelidx);
    return 0;
}

// INST(0x0D l:labelidx, br_if l)
static int decode_br_if(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    wasm_u64_t labelidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "br_if %llu\n", labelidx);
    return 0;
}

// INST(0x0E l:vec(labelidx) lN:labelidx, br_table l* lN)
static int decode_br_table(wasmbox_input_stream_t *ins, wasm_u8_t op) {
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
static int decode_return(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    fprintf(stdout, "return\n");
    return 0;
}

// BLOCK_INST(0x11, x:typeidx 0x00, call_indirect x)
static int decode_call(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    wasm_u64_t funcidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "call %llu\n", funcidx);
    return 0;
}

// INST(0x11 x:typeidx 0x00, call_indirect x)
static int decode_call_indirect(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    wasm_u64_t typeidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "call_indirect %llu\n", typeidx);
    return 0;
}

static int decode_variable_inst(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    wasm_u64_t idx = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    switch (op) {
#define FUNC(opcode, param, type, inst) case (opcode): { \
    fprintf(stdout, "" #inst "(x:%llu)\n", idx); \
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

static int decode_memory_inst(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    wasm_u64_t align;
    wasm_u64_t offset;
    if (parse_memarg(ins, &align, &offset)) {
        return -1;
    }
    switch (op) {
#define FUNC(opcode, param, type, inst) case (opcode): { \
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

static int decode_memory_size_and_grow(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    if (wasmbox_input_stream_read_u8(ins) != 0x00) {
        return -1;
    }
    switch (op) {
#define FUNC(opcode, param, type, inst) case (opcode): { \
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

/* Numeric Instructions */
static int decode_constant_inst(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    wasmbox_value_t v;
    switch (op) {
        case 0x41:
            v.u32 = wasmbox_parse_signed_leb128(ins->data + ins->index, &ins->index, ins->length);
            fprintf(stdout, "i32.const %u\n", v.u32);
            break;
        case 0x42:
            v.u64 = wasmbox_parse_signed_leb128(ins->data + ins->index, &ins->index, ins->length);
            fprintf(stdout, "i64.const %llu\n", v.u64);
            break;
        case 0x43:
            v.f32 = *(wasm_f32_t *) (ins->data + ins->index);
            fprintf(stdout, "f32.const %x %f\n", v.u32, v.f32);
            ins->index += sizeof(wasm_f32_t);
            break;
        case 0x44:
            v.f64 = *(wasm_f64_t *) (ins->data + ins->index);
            fprintf(stdout, "f64.const %llx %g\n", v.u64, v.f64);
            ins->index += sizeof(wasm_f64_t);
            break;
        default:
            return -1;
    }
    return 0;
}

static int decode_op0_inst(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    switch (op) {
#define FUNC(opcode, type, inst) case (opcode): { \
    fprintf(stdout, "" #type "." # inst "\n"); \
    return 0; \
}
        DUMMY_INST_EACH(FUNC)
        PARAMETRIC_INST_EACH(FUNC)
        NUMERIC_INST_EACH(FUNC)
#undef FUNC
        default:
            return -1;
    }
    return -1;
}

static int decode_truncation_inst(wasmbox_input_stream_t *ins, wasm_u8_t op) {
    wasm_u8_t op1 = wasmbox_input_stream_read_u8(ins);
    switch (op1) {
#define FUNC(opcode0, opcode1, type, inst) case opcode1: { \
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

static int parse_instruction(wasmbox_input_stream_t *ins) {
    wasm_u8_t op = wasmbox_input_stream_read_u8(ins);
    for (int i = 0; i < sizeof(decoders) / sizeof(decoders[0]); i++) {
        const wasmbox_op_decorder_t d = decoders[i];
        if (d.lower <= op && op <= d.upper) {
            return d.func(ins, op);
        }
    }
    return -1;
}

static int parse_code(wasmbox_input_stream_t *ins, wasmbox_module_t *mod, wasm_u64_t codelen) {
    wasm_u64_t end = ins->index + codelen;
    while (ins->index < end) {
        if (parse_instruction(ins)) {
            return -1;
        }
    }
    return 0;
}

static int parse_local_variable(wasmbox_input_stream_t *ins, wasm_u64_t *index, wasmbox_value_type_t *valtype) {
    *index = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    return parse_value_type(ins, valtype);
}

static int parse_local_variables(wasmbox_input_stream_t *ins) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
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

static int parse_function(wasmbox_input_stream_t *ins, wasmbox_module_t *mod) {
    wasm_u64_t size = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    wasm_u64_t index = ins->index;
    fprintf(stdout, "code(size:%llu)\n", size);
    dump_binary(ins, size);
    if (parse_local_variables(ins)) {
        return -1;
    }
    size -= ins->index - index;
    return parse_code(ins, mod, size);
}

static int parse_type_section(wasmbox_input_stream_t *ins, wasm_u64_t section_size, wasmbox_module_t *mod) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    fprintf(stdout, "type size=%llu\n", len);
    for (wasm_u64_t i = 0; i < len; i++) {
        if (parse_function_type(ins, mod) != 0) {
            return -1;
        }
    }
    return 0;
}

static int parse_name(wasmbox_input_stream_t *ins, wasmbox_name_t **name) {
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    wasmbox_name_t *n = (wasmbox_name_t *) malloc(sizeof(wasmbox_name_t) + len);
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
    free(module_name);
    free(ns_name);
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
    fprintf(stdout, "function (num:%llu)\n", len);
    for (wasm_u64_t i = 0; i < len; i++) {
        wasm_u32_t v = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
        fprintf(stdout, "function idx(%u)\n", v);
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
    while (1) {
        wasm_u8_t next = wasmbox_input_stream_peek_u8(ins);
        if (next == 0x0B) {
            wasmbox_input_stream_read_u8(ins);
            break;
        }
        if (parse_instruction(ins)) {
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
    fprintf(stdout, "- export '%.*s' %s(%d)\n", name->len, name->value, debug_name, index);
    free(name);
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
    wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index, ins->length);
    for (wasm_u64_t i = 0; i < len; i++) {
        if (parse_function(ins, mod)) {
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
    wasmbox_input_stream_close(ins);
    return parsed;
}

int wasmbox_eval_module(wasmbox_module_t *mod, wasmbox_value_t result[],
                        wasm_u16_t result_stack_size) {
    return 0;
}
