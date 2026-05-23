#ifndef ZERO_C_BUILDABILITY_INTERNAL_H
#define ZERO_C_BUILDABILITY_INTERNAL_H

#include "buildability.h"

typedef enum {
  Z_BUILD_BACKEND_NONE,
  Z_BUILD_BACKEND_ELF64,
  Z_BUILD_BACKEND_ELF_AARCH64,
  Z_BUILD_BACKEND_MACHO64,
  Z_BUILD_BACKEND_COFF_X64
} ZBuildBackend;

typedef struct {
  const ZTargetInfo *target;
  const char *emit_kind;
  const char *backend_name;
  const char *expected;
  const char *help;
  ZBuildBackend backend;
} ZBuildability;

enum {
  BUILD_MACHO_SCRATCH_SLOT_COUNT = 32u
};

const char *z_build_type_name(IrTypeKind type);
const char *z_build_value_kind_name(IrValueKind kind);
bool z_build_is_elf_scalar(IrTypeKind type);
bool z_build_is_scalar32(IrTypeKind type);
bool z_build_diag(const ZBuildability *ctx, ZDiag *diag, const char *message, int line, int column, const char *actual);
bool z_build_select(const IrProgram *ir, const ZTargetInfo *target, const char *emit_kind, ZBuildability *ctx, ZDiag *diag);
bool z_build_check_coff_byte_view_len(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag);
bool z_build_check_coff_byte_view(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag);
bool z_build_check_macho_byte_view_len(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag);
bool z_build_check_macho_byte_view(const ZBuildability *ctx, const IrFunction *fun, const IrValue *view, ZDiag *diag);
bool z_build_check_aarch64_literal_shape(const ZBuildability *ctx, const IrFunction *fun, ZDiag *diag);

#endif
