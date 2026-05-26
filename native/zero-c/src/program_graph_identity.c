#include "program_graph.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

const char *z_program_graph_node_kind_name(ZProgramGraphNodeKind kind) {
  switch (kind) {
    case Z_PROGRAM_GRAPH_NODE_MODULE: return "Module";
    case Z_PROGRAM_GRAPH_NODE_IMPORT: return "Import";
    case Z_PROGRAM_GRAPH_NODE_C_IMPORT: return "CImport";
    case Z_PROGRAM_GRAPH_NODE_CONST: return "Const";
    case Z_PROGRAM_GRAPH_NODE_TYPE_ALIAS: return "TypeAlias";
    case Z_PROGRAM_GRAPH_NODE_SHAPE: return "Shape";
    case Z_PROGRAM_GRAPH_NODE_INTERFACE: return "Interface";
    case Z_PROGRAM_GRAPH_NODE_ENUM: return "Enum";
    case Z_PROGRAM_GRAPH_NODE_CHOICE: return "Choice";
    case Z_PROGRAM_GRAPH_NODE_FUNCTION: return "Function";
    case Z_PROGRAM_GRAPH_NODE_PARAM: return "Param";
    case Z_PROGRAM_GRAPH_NODE_FIELD: return "Field";
    case Z_PROGRAM_GRAPH_NODE_ENUM_CASE: return "EnumCase";
    case Z_PROGRAM_GRAPH_NODE_CHOICE_CASE: return "ChoiceCase";
    case Z_PROGRAM_GRAPH_NODE_BLOCK: return "Block";
    case Z_PROGRAM_GRAPH_NODE_LET: return "Let";
    case Z_PROGRAM_GRAPH_NODE_ASSIGNMENT: return "Assignment";
    case Z_PROGRAM_GRAPH_NODE_DEFER: return "Defer";
    case Z_PROGRAM_GRAPH_NODE_CHECK: return "Check";
    case Z_PROGRAM_GRAPH_NODE_RETURN: return "Return";
    case Z_PROGRAM_GRAPH_NODE_EXPRESSION_STATEMENT: return "ExpressionStatement";
    case Z_PROGRAM_GRAPH_NODE_IF: return "If";
    case Z_PROGRAM_GRAPH_NODE_WHILE: return "While";
    case Z_PROGRAM_GRAPH_NODE_FOR: return "For";
    case Z_PROGRAM_GRAPH_NODE_BREAK: return "Break";
    case Z_PROGRAM_GRAPH_NODE_CONTINUE: return "Continue";
    case Z_PROGRAM_GRAPH_NODE_MATCH: return "Match";
    case Z_PROGRAM_GRAPH_NODE_RAISE: return "Raise";
    case Z_PROGRAM_GRAPH_NODE_MATCH_ARM: return "MatchArm";
    case Z_PROGRAM_GRAPH_NODE_IDENTIFIER: return "Identifier";
    case Z_PROGRAM_GRAPH_NODE_LITERAL: return "Literal";
    case Z_PROGRAM_GRAPH_NODE_FIELD_ACCESS: return "FieldAccess";
    case Z_PROGRAM_GRAPH_NODE_INDEX_ACCESS: return "IndexAccess";
    case Z_PROGRAM_GRAPH_NODE_SLICE: return "Slice";
    case Z_PROGRAM_GRAPH_NODE_CALL: return "Call";
    case Z_PROGRAM_GRAPH_NODE_METHOD_CALL: return "MethodCall";
    case Z_PROGRAM_GRAPH_NODE_CAST: return "Cast";
    case Z_PROGRAM_GRAPH_NODE_BORROW: return "Borrow";
    case Z_PROGRAM_GRAPH_NODE_RESCUE: return "Rescue";
    case Z_PROGRAM_GRAPH_NODE_META: return "Meta";
    case Z_PROGRAM_GRAPH_NODE_SHAPE_LITERAL: return "ShapeLiteral";
    case Z_PROGRAM_GRAPH_NODE_ARRAY_LITERAL: return "ArrayLiteral";
    case Z_PROGRAM_GRAPH_NODE_FIELD_INIT: return "FieldInit";
    case Z_PROGRAM_GRAPH_NODE_TYPE_REF: return "TypeRef";
    case Z_PROGRAM_GRAPH_NODE_EFFECT_REF: return "EffectRef";
    case Z_PROGRAM_GRAPH_NODE_ERROR_VARIANT: return "ErrorVariant";
    case Z_PROGRAM_GRAPH_NODE_EXPRESSION: return "Expression";
    case Z_PROGRAM_GRAPH_NODE_STATEMENT: return "Statement";
  }
  return "Node";
}

