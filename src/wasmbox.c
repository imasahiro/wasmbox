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

#include "allocator.h"
#include "input-stream.h"
#include "interpreter.h"
#include "leb128.h"
#include "opcodes.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define LOG(MSG) fprintf(stderr, "(%s:%d)" MSG, __FILE__, __LINE__)

/* Module API */
#define MODULE_TYPES_INIT_SIZE 4
static void wasmbox_module_register_new_type(wasmbox_module_t *mod,
                                             wasmbox_type_t *func_type) {
  if (mod->types == NULL) {
    mod->types = (wasmbox_type_t **) wasmbox_malloc(sizeof(mod->types) *
                                                    MODULE_TYPES_INIT_SIZE);
    mod->type_size = 0;
    mod->type_capacity = MODULE_TYPES_INIT_SIZE;
  }
  if (mod->type_size + 1 == mod->type_capacity) {
    mod->type_capacity *= 2;
    mod->types = (wasmbox_type_t **) wasmbox_realloc(
        mod->types, sizeof(mod->types) * mod->type_capacity);
  }
  mod->types[mod->type_size++] = func_type;
}

#define MODULE_FUNCTIONS_INIT_SIZE 4
static void
wasmbox_module_register_new_function(wasmbox_module_t *mod,
                                     wasmbox_mutable_function_t *func) {
  if (mod->functions == NULL) {
    mod->functions = (wasmbox_function_t **) wasmbox_malloc(
        sizeof(wasmbox_mutable_function_t) * MODULE_FUNCTIONS_INIT_SIZE);
    mod->function_size = 0;
    mod->function_capacity = MODULE_FUNCTIONS_INIT_SIZE;
  }
  if (mod->function_size + 1 == mod->function_capacity) {
    mod->function_capacity *= 2;
    mod->functions = (wasmbox_function_t **) wasmbox_realloc(
        mod->functions,
        sizeof(wasmbox_mutable_function_t) * mod->function_capacity);
  }
  mod->functions[mod->function_size++] = (wasmbox_function_t *) func;
}

static int wasmbox_module_add_memory_page(wasmbox_module_t *mod,
                                          wasmbox_limit_t *memory_size) {
  if (mod->memory_block != NULL) {
    LOG("only one memory block allowed");
    return -1;
  }
  if (memory_size->min > memory_size->max) {
    LOG("not supported");
    return -1;
  }
  wasm_u32_t block_size = WASMBOX_PAGE_SIZE * memory_size->min;
  mod->memory_block = (wasmbox_memory_block_t *) wasmbox_malloc(
      sizeof(*mod->memory_block) + block_size);
  mod->memory_block_size = memory_size->min;
  mod->memory_block_capacity = memory_size->max;
  return 0;
}

static void wasmbox_function_add_table(wasmbox_mutable_function_t *func,
                                       wasmbox_table_t *table) {
  if (func->tables == NULL) {
    func->tables =
        (wasmbox_table_t **) wasmbox_malloc(sizeof(wasmbox_table_t *));
    func->table_size = 0;
    func->table_capacity = 1;
  }
  if (func->table_size + 1 == func->table_capacity) {
    func->table_capacity *= 2;
    func->tables = (wasmbox_table_t **) wasmbox_realloc(
        func->tables, sizeof(wasmbox_table_t *) * func->table_capacity);
    bzero(func->tables + func->table_size,
          (func->table_capacity - func->table_size) *
              sizeof(wasmbox_table_t *));
  }
  func->tables[func->table_size++] = table;
}

#define STACK_INIT_SIZE 4

static void
wasmbox_function_stack_expand_if_needed(wasmbox_mutable_function_t *func) {
  if (func->operand_stack == NULL) {
    func->operand_stack = (wasm_s16_t *) wasmbox_malloc(
        sizeof(*func->operand_stack) * STACK_INIT_SIZE);
    func->stack_size = 0;
    func->stack_capacity = STACK_INIT_SIZE;
  }
  if (func->stack_size + 1 == func->stack_capacity) {
    func->stack_capacity *= 2;
    func->operand_stack = (wasm_s16_t *) wasmbox_realloc(
        func->operand_stack,
        sizeof(*func->operand_stack) * func->stack_capacity);
    bzero(func->operand_stack + func->stack_size,
          (func->stack_capacity - func->stack_size) * sizeof(wasm_s16_t));
  }
}

static wasm_s16_t
wasmbox_function_push_stack(wasmbox_mutable_function_t *func) {
  wasmbox_function_stack_expand_if_needed(func);
  wasm_s16_t reg = func->stack_top++;
  func->operand_stack[func->stack_size++] = reg;
  return reg;
}

static wasm_s16_t
wasmbox_function_peek_stack(wasmbox_mutable_function_t *func) {
  if (func->stack_size == 0) {
    LOG("empty stack");
    return -1000;
  }
  return func->operand_stack[func->stack_size - 1];
}

static wasm_s16_t wasmbox_function_pop_stack(wasmbox_mutable_function_t *func) {
  wasm_s16_t reg = wasmbox_function_peek_stack(func);
  --func->stack_size;
  return reg;
}

static void wasmbox_block_link(wasmbox_mutable_function_t *func) {
  wasm_u32_t code_size = 0;

  for (wasm_u16_t i = 0; i < func->block_size; ++i) {
    wasmbox_block_t *block = &func->blocks[i];
    // Rewrite explicit jump if target block is next block.
    // BB0: ...            | BB0: ...
    //      JUMP BB1       |      nop
    // BB1: do something   | BB1: do something
    //      ...            |      ...
    if (block->code_size > 0 &&
        block->code[block->code_size - 1].h.opcode == OPCODE_JUMP) {
      wasmbox_code_t *code = &block->code[block->code_size - 1];
      wasmbox_block_t *target = &func->blocks[code->op0.index];
      if (i + 1 < func->block_size && target == &func->blocks[i + 1]) {
        code->h.opcode = OPCODE_NOP;
        block->code_size -= 1;
      }
    }
  }
  for (wasm_u16_t i = 0; i < func->block_size; ++i) {
    wasmbox_block_t *block = &func->blocks[i];
    block->start = code_size;
    code_size += block->code_size;
    block->end = code_size;
  }
  if (func->base.code_size > 0) {
    func->base.code_size += code_size;
    func->base.code = (wasmbox_code_t *) wasmbox_realloc(
        func->base.code, sizeof(wasmbox_code_t) * func->base.code_size);
  } else {
    func->base.code_size = code_size;
    func->base.code =
        (wasmbox_code_t *) wasmbox_malloc(sizeof(wasmbox_code_t) * code_size);
  }
  for (wasm_u16_t i = 0; i < func->block_size; ++i) {
    wasmbox_block_t *block = &func->blocks[i];
    for (int j = 0; j < block->code_size; ++j) {
      wasmbox_code_t *code = &block->code[j];
      enum wasm_jump_direction direction =
          (enum wasm_jump_direction) code->op2.index;
      if (code->h.opcode == OPCODE_JUMP) {
        wasmbox_block_t *target = &func->blocks[code->op0.index];
        wasm_u32_t offset =
            direction == WASM_JUMP_DIRECTION_HEAD ? target->start : target->end;
        code->op0.code = func->base.code + offset;
      } else if (code->h.opcode == OPCODE_JUMP_IF) {
        wasmbox_block_t *target = &func->blocks[code->op0.index];
        wasm_u32_t offset =
            direction == WASM_JUMP_DIRECTION_HEAD ? target->start : target->end;
        code->op0.code = func->base.code + offset;
      } else if (code->h.opcode == OPCODE_JUMP_TABLE) {
        wasm_u32_t offset;
        wasmbox_block_t *target;
        wasmbox_table_t *table = code->op0.table;
        for (int k = 0; k < table->size; ++k) {
          target = &func->blocks[table->labels[k].block_id];
          offset = target->direction == WASM_JUMP_DIRECTION_HEAD ? target->start
                                                                 : target->end;
          table->labels[k].code = func->base.code + offset;
        }
        target = &func->blocks[code->op1.index];
        offset = target->direction == WASM_JUMP_DIRECTION_HEAD ? target->start
                                                               : target->end;
        code->op1.code = func->base.code + offset;
      }
    }

    if (block->code_size > 0) {
      memcpy(func->base.code + block->start, block->code,
             sizeof(wasmbox_code_t) * block->code_size);
    }
  }
}

