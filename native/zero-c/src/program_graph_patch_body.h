#ifndef ZERO_C_PROGRAM_GRAPH_PATCH_BODY_H
#define ZERO_C_PROGRAM_GRAPH_PATCH_BODY_H

#include "program_graph_patch.h"

bool z_program_graph_patch_apply_replace_function_body(ZProgramGraph *graph, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op);
bool z_program_graph_patch_apply_replace_block_body(ZProgramGraph *graph, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op);
bool z_program_graph_patch_apply_replace_expr(ZProgramGraph *graph, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op);

#endif