const char *z_program_graph_edge_target_name(ZProgramGraphEdgeTarget target) {
  switch (target) {
    case Z_PROGRAM_GRAPH_EDGE_TARGET_NODE: return "node";
    case Z_PROGRAM_GRAPH_EDGE_TARGET_SYMBOL: return "symbol";
    case Z_PROGRAM_GRAPH_EDGE_TARGET_TYPE: return "type";
    case Z_PROGRAM_GRAPH_EDGE_TARGET_EFFECT: return "effect";
  }
  return "unknown";
}

const char *z_program_graph_validation_state_name(ZProgramGraphValidationState state) {
  switch (state) {
    case Z_PROGRAM_GRAPH_VALIDATION_DECODED: return "decoded";
    case Z_PROGRAM_GRAPH_VALIDATION_SHAPE_VALID: return "shape-valid";
    case Z_PROGRAM_GRAPH_VALIDATION_RESOLVED: return "resolved";
    case Z_PROGRAM_GRAPH_VALIDATION_TYPED: return "typed";
    case Z_PROGRAM_GRAPH_VALIDATION_EFFECT_CHECKED: return "effect-checked";
    case Z_PROGRAM_GRAPH_VALIDATION_BUILDABLE: return "buildable";
  }
  return "unknown";
}

static uint64_t graph_hash_text(uint64_t hash, const char *text) {
  const unsigned char *p = (const unsigned char *)(text ? text : "");
  while (*p) {
    hash ^= (uint64_t)*p++;
    hash *= 1099511628211ull;
  }
  hash ^= 0xffu;
  hash *= 1099511628211ull;
  return hash;
}

static uint64_t graph_hash_u64(uint64_t hash, uint64_t value) {
  for (unsigned i = 0; i < 8; i++) {
    hash ^= (value >> (i * 8)) & 0xffu;
    hash *= 1099511628211ull;
  }
  return hash;
}

static char *graph_format_domain_id(const char *domain, uint64_t hash) {
  char text[40];
  snprintf(text, sizeof(text), "%s:%016llx", domain, (unsigned long long)hash);
  return z_strdup(text);
}

static bool graph_node_declares_symbol(const ZProgramGraphNode *node) {
  if (!node || !node->name || !node->name[0]) return false;
  switch (node->kind) {
    case Z_PROGRAM_GRAPH_NODE_MODULE:
    case Z_PROGRAM_GRAPH_NODE_CONST:
    case Z_PROGRAM_GRAPH_NODE_C_IMPORT:
    case Z_PROGRAM_GRAPH_NODE_TYPE_ALIAS:
    case Z_PROGRAM_GRAPH_NODE_SHAPE:
    case Z_PROGRAM_GRAPH_NODE_INTERFACE:
    case Z_PROGRAM_GRAPH_NODE_ENUM:
    case Z_PROGRAM_GRAPH_NODE_CHOICE:
    case Z_PROGRAM_GRAPH_NODE_FUNCTION:
    case Z_PROGRAM_GRAPH_NODE_PARAM:
    case Z_PROGRAM_GRAPH_NODE_FIELD:
    case Z_PROGRAM_GRAPH_NODE_ENUM_CASE:
    case Z_PROGRAM_GRAPH_NODE_CHOICE_CASE:
    case Z_PROGRAM_GRAPH_NODE_LET:
    case Z_PROGRAM_GRAPH_NODE_ERROR_VARIANT:
      return true;
    default:
      return false;
  }
}