static int wasmbox_function_freeze(wasmbox_module_t *mod,
                                   wasmbox_mutable_function_t *func) {
  wasmbox_block_link(func);
  for (wasm_u16_t i = 0; i < func->block_size; ++i) {
    wasmbox_block_t *block = &func->blocks[i];
    if (block->code_capacity > 0) {
      wasmbox_free(block->code);
    }
  }
  wasmbox_free(func->blocks);
  if (func->stack_capacity > 0) {
    wasmbox_free(func->operand_stack);
  }
#ifdef WASMBOX_VM_USE_DIRECT_THREADED_CODE
  void **labels = (void **) mod->shared_code[0].op0.value.u64;
  for (int i = 0; i < func->base.code_size; ++i) {
    func->base.code[i].h.label = labels[func->base.code[i].h.opcode];
  }
#endif
  func->operand_stack = NULL;
  func->stack_top = -1;
  func->current_block_id = -1;
  return 0;
}

static wasm_s16_t wasmbox_block_add(wasmbox_mutable_function_t *func) {
  if (func->blocks == NULL) {
    func->blocks = (wasmbox_block_t *) wasmbox_malloc(sizeof(wasmbox_block_t));
    func->block_size = 0;
    func->block_capacity = 1;
  }
  if (func->block_size + 1 > func->block_capacity) {
    func->block_capacity *= 2;
    func->blocks = (wasmbox_block_t *) wasmbox_realloc(
        func->blocks, sizeof(wasmbox_block_t) * func->block_capacity);
  }
  wasm_s16_t block_index = func->block_size++;
  wasmbox_block_t *block = &func->blocks[block_index];
  memset(block, -1, sizeof(wasmbox_block_t));
  block->type.type = WASMBOX_BLOCK_TYPE_NONE;
  block->id = block_index;
  block->code = NULL;
  block->code_size = 0;
  block->code_capacity = 0;
  block->already_terminated = 0;
  return block_index;
}

static void wasmbox_block_switch(wasmbox_mutable_function_t *func,
                                 wasm_s16_t block_index) {
  func->current_block_id = block_index;
}

static void wasmbox_block_link_next(wasmbox_mutable_function_t *func,
                                    wasm_s16_t next_id) {
  wasmbox_block_t *block = &func->blocks[next_id];
  block->next_id = func->current_block_id;
}

static void wasmbox_block_link_parent(wasmbox_mutable_function_t *func,
                                      wasm_u16_t parent_id) {
  wasmbox_block_t *current = &func->blocks[func->current_block_id];
  current->parent_id = parent_id;
}

#define MODULE_CODE_INIT_SIZE 4
static void wasmbox_code_add(wasmbox_mutable_function_t *func,
                             wasmbox_code_t *code) {
  if (func->current_block_id == -1) {
    func->current_block_id = wasmbox_block_add(func);
  }
  wasmbox_block_t *block = &func->blocks[func->current_block_id];
  if (block->already_terminated != 0) {
    // Block is already terminated. No need to emit rest of code"
    return;
  }
  if (block->code == NULL) {
    block->code = (wasmbox_code_t *) wasmbox_malloc(sizeof(wasmbox_code_t) *
                                                    MODULE_CODE_INIT_SIZE);
    block->code_size = 0;
    block->code_capacity = MODULE_CODE_INIT_SIZE;
  }
  if (block->code_size + 1 > block->code_capacity) {
    block->code_capacity *= 2;
    block->code = (wasmbox_code_t *) wasmbox_realloc(
        block->code, sizeof(wasmbox_code_t) * block->code_capacity);
  }
  memcpy(&block->code[block->code_size++], code, sizeof(*code));
}

static void wasmbox_code_add_const(wasmbox_mutable_function_t *func,
                                   int vmopcode, wasmbox_value_t v) {
  wasmbox_code_t code;
  code.h.opcode = vmopcode;
  code.op0.reg = wasmbox_function_push_stack(func);
  code.op1.value = v;
  wasmbox_code_add(func, &code);
}

static void wasmbox_code_add_global(wasmbox_mutable_function_t *func,
                                    int vmopcode, wasm_u32_t index) {
  wasmbox_code_t code;
  code.h.opcode = vmopcode;
  code.op0.reg = wasmbox_function_push_stack(func);
  code.op1.index = index;
  wasmbox_code_add(func, &code);
}

static int wasmbox_code_add_unary_op(wasmbox_mutable_function_t *func,
                                     int vmopcode) {
  wasmbox_code_t code;
  code.h.opcode = vmopcode;
  code.op1.reg = wasmbox_function_pop_stack(func);
  code.op0.reg = wasmbox_function_push_stack(func);
  wasmbox_code_add(func, &code);
  return 0;
}

static int wasmbox_code_add_binary_op(wasmbox_mutable_function_t *func,
                                      int vmopcode) {
  wasmbox_code_t code;
  code.h.opcode = vmopcode;
  code.op2.reg = wasmbox_function_pop_stack(func);
  code.op1.reg = wasmbox_function_pop_stack(func);
  code.op0.reg = wasmbox_function_push_stack(func);
  wasmbox_code_add(func, &code);
  return 0;
}

