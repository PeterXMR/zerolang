#ifndef ZERO_C_X64_EMIT_H
#define ZERO_C_X64_EMIT_H

#include "zero.h"

#include <stdint.h>

void z_x64_append_u8(ZBuf *buf, unsigned value);
void z_x64_append_u32(ZBuf *buf, uint32_t value);
void z_x64_append_u64(ZBuf *buf, uint64_t value);
void z_x64_patch_u32(ZBuf *buf, size_t offset, uint32_t value);
void z_x64_patch_rel32(ZBuf *buf, size_t patch_offset, size_t target_offset);
size_t z_x64_emit_jmp32_placeholder(ZBuf *buf, unsigned opcode);
size_t z_x64_emit_jcc32_placeholder(ZBuf *buf, unsigned condition);
void z_x64_emit_rbp_disp_reg(ZBuf *buf, unsigned opcode, unsigned reg, unsigned offset, bool wide);
void z_x64_emit_load_rbp_positive_reg(ZBuf *buf, unsigned reg, unsigned offset, bool wide);
void z_x64_emit_push_reg64(ZBuf *buf, unsigned reg);
void z_x64_emit_pop_reg64(ZBuf *buf, unsigned reg);
void z_x64_emit_sub_rsp(ZBuf *buf, unsigned amount);
void z_x64_emit_add_rsp(ZBuf *buf, unsigned amount);

#endif