static uint64_t graph_node_hash_value(const ZProgramGraphNode *node) {
  uint64_t hash = 1469598103934665603ull;
  hash = graph_hash_text(hash, z_program_graph_node_kind_name(node->kind));
  hash = graph_hash_text(hash, node->name);
  hash = graph_hash_text(hash, node->type);
  hash = graph_hash_text(hash, node->value);
  hash = graph_hash_text(hash, node->path);
  hash = graph_hash_u64(hash, (uint64_t)(unsigned)node->line);
  hash = graph_hash_u64(hash, (uint64_t)(unsigned)node->column);
  hash = graph_hash_u64(hash, node->is_public ? 1 : 0);
  hash = graph_hash_u64(hash, node->is_mutable ? 1 : 0);
  hash = graph_hash_u64(hash, node->is_static ? 1 : 0);
  hash = graph_hash_u64(hash, node->fallible ? 1 : 0);
  hash = graph_hash_u64(hash, node->export_c ? 1 : 0);
  return hash;
}

static char *graph_node_symbol_id(const ZProgramGraphNode *node) {
  if (!graph_node_declares_symbol(node)) return NULL;
  uint64_t hash = 1469598103934665603ull;
  hash = graph_hash_text(hash, z_program_graph_node_kind_name(node->kind));
  hash = graph_hash_text(hash, node->path);
  hash = graph_hash_text(hash, node->name);
  hash = graph_hash_u64(hash, (uint64_t)(unsigned)node->line);
  hash = graph_hash_u64(hash, (uint64_t)(unsigned)node->column);
  return graph_format_domain_id("symbol", hash);
}

static char *graph_node_type_id(const ZProgramGraphNode *node) {
  if (!node || !node->type || !node->type[0]) return NULL;
  uint64_t hash = graph_hash_text(1469598103934665603ull, node->type);
  return graph_format_domain_id("type", hash);
}

static char *graph_node_effect_id(const ZProgramGraphNode *node) {
  if (!node || node->kind != Z_PROGRAM_GRAPH_NODE_EFFECT_REF || !node->name || !node->name[0]) return NULL;
  uint64_t hash = graph_hash_text(1469598103934665603ull, node->name);
  return graph_format_domain_id("effect", hash);
}

void z_program_graph_finalize_identities(ZProgramGraph *graph) {
  if (!graph) return;
  uint64_t graph_hash = 1469598103934665603ull;
  graph_hash = graph_hash_u64(graph_hash, graph->schema_version);
  graph_hash = graph_hash_text(graph_hash, graph->module_identity);
  for (size_t i = 0; i < graph->node_len; i++) {
    ZProgramGraphNode *node = &graph->nodes[i];
    free(node->symbol_id);
    free(node->type_id);
    free(node->effect_id);
    free(node->node_hash);
    node->symbol_id = graph_node_symbol_id(node);
    node->type_id = graph_node_type_id(node);
    node->effect_id = graph_node_effect_id(node);
    node->node_hash = graph_format_domain_id("nodehash", graph_node_hash_value(node));
    graph_hash = graph_hash_text(graph_hash, node->id);
    graph_hash = graph_hash_text(graph_hash, node->node_hash);
    graph_hash = graph_hash_text(graph_hash, node->symbol_id);
    graph_hash = graph_hash_text(graph_hash, node->type_id);
    graph_hash = graph_hash_text(graph_hash, node->effect_id);
  }
  for (size_t i = 0; i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    graph_hash = graph_hash_text(graph_hash, edge->from);
    graph_hash = graph_hash_text(graph_hash, edge->to);
    graph_hash = graph_hash_text(graph_hash, edge->kind);
    graph_hash = graph_hash_text(graph_hash, z_program_graph_edge_target_name(edge->target));
    graph_hash = graph_hash_u64(graph_hash, edge->order);
  }
  free(graph->graph_hash);
  graph->graph_hash = graph_format_domain_id("graph", graph_hash);
}