static void wasmbox_code_add_move(wasmbox_mutable_function_t *func,
                                  wasm_s32_t from, wasm_s32_t to) {
  wasmbox_code_t code;
  code.h.opcode = OPCODE_MOVE;
  code.op0.reg = to;
  code.op1.reg = from;
  wasmbox_code_add(func, &code);
}

static void wasmbox_code_add_return(wasmbox_mutable_function_t *func) {
  wasmbox_code_t code;
  code.h.opcode = OPCODE_RETURN;
  wasmbox_code_add(func, &code);
}

static void wasmbox_code_add_load(wasmbox_mutable_function_t *func,
                                  int vmopcode, wasm_u32_t offset) {
  wasmbox_code_t code;
  code.h.opcode = vmopcode;
  code.op0.reg = wasmbox_function_push_stack(func);
  code.op1.index = offset;
  wasmbox_code_add(func, &code);
}

static void wasmbox_code_add_store(wasmbox_mutable_function_t *func,
                                   int vmopcode, wasm_u32_t offset) {
  wasmbox_code_t code;
  code.h.opcode = vmopcode;
  code.op0.index = offset;
  code.op1.reg = wasmbox_function_pop_stack(func);
  wasmbox_code_add(func, &code);
}

static void wasmbox_code_add_exit(wasmbox_mutable_function_t *func) {
  wasmbox_code_t code;
  code.h.opcode = OPCODE_EXIT;
  wasmbox_code_add(func, &code);
}

static void wasmbox_code_add_jump(wasmbox_mutable_function_t *func,
                                  int vmopcode, wasm_u32_t blockindex,
                                  enum wasm_jump_direction direction) {
  wasmbox_code_t code;
  code.h.opcode = vmopcode;
  code.op0.index = blockindex;
  if (vmopcode == OPCODE_JUMP_IF) {
    code.op1.reg = wasmbox_function_pop_stack(func);
  }
  code.op2.index = (wasm_u32_t) direction;
  wasmbox_code_add(func, &code);
  wasmbox_block_t *block = &func->blocks[func->current_block_id];
  if (vmopcode == OPCODE_JUMP) {
    block->already_terminated = 1;
  }
}

#define TYPE_EACH(FUNC)                  \
  FUNC(0x7f, WASM_TYPE_I32, I32)         \
  FUNC(0x7e, WASM_TYPE_I64, I64)         \
  FUNC(0x7d, WASM_TYPE_F32, F32)         \
  FUNC(0x7c, WASM_TYPE_F64, F64)         \
  FUNC(0x70, WASM_TYPE_FUNCREF, FUNCREF) \
  FUNC(0x6f, WASM_TYPE_EXTERNREF, EXTERNREF)

static const char *value_type_to_string(wasmbox_value_type_t type) {
  switch (type) {
#define FUNC(opcode, type_enum, type_name) \
  case type_enum:                          \
    return #type_name;
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
    fprintf(
        stdout, "%s",
        value_type_to_string(func_type->args[func_type->argument_size + i]));
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

static int parse_custom_section(wasmbox_input_stream_t *ins,
                                wasm_u64_t section_size,
                                wasmbox_module_t *mod) {
  // XXX Current we just skip custom section since it would not affect to other
  // sections.
  dump_binary(ins, section_size);
  ins->index += section_size;
  return 0;
}

static int parse_value_type(wasmbox_input_stream_t *ins,
                            wasmbox_value_type_t *type) {
  wasm_u8_t v = wasmbox_input_stream_read_u8(ins);
  switch (v) {
#define FUNC(opcode, type_enum, type_name) \
  case opcode:                             \
    *type = type_enum;                     \
    return 0;
    TYPE_EACH(FUNC)
#undef FUNC
    default:
      // unreachable
      LOG("unknown type");
      return -1;
  }
}

static int parse_type_vector(wasmbox_input_stream_t *ins, wasm_u32_t len,
                             wasmbox_value_type_t *type) {
  for (wasm_u32_t i = 0; i < len; i++) {
    if (parse_value_type(ins, &type[i]) != 0) {
      return -1;
    }
  }
  return 0;
}

static wasmbox_type_t *parse_function_type(wasmbox_input_stream_t *ins) {
  wasm_u8_t ch = wasmbox_input_stream_read_u8(ins);
  assert(ch == 0x60);
  wasm_u32_t args_size = wasmbox_parse_unsigned_leb128(
      ins->data + ins->index, &ins->index, ins->length);

  wasm_u32_t current_pos = ins->index;
  // Skip argument parsing to know total args+returns size.
  ins->index += args_size;

  wasm_u32_t ret_size = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                      &ins->index, ins->length);
  wasmbox_type_t *func_type = (wasmbox_type_t *) wasmbox_malloc(
      sizeof(*func_type) +
      sizeof(wasmbox_value_type_t *) * (args_size + ret_size));
  func_type->argument_size = args_size;
  func_type->return_size = ret_size;

  wasm_u32_t after_return_size = ins->index;
  ins->index = current_pos;
  if (parse_type_vector(ins, args_size, func_type->args) != 0) {
    return NULL;
  }

  ins->index = after_return_size;
  if (parse_type_vector(ins, func_type->return_size,
                        func_type->args + args_size) != 0) {
    return NULL;
  }
  return func_type;
}

static int parse_blocktype(wasmbox_input_stream_t *ins,
                           wasmbox_blocktype_t *type) {
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
      type->v.x = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                &ins->index, ins->length);
      return 0;
  }
}

static int parse_instruction(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                             wasmbox_mutable_function_t *func);

static int parse_expression(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                            wasmbox_mutable_function_t *func) {
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

static int eval_expression(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                           wasmbox_value_t *result) {
  wasmbox_value_t stack[8];
  wasmbox_mutable_function_t func = {};
  func.current_block_id = -1;
  if (parse_expression(ins, mod, &func) < 0) {
    return -1;
  }
  wasmbox_code_add_move(&func, wasmbox_function_pop_stack(&func), -1);
  wasmbox_code_add_exit(&func);
  wasmbox_function_freeze(mod, &func);
  wasmbox_eval_function(mod, func.base.code, stack + 1);
  *result = stack[0];
  if (func.base.code_size > 0) {
    wasmbox_free(func.base.code);
  }
  return 0;
}

static void print_block_type(const char *prefix, wasmbox_blocktype_t *type) {
  switch (type->type) {
    case WASMBOX_BLOCK_TYPE_NONE:
      break;
    case WASMBOX_BLOCK_TYPE_VAL:
      fprintf(stdout, "%s (type: %s)\n", prefix,
              value_type_to_string(type->v.t));
      break;
    case WASMBOX_BLOCK_TYPE_INDEX:
      fprintf(stdout, "%s (index: %lld)\n", prefix, type->v.x);
      break;
  }
}

// INST(0x02 bt:blocktype (in:instr)* 0x0B, block bt in* end)
// INST(0x03 bt:blocktype (in:instr)* 0x0B, loop bt in* end)
static int decode_block(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                        wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasmbox_blocktype_t blocktype;
  if (parse_blocktype(ins, &blocktype)) {
    return -1;
  }
  enum wasm_jump_direction direction;
  switch (op) {
    case 0x02:
      print_block_type("block", &blocktype);
      direction = WASM_JUMP_DIRECTION_TAIL;
      break;
    case 0x03:
      print_block_type("loop", &blocktype);
      direction = WASM_JUMP_DIRECTION_HEAD;
      break;
    default:
      return -1;
  }
  wasm_s16_t current_block = func->current_block_id;
  wasm_s16_t block_body = wasmbox_block_add(func);
  wasmbox_block_t *body = &func->blocks[block_body];
  body->direction = direction;
  wasm_s16_t block_then = wasmbox_block_add(func);

  wasm_s16_t block_value = -1;
  if (blocktype.type == WASMBOX_BLOCK_TYPE_VAL) {
    block_value = wasmbox_function_push_stack(func);
  }

  wasmbox_code_add_jump(func, OPCODE_JUMP, block_body,
                        WASM_JUMP_DIRECTION_HEAD);
  wasmbox_block_switch(func, block_body);
  wasmbox_block_link_parent(func, current_block);
  int parsed = parse_expression(ins, mod, func);
  if (blocktype.type == WASMBOX_BLOCK_TYPE_VAL) {
    wasmbox_code_add_move(func, wasmbox_function_pop_stack(func), block_value);
  }
  wasmbox_code_add_jump(func, OPCODE_JUMP, block_then,
                        WASM_JUMP_DIRECTION_HEAD);
  wasmbox_block_switch(func, block_then);
  wasmbox_block_link_next(func, current_block);
  return parsed;
}

// INST(0x05, end)
static int decode_block_end(wasmbox_input_stream_t *in, wasmbox_module_t *mod,
                            wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasmbox_block_t *block = &func->blocks[func->current_block_id];
  if (func->base.type->return_size > 0 && block->already_terminated == 0) {
    wasmbox_code_add_move(func, wasmbox_function_pop_stack(func), -1);
  }
  wasmbox_code_add_return(func);

  return 0;
}

// INST(0x04 bt:blocktype (in:instr)* 0x0B, if bt in* end)
// INST(0x04 bt:blocktype (in:instr)* 0x05 (in2:instr)* 0x0B, if bt in1* else
// in2* end)
static int decode_if(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                     wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasmbox_blocktype_t blocktype;
  if (parse_blocktype(ins, &blocktype)) {
    return -1;
  }
  print_block_type("if", &blocktype);
  wasmbox_code_t code;
  wasm_s16_t current_block = func->current_block_id;
  wasm_s16_t block_then = wasmbox_block_add(func);
  wasm_s16_t block_else = wasmbox_block_add(func);
  wasm_s16_t block_cont = wasmbox_block_add(func);

  wasmbox_block_switch(func, block_then);
  wasmbox_block_link_parent(func, current_block);
  wasm_s16_t block_value = -1;
  if (blocktype.type == WASMBOX_BLOCK_TYPE_VAL) {
    block_value = wasmbox_function_push_stack(func);
  }
  while (1) {
    wasm_u8_t next = wasmbox_input_stream_peek_u8(ins);
    if (next == 0x05) { // else
      wasmbox_input_stream_read_u8(ins);
      if (blocktype.type == WASMBOX_BLOCK_TYPE_VAL) {
        wasmbox_code_add_move(func, wasmbox_function_pop_stack(func),
                              block_value);
      }
      wasmbox_code_add_jump(func, OPCODE_JUMP, block_cont,
                            WASM_JUMP_DIRECTION_HEAD);
      wasmbox_block_switch(func, block_else);
      wasmbox_block_link_parent(func, current_block);
      continue;
    }
    if (next == 0x0B) { // endif
      wasmbox_input_stream_read_u8(ins);
      if (blocktype.type == WASMBOX_BLOCK_TYPE_VAL) {
        wasmbox_code_add_move(func, wasmbox_function_pop_stack(func),
                              block_value);
      }
      wasmbox_code_add_jump(func, OPCODE_JUMP, block_cont,
                            WASM_JUMP_DIRECTION_HEAD);
      wasmbox_block_switch(func, block_cont);
      wasmbox_block_link_next(func, current_block);
      break;
    }
    if (parse_instruction(ins, mod, func)) {
      return -1;
    }
  }
  return 0;
}

static wasmbox_block_t *resolve_target_block(wasmbox_mutable_function_t *func,
                                             wasm_u64_t label) {
  wasmbox_block_t *block = &func->blocks[func->current_block_id];
  for (wasm_u64_t i = 0; i < label; ++i) {
    wasm_u16_t parent = block->parent_id;
    block = &func->blocks[parent];
  }
  assert(block != NULL);
  return block;
}

// INST(0x0C l:labelidx, br l)
static int decode_br(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                     wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasm_u64_t labelidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                      &ins->index, ins->length);
  wasmbox_block_t *block = resolve_target_block(func, labelidx);
  wasmbox_code_add_jump(func, OPCODE_JUMP, block->id, block->direction);
  return 0;
}

// INST(0x0D l:labelidx, br_if l)
static int decode_br_if(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                        wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasm_u64_t labelidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                      &ins->index, ins->length);
  wasmbox_block_t *block = resolve_target_block(func, labelidx);
  wasmbox_code_add_jump(func, OPCODE_JUMP_IF, block->id, block->direction);
  return 0;
}

// INST(0x0E l:vec(labelidx) lN:labelidx, br_table l* lN)
static int decode_br_table(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                           wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  wasmbox_table_t *table = (wasmbox_table_t *) wasmbox_malloc(
      sizeof(wasmbox_table_t) + sizeof(wasmbox_code_t *) * len);
  table->size = len;
  wasmbox_function_add_table(func, table);

  for (wasm_u64_t i = 0; i < len; i++) {
    wasm_u64_t labelidx = wasmbox_parse_unsigned_leb128(
        ins->data + ins->index, &ins->index, ins->length);
    wasmbox_block_t *block = resolve_target_block(func, labelidx);
    table->labels[i].block_id = block->id;
  }
  wasm_u64_t default_label = wasmbox_parse_unsigned_leb128(
      ins->data + ins->index, &ins->index, ins->length);
  wasmbox_block_t *default_block = resolve_target_block(func, default_label);

  wasmbox_code_t code;
  code.h.opcode = OPCODE_JUMP_TABLE;
  code.op0.table = table;
  code.op1.index = default_block->id;
  code.op2.reg = wasmbox_function_pop_stack(func);
  wasmbox_code_add(func, &code);
  return 0;
}

// INST(0x0F, return)
static int decode_return(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                         wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasmbox_block_t *block = &func->blocks[func->current_block_id];
  if (func->base.type->return_size > 0 && block->already_terminated == 0) {
    wasmbox_code_add_move(func, wasmbox_function_pop_stack(func), -1);
  }
  wasmbox_code_add_return(func);
  return 0;
}

static wasm_u16_t setup_params(wasmbox_mutable_function_t *func,
                               wasmbox_type_t *type) {
  wasm_u16_t stack_top = func->stack_top;
  wasm_u16_t argument_to =
      stack_top + type->return_size + WASMBOX_FUNCTION_CALL_OFFSET;
  for (int i = 0; i < type->argument_size; ++i) {
    wasmbox_code_add_move(func, wasmbox_function_pop_stack(func),
                          argument_to + i);
  }
  return stack_top;
}

// BLOCK_INST(0x11, x:typeidx 0x00, call_indirect x)
static int decode_call(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                       wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasm_u64_t funcidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                     &ins->index, ins->length);
  wasmbox_function_t *call = mod->functions[funcidx];
  if (call == NULL) {
    LOG("Failed to find function\n");
    return -1;
  }
  wasm_u16_t stack_top = setup_params(func, call->type);
  wasmbox_code_t code;
  code.h.opcode = OPCODE_STATIC_CALL;
  code.op0.reg = stack_top;
  for (int i = 0; i < call->type->return_size; ++i) {
    wasmbox_function_push_stack(func);
  }
  code.op1.func = mod->functions[funcidx];
  code.op2.index = call->type->return_size;
  wasmbox_code_add(func, &code);
  return 0;
}

// INST(0x11 x:tableidx, y:typeidx call_indirect x)
static int decode_call_indirect(wasmbox_input_stream_t *ins,
                                wasmbox_module_t *mod,
                                wasmbox_mutable_function_t *func,
                                wasm_u8_t op) {
  wasm_u64_t typeidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                     &ins->index, ins->length);
  wasm_u64_t tableidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                      &ins->index, ins->length);
  wasmbox_type_t *type = mod->types[typeidx];
  wasm_u16_t stack_top = setup_params(func, type);

  wasmbox_code_t code;
  code.h.opcode = OPCODE_DYNAMIC_CALL;
  code.op0.reg = stack_top;
  for (int i = 0; i < type->return_size; ++i) {
    wasmbox_function_push_stack(func);
  }
  code.op1.index = tableidx;
  code.op2.index = type->return_size;
  wasmbox_code_add(func, &code);
  return 0;
}

static int decode_variable_inst(wasmbox_input_stream_t *ins,
                                wasmbox_module_t *mod,
                                wasmbox_mutable_function_t *func,
                                wasm_u8_t op) {
  wasm_u64_t idx = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  wasm_s16_t reg;
  switch (op) {
    case 0x20: // local.get
      wasmbox_code_add_move(func, WASMBOX_FUNCTION_CALL_OFFSET + idx,
                            wasmbox_function_push_stack(func));
      return 0;
    case 0x21: // local.set
      wasmbox_code_add_move(func, wasmbox_function_pop_stack(func),
                            WASMBOX_FUNCTION_CALL_OFFSET + idx);
      return 0;
    case 0x22: // local.tee
      wasmbox_code_add_move(func, wasmbox_function_peek_stack(func),
                            WASMBOX_FUNCTION_CALL_OFFSET + idx);
      return 0;
    case 0x23: // global.get
      wasmbox_code_add_global(func, OPCODE_GLOBAL_GET, idx);
      return 0;
    case 0x24: // global.set
      wasmbox_code_add_global(func, OPCODE_GLOBAL_SET, idx);
      return 0;
    default:
      return -1;
  }
  return -1;
}

static int decode_table_inst(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                             wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasm_u32_t tableidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                      &ins->index, ins->length);
  switch (op) {
    case 0x25: // table.get x
      break;
    case 0x26: // table.set x
      break;
  }
  return -1;
}

static int parse_memarg(wasmbox_input_stream_t *ins, wasm_u32_t *align,
                        wasm_u32_t *offset) {
  *align = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index,
                                         ins->length);
  *offset = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index,
                                          ins->length);
  return 0;
}

static int decode_memory_inst(wasmbox_input_stream_t *ins,
                              wasmbox_module_t *mod,
                              wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasm_u32_t align;
  wasm_u32_t offset;
  if (parse_memarg(ins, &align, &offset)) {
    return -1;
  }
  switch (op) {
#define FUNC(opcode, out_type, in_type, inst, vmopcode) \
  case (opcode): {                                      \
    wasmbox_code_add_##inst(func, vmopcode, offset);    \
    break;                                              \
  }
    MEMORY_INST_EACH(FUNC)
#undef FUNC
    default:
      return -1;
  }
  return 0;
}

static int decode_memory_size_and_grow(wasmbox_input_stream_t *ins,
                                       wasmbox_module_t *mod,
                                       wasmbox_mutable_function_t *func,
                                       wasm_u8_t op) {
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
      code.op1.reg = wasmbox_function_pop_stack(func);
      break;
    default:
      return -1;
  }
  code.op0.reg = wasmbox_function_push_stack(func);
  wasmbox_code_add(func, &code);
  return 0;
}

static void parse_i32_const(wasmbox_input_stream_t *ins, wasmbox_value_t *v) {
  (*v).s32 = wasmbox_parse_signed_leb128(ins->data + ins->index, &ins->index,
                                         ins->length);
}

static void parse_i64_const(wasmbox_input_stream_t *ins, wasmbox_value_t *v) {
  (*v).s64 = wasmbox_parse_signed_leb128(ins->data + ins->index, &ins->index,
                                         ins->length);
}

static void parse_f32_const(wasmbox_input_stream_t *ins, wasmbox_value_t *v) {
  (*v).f32 = *(wasm_f32_t *) (ins->data + ins->index);
  ins->index += sizeof(wasm_f32_t);
}

static void parse_f64_const(wasmbox_input_stream_t *ins, wasmbox_value_t *v) {
  (*v).f64 = *(wasm_f64_t *) (ins->data + ins->index);
  ins->index += sizeof(wasm_f64_t);
}

/* Numeric Instructions */
static int decode_constant_inst(wasmbox_input_stream_t *ins,
                                wasmbox_module_t *mod,
                                wasmbox_mutable_function_t *func,
                                wasm_u8_t op) {
  wasmbox_value_t v;
  switch (op) {
#define FUNC(opcode, type, inst, attr, vmopcode) \
  case (opcode): {                               \
    parse_##inst(ins, &v);                       \
    wasmbox_code_add_const(func, vmopcode, v);   \
    return 0;                                    \
  }
    CONST_OP_EACH(FUNC)
#undef FUNC
    default:
      return -1;
  }
  return -1;
}

static int decode_op0_inst(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                           wasmbox_mutable_function_t *func, wasm_u8_t op) {
  wasmbox_code_t code;
  switch (op) {
    case 0x00: // unreachable
      code.h.opcode = OPCODE_UNREACHABLE;
      wasmbox_code_add(func, &code);
      wasmbox_block_t *block = &func->blocks[func->current_block_id];
      block->already_terminated = 1;
      return 0;
    case 0x01: // nop
      return 0;
    case 0x1A: // drop
      // Just pop single operand without emitting code.
      wasmbox_function_pop_stack(func);
      return 0;
    case 0x1B: // select
      code.h.opcode = OPCODE_SELECT;
      code.op1.reg = wasmbox_function_pop_stack(func);
      code.op2.r.reg2 = wasmbox_function_pop_stack(func);
      code.op2.r.reg1 = wasmbox_function_pop_stack(func);
      code.op0.reg = wasmbox_function_push_stack(func);
      wasmbox_code_add(func, &code);
      return 0;

#define FUNC(op, param, type, inst, vmopcode) \
  case (op):                                  \
    return wasmbox_code_add_##param##_op(func, vmopcode);
      NUMERIC_INST_EACH(FUNC)
#undef FUNC
    default:
      return -1;
  }
  return -1;
}

static int decode_truncation_inst(wasmbox_input_stream_t *ins,
                                  wasmbox_module_t *mod,
                                  wasmbox_mutable_function_t *func,
                                  wasm_u8_t op) {
  wasm_u8_t op1 = wasmbox_input_stream_read_u8(ins);
  switch (op1) {
#define FUNC(opcode0, opcode1, type, inst, vmopcode) \
  case opcode1: {                                    \
    fprintf(stdout, "" #type "." #inst "\n");        \
    return 0;                                        \
  }
    SATURATING_TRUNCATION_INST_EACH(FUNC)
#undef FUNC
    default:
      return -1;
  }
  return -1;
}

static int decode_undefined_op(wasmbox_input_stream_t *ins,
                               wasmbox_module_t *mod,
                               wasmbox_mutable_function_t *func, wasm_u8_t op) {
  LOG("undefined op code");
  return -1;
}

static const wasm_u8_t decoder_table[] = {
    1,  1,  2,  2,  3,  0,  0,  0,  0,  0,  0,  4,  5,  6,  7,  8,  9,  10, 0,
    0,  0,  0,  0,  0,  0,  0,  11, 11, 0,  0,  0,  0,  12, 12, 12, 12, 12, 0,
    0,  0,  13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  17, 0,  0,  0,
};

static const wasmbox_op_decode_func_t decode_funcs[] = {
    decode_undefined_op,
    decode_op0_inst,
    decode_block,
    decode_if,
    decode_block_end,
    decode_br,
    decode_br_if,
    decode_br_table,
    decode_return,
    decode_call,
    decode_call_indirect,
    decode_op0_inst,
    decode_variable_inst,
    decode_memory_inst,
    decode_memory_size_and_grow,
    decode_constant_inst,
    decode_op0_inst,
    decode_truncation_inst};

static int parse_instruction(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                             wasmbox_mutable_function_t *func) {
  wasm_u8_t op = wasmbox_input_stream_read_u8(ins);
  const wasmbox_op_decode_func_t decorder = decode_funcs[decoder_table[op]];
  return decorder(ins, mod, func, op);
}

static int parse_code(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                      wasmbox_mutable_function_t *func, wasm_u64_t codelen) {
  wasm_u64_t end = ins->index + codelen;
  while (ins->index < end) {
    if (parse_instruction(ins, mod, func)) {
      return -1;
    }
  }
  return 0;
}

static int parse_local_variable(wasmbox_input_stream_t *ins, wasm_u64_t *index,
                                wasmbox_value_type_t *valtype) {
  *index = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index,
                                         ins->length);
  return parse_value_type(ins, valtype);
}

static int parse_local_variables(wasmbox_input_stream_t *ins,
                                 wasmbox_mutable_function_t *func) {
  wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  for (wasm_u64_t i = 0; i < len; i++) {
    wasm_u64_t localidx;
    wasmbox_value_type_t type;
    if (parse_local_variable(ins, &localidx, &type)) {
      return -1;
    }
    func->base.locals += localidx;
  }
  func->stack_top += func->base.locals;
  return 0;
}

static int parse_function(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                          wasm_u32_t funcindex) {
  wasmbox_mutable_function_t *func =
      (wasmbox_mutable_function_t *) mod->functions[funcindex];
  wasm_u64_t size = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                  &ins->index, ins->length);
  wasm_u64_t index = ins->index;
#if 0
  fprintf(stdout, "code(size:%llu)\n", size);
    dump_binary(ins, size);
#endif
  if (parse_local_variables(ins, func)) {
    return -1;
  }
  size -= ins->index - index;
  // Create entry block.
  wasmbox_block_switch(func, wasmbox_block_add(func));
  int parsed = parse_code(ins, mod, func, size);
  if (parsed == 0) {
    wasmbox_function_freeze(mod, func);
  }
  return parsed;
}

static int parse_type_section(wasmbox_input_stream_t *ins,
                              wasm_u64_t section_size, wasmbox_module_t *mod) {
  wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  for (wasm_u64_t i = 0; i < len; i++) {
    wasmbox_type_t *func_type = NULL;
    if ((func_type = parse_function_type(ins)) == NULL) {
      return -1;
    }
    wasmbox_module_register_new_type(mod, func_type);
  }
  return 0;
}

static int parse_name(wasmbox_input_stream_t *ins, wasmbox_name_t **name) {
  wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  wasmbox_name_t *n =
      (wasmbox_name_t *) wasmbox_malloc(sizeof(wasmbox_name_t) + len);
  n->len = len;
  for (wasm_u64_t i = 0; i < len; i++) {
    n->value[i] = wasmbox_input_stream_read_u8(ins);
  }
  *name = n;
  return 0;
}

static int parse_limit(wasmbox_input_stream_t *ins, wasmbox_limit_t *limit) {
  wasm_u8_t has_upper_limit = wasmbox_input_stream_read_u8(ins);
  limit->min = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                             &ins->index, ins->length);
  if (has_upper_limit) {
    limit->max = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                               &ins->index, ins->length);
  } else {
    limit->max = WASM_U32_MAX;
  }
  return 0;
}

static int parse_import_description(wasmbox_input_stream_t *ins,
                                    wasmbox_module_t *mod) {
  wasm_u8_t type = wasmbox_input_stream_read_u8(ins);
  wasm_u32_t v;
  wasmbox_limit_t limit;
  wasmbox_value_type_t value_type;
  switch (type) {
    case 0x00: // func x:typeidx
      v = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index,
                                        ins->length);
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
      assert(type == 0x00 /* const */ || type == 0x01 /* var */ ||
             type == 0x02 /* mutable */);
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
  fprintf(stdout, "import(%.*s:%.*s)\n", module_name->len, module_name->value,
          ns_name->len, ns_name->value);
  wasmbox_free(module_name);
  wasmbox_free(ns_name);
  return 0;
}

static int parse_import_section(wasmbox_input_stream_t *ins,
                                wasm_u64_t section_size,
                                wasmbox_module_t *mod) {
  wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  fprintf(stdout, "import(num:%llu)\n", len);
  for (wasm_u64_t i = 0; i < len; i++) {
    if (parse_import(ins, mod)) {
      return -1;
    }
  }
  return 0;
}

static int parse_function_section(wasmbox_input_stream_t *ins,
                                  wasm_u64_t section_size,
                                  wasmbox_module_t *mod) {
  wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  for (wasm_u64_t i = 0; i < len; i++) {
    wasm_u32_t v = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
    wasmbox_mutable_function_t *func =
        (wasmbox_mutable_function_t *) wasmbox_malloc(sizeof(*func));
    func->base.type = mod->types[v];
    func->stack_top =
        WASMBOX_FUNCTION_CALL_OFFSET + func->base.type->argument_size;
    func->current_block_id = -1;
    wasmbox_module_register_new_function(mod, func);
  }
  return 0;
}

static int parse_table_section(wasmbox_input_stream_t *ins,
                               wasm_u64_t section_size, wasmbox_module_t *mod) {
  wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  for (wasm_u64_t i = 0; i < len; i++) {
    wasmbox_value_type_t type;
    wasmbox_limit_t limit;
    if (parse_value_type(ins, &type) != 0) {
      return -1;
    }
    if (parse_limit(ins, &limit) != 0) {
      return -1;
    }
  }
  mod->table_size = len;
  mod->tables =
      (wasmbox_table_t **) wasmbox_malloc(sizeof(wasmbox_table_t *) * len);
  return 0;
}

static int parse_memory_section(wasmbox_input_stream_t *ins,
                                wasm_u64_t section_size,
                                wasmbox_module_t *mod) {
  wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
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

static int parse_global_variable(wasmbox_input_stream_t *ins,
                                 wasmbox_module_t *mod,
                                 wasmbox_mutable_function_t *global) {
  wasmbox_value_type_t valtype;
  if (parse_value_type(ins, &valtype) != 0) {
    return -1;
  }
  wasm_u8_t mut = wasmbox_input_stream_read_u8(ins);
  int is_const = mut == 0x01;
  if (mut != 0x00 /*const*/ && mut != 0x01 /*var*/) {
    LOG("unreachable");
    return -1;
  }

  if (parse_expression(ins, mod, global) < 0) {
    return -1;
  }
  return 0;
}

static int parse_global_section(wasmbox_input_stream_t *ins,
                                wasm_u64_t section_size,
                                wasmbox_module_t *mod) {
  wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  if (len > 0) {
    mod->globals = wasmbox_malloc(sizeof(*mod->globals) * len);
    mod->global_size = len;
  }
  wasmbox_mutable_function_t *global =
      (wasmbox_mutable_function_t *) mod->global_function;
  if (global == NULL) {
    static const char global_name[] = "__global__";
    static const wasm_u32_t global_name_len = 10;
    global = (wasmbox_mutable_function_t *) wasmbox_malloc(
        sizeof(wasmbox_mutable_function_t));
    global->current_block_id = -1;
    global->base.name = (wasmbox_name_t *) wasmbox_malloc(
        sizeof(wasmbox_name_t) + global_name_len);
    global->base.name->len = global_name_len;
    memcpy(global->base.name->value, global_name, global_name_len);

    mod->global_function = &global->base;
  }
  for (wasm_u64_t i = 0; i < len; i++) {
    if (parse_global_variable(ins, mod, global) != 0) {
      return -1;
    }
  }
  wasmbox_code_add_exit(global);
  wasmbox_function_freeze(mod, global);
  return 0;
}

static int parse_export_entry(wasmbox_input_stream_t *ins,
                              wasmbox_module_t *mod) {
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
  wasm_u32_t index = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                   &ins->index, ins->length);
  if (type != 0x00) {
#if 0
    fprintf(stdout, "- export '%.*s' %s(%d)\n", name->len, name->value, debug_name, index);
#endif
    wasmbox_free(name);
  } else {
    mod->functions[index]->name = name;
  }
  return 0;
}

static int parse_export_section(wasmbox_input_stream_t *ins,
                                wasm_u64_t section_size,
                                wasmbox_module_t *mod) {
  wasm_u64_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  for (wasm_u64_t i = 0; i < len; i++) {
    if (parse_export_entry(ins, mod) != 0) {
      return -1;
    }
  }
  return 0;
}

static int parse_start_section(wasmbox_input_stream_t *ins,
                               wasm_u64_t section_size, wasmbox_module_t *mod) {
  fprintf(stdout, "start\n");
  dump_binary(ins, section_size);
  ins->index += section_size;
  return 0;
}

static int parse_func_index_vector(wasmbox_input_stream_t *ins,
                                   wasmbox_module_t *mod, wasm_u32_t tableidx,
                                   wasm_u32_t offset) {
  wasm_u32_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  assert(tableidx < mod->table_size);
  wasmbox_table_t *table = mod->tables[tableidx];
  if (table == NULL) {
    table = (wasmbox_table_t *) wasmbox_malloc(sizeof(wasmbox_table_t) +
                                               sizeof(wasmbox_code_t *) * len);
    mod->tables[tableidx] = table;
  }
  for (wasm_u32_t i = 0; i < len; i++) {
    wasm_u32_t funcidx = wasmbox_parse_unsigned_leb128(
        ins->data + ins->index, &ins->index, ins->length);
    if (funcidx > mod->function_size) {
      LOG("table: out of index");
      return -1;
    }
    table->labels[i].func = mod->functions[funcidx];
  }
  return 0;
}

static int parse_element(wasmbox_input_stream_t *ins, wasmbox_module_t *mod,
                         wasm_u32_t id) {
  wasm_u8_t type = wasmbox_input_stream_read_u8(ins);
  uint8_t elemkind = -1;
  wasm_u32_t tableidx = 0;
  enum table_mode { TABLE_ACTIVE, TABLE_DECLARATIVE, TABLE_PASSIVE } mode;
  wasmbox_value_t offset;
  offset.u32 = 0;
  switch (type) {
    case 0x00:
      // e:expr y*:vec(funcidx)
      // {type funcref, init ((ref.func y) end)*, mode active
      //   {table 0, offset e}}
      if (eval_expression(ins, mod, &offset) < 0) {
        return -1;
      }
      return parse_func_index_vector(ins, mod, tableidx, offset.u32);
    case 0x01:
      // et: elemkind y*:vec(funcidx)
      // {type et, init ((ref.func y) end)*, mode passive}
      elemkind = wasmbox_input_stream_read_u8(ins);
      assert(elemkind == 0);
      return parse_func_index_vector(ins, mod, tableidx, 0);
    case 0x02:
      // x:tableidx e:expr et : elemkind y*:vec(funcidx)
      // {type et, init ((ref.func y) end)*, mode active {table x, offset e}}
      tableidx = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                               &ins->index, ins->length);
      if (eval_expression(ins, mod, &offset) < 0) {
        return -1;
      }
      elemkind = wasmbox_input_stream_read_u8(ins);
      assert(elemkind == 0);
      return parse_func_index_vector(ins, mod, tableidx, offset.u32);
    case 0x03:
      // et: elemkind y*:vec(funcidx)
      // {type et, init ((ref.func 𝑦) end)*, mode declarative}
      elemkind = wasmbox_input_stream_read_u8(ins);
      assert(elemkind == 0);
      return parse_func_index_vector(ins, mod, tableidx, offset.u32);
    case 0x04:
      // e:expr el* :vec(expr)
      // {type funcref, init el*, mode active {table 0, offset e}}
      if (eval_expression(ins, mod, &offset) < 0) {
        return -1;
      }
      // return parse_expression_vector();
    case 0x05:
      // et: reftype el*:vec(expr)
      // {type et, init el*, mode passive}
    case 0x06:
      // x:tableidx e:expr et: reftype el* :vec(expr)
      // {type et, init el*, mode active {table x, offset e}}
    case 0x07:
      // et : reftype e * :vec(expr)
      break;
    default:
      return -1;
  }
  return -1;
}

static int parse_element_section(wasmbox_input_stream_t *ins,
                                 wasm_u64_t section_size,
                                 wasmbox_module_t *mod) {
  wasm_u32_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  for (wasm_u32_t i = 0; i < len; i++) {
    if (parse_element(ins, mod, i)) {
      return -1;
    }
  }
  return 0;
}

static int parse_code_section(wasmbox_input_stream_t *ins,
                              wasm_u64_t section_size, wasmbox_module_t *mod) {
  wasm_u32_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
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
  wasmbox_mutable_function_t func = {};
  func.current_block_id = -1;

  wasmbox_value_t offset;
  offset.u32 = 0;
  switch (type) {
    case 0x02: // active with memory index
      index = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index,
                                            ins->length);
      assert(index == 0);
      /* fallthrough */
    case 0x00: // active without memory index
      if (eval_expression(ins, mod, &offset) < 0) {
        return -1;
      }
      /* fallthrough */
    case 0x01: // passive
      len = wasmbox_parse_unsigned_leb128(ins->data + ins->index, &ins->index,
                                          ins->length);
      memcpy(mod->memory_block->data + offset.u32, ins->data + ins->index, len);
      ins->index += len;
      break;
    default:
      return -1;
  }
  return 0;
}

static int parse_data_section(wasmbox_input_stream_t *ins,
                              wasm_u64_t section_size, wasmbox_module_t *mod) {
  wasm_u32_t len = wasmbox_parse_unsigned_leb128(ins->data + ins->index,
                                                 &ins->index, ins->length);
  for (wasm_u32_t i = 0; i < len; i++) {
    if (parse_data(ins, mod)) {
      return -1;
    }
  }
  return 0;
}

typedef int (*section_parse_func_t)(wasmbox_input_stream_t *ins,
                                    wasm_u64_t section_size,
                                    wasmbox_module_t *mod);

struct section_parser {
  const char *name;
  section_parse_func_t func;
};

static const struct section_parser section_parser[] = {
    {"custom", parse_custom_section}, {"type", parse_type_section},
    {"import", parse_import_section}, {"function", parse_function_section},
    {"table", parse_table_section},   {"memory", parse_memory_section},
    {"global", parse_global_section}, {"export", parse_export_section},
    {"start", parse_start_section},   {"element", parse_element_section},
    {"code", parse_code_section},     {"data", parse_data_section},
};

static int parse_section(wasmbox_input_stream_t *ins, wasmbox_module_t *mod) {
  wasm_u8_t section_type = wasmbox_input_stream_read_u8(ins);
  assert(0 <= section_type && section_type <= 11);
  wasm_u64_t section_size = wasmbox_parse_unsigned_leb128(
      ins->data + ins->index, &ins->index, ins->length);
#if 0
  fprintf(stdout, "type=%d(%s), section_size=%llu\n", section_type, section_parser[section_type].name, section_size);
#endif
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

static void wasmbox_module_dump(wasmbox_module_t *mod) {
  fprintf(stdout, "module %p {\n", mod);
  if (mod->memory_block_size > 0) {
    fprintf(stdout, "  mem(%p, current=%u, max=%u)\n", mod->memory_block,
            mod->memory_block_size, mod->memory_block_capacity);
  }
  if (mod->global_function) {
    print_function(mod->global_function, 0);
    fprintf(stdout, "\n");
  }
  if (mod->global_size) {
    fprintf(stdout, "global variables: %d\n", mod->global_size);
  }
  for (wasm_u32_t i = 0; i < mod->function_size; ++i) {
    wasmbox_function_t *f = mod->functions[i];
    print_function(f, i);
    fprintf(stdout, " {\n");
    wasmbox_dump_function(f->code, f->code + f->code_size, "  ");
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
  wasmbox_virtual_machine_init(mod);
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
    wasmbox_mutable_function_t *func =
        (wasmbox_mutable_function_t *) mod->functions[i];
    if (func->base.name != NULL) {
      wasmbox_free(func->base.name);
    }
    wasmbox_free(func->base.code);
    for (int j = 0; j < func->table_size; ++j) {
      wasmbox_free(func->tables[j]);
    }
    if (func->table_size > 0) {
      wasmbox_free(func->tables);
    }
    wasmbox_free(func);
  }
  wasmbox_free(mod->functions);
  if (mod->table_size > 0) {
    for (int i = 0; i < mod->table_size; ++i) {
      wasmbox_free(mod->tables[i]);
    }
    wasmbox_free(mod->tables);
  }
  if (mod->global_function) {
    wasmbox_mutable_function_t *func =
        (wasmbox_mutable_function_t *) mod->global_function;
    wasmbox_free(func->base.name);
    wasmbox_free(func->base.code);
    wasmbox_free(func);
  }
  if (mod->global_size > 0) {
    wasmbox_free(mod->globals);
  }
  if (mod->memory_block) {
    wasmbox_free(mod->memory_block);
  }
  return 0;
}
