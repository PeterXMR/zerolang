#include "program_graph_semantics.h"

#include "std_sig.h"
#include "std_source.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const ZStdHelperInfo *helper;
  const ZStdSourceModule *source_module;
  const ZProgramGraphNode *c_import;
  const ZProgramGraphNode *target_function;
  const ZProgramGraphNode *world_binding;
  bool world_write;
  bool fallible;
  bool present;
} ZGraphSemanticContract;

static bool graph_semantics_text_eq(const char *left, const char *right) {
  return strcmp(left ? left : "", right ? right : "") == 0;
}

static bool graph_semantics_text_present(const char *text) {
  return text && text[0];
}

static bool graph_semantics_text_starts_with(const char *text, const char *prefix) {
  size_t prefix_len = prefix ? strlen(prefix) : 0;
  return text && prefix && strncmp(text, prefix, prefix_len) == 0;
}

static bool graph_semantics_text_contains(const char *text, const char *needle) {
  return text && needle && strstr(text, needle) != NULL;
}

static void graph_semantics_append_quoted(ZBuf *buf, const char *text) {
  zbuf_append_char(buf, '"');
  for (const char *p = text ? text : ""; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    switch (ch) {
      case '\\': zbuf_append(buf, "\\\\"); break;
      case '"': zbuf_append(buf, "\\\""); break;
      case '\n': zbuf_append(buf, "\\n"); break;
      case '\r': zbuf_append(buf, "\\r"); break;
      case '\t': zbuf_append(buf, "\\t"); break;
      default:
        if (ch < 0x20) {
          const char *hex = "0123456789abcdef";
          char escape[7] = {'\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 0x0f], 0};
          zbuf_append(buf, escape);
        } else {
          zbuf_append_char(buf, (char)ch);
        }
        break;
    }
  }
  zbuf_append_char(buf, '"');
}

static void graph_semantics_append_source_range_json(ZBuf *buf, const ZProgramGraphNode *node) {
  int line = node && node->line > 0 ? node->line : 1;
  int column = node && node->column > 0 ? node->column : 1;
  zbuf_append(buf, "{\"path\":");
  graph_semantics_append_quoted(buf, node ? node->path : "");
  zbuf_appendf(buf,
               ",\"start\":{\"line\":%d,\"column\":%d},\"end\":{\"line\":%d,\"column\":%d},\"columnUnit\":\"utf8-byte\"}",
               line,
               column,
               line,
               column + 1);
}

static size_t graph_semantics_node_index(const ZProgramGraph *graph, const char *id) {
  for (size_t i = 0; graph && id && i < graph->node_len; i++) {
    if (graph_semantics_text_eq(graph->nodes[i].id, id)) return i;
  }
  return SIZE_MAX;
}

static const ZProgramGraphNode *graph_semantics_node(const ZProgramGraph *graph, size_t index) {
  return graph && index < graph->node_len ? &graph->nodes[index] : NULL;
}

static const ZProgramGraphEdge *graph_semantics_owner_edge(const ZProgramGraph *graph, const char *node_id) {
  for (size_t i = 0; graph && node_id && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && graph_semantics_text_eq(edge->to, node_id)) return edge;
  }
  return NULL;
}

static const ZProgramGraphNode *graph_semantics_child(const ZProgramGraph *graph, const ZProgramGraphNode *node, const char *kind, size_t order) {
  for (size_t i = 0; graph && node && kind && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE &&
        edge->order == order &&
        graph_semantics_text_eq(edge->from, node->id) &&
        graph_semantics_text_eq(edge->kind, kind)) {
      return graph_semantics_node(graph, graph_semantics_node_index(graph, edge->to));
    }
  }
  return NULL;
}

static size_t graph_semantics_child_count(const ZProgramGraph *graph, const ZProgramGraphNode *node, const char *kind) {
  size_t count = 0;
  for (size_t i = 0; graph && node && kind && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE &&
        graph_semantics_text_eq(edge->from, node->id) &&
        graph_semantics_text_eq(edge->kind, kind)) {
      count++;
    }
  }
  return count;
}

static char *graph_semantics_expr_chain(const ZProgramGraph *graph, const ZProgramGraphNode *node) {
  if (!graph || !node) return NULL;
  if (node->kind == Z_PROGRAM_GRAPH_NODE_IDENTIFIER) return graph_semantics_text_present(node->name) ? z_strdup(node->name) : NULL;
  if (node->kind == Z_PROGRAM_GRAPH_NODE_FIELD_ACCESS) {
    const ZProgramGraphNode *left = graph_semantics_child(graph, node, "left", 0);
    char *left_name = graph_semantics_expr_chain(graph, left);
    if (!left_name || !left_name[0] || !graph_semantics_text_present(node->name)) {
      free(left_name);
      return NULL;
    }
    ZBuf out;
    zbuf_init(&out);
    zbuf_append(&out, left_name);
    zbuf_append_char(&out, '.');
    zbuf_append(&out, node->name);
    free(left_name);
    return out.data ? out.data : NULL;
  }
  if (node->kind == Z_PROGRAM_GRAPH_NODE_CALL || node->kind == Z_PROGRAM_GRAPH_NODE_METHOD_CALL) {
    const ZProgramGraphNode *left = graph_semantics_child(graph, node, "left", 0);
    char *left_name = graph_semantics_expr_chain(graph, left);
    if (left_name) return left_name;
    return graph_semantics_text_present(node->name) ? z_strdup(node->name) : NULL;
  }
  return NULL;
}

static char *graph_semantics_first_segment(const char *name) {
  const char *dot = name ? strchr(name, '.') : NULL;
  return dot ? z_strndup(name, (size_t)(dot - name)) : (name ? z_strdup(name) : NULL);
}

static const char *graph_semantics_last_segment(const char *name) {
  const char *dot = name ? strrchr(name, '.') : NULL;
  return dot && dot[1] ? dot + 1 : (name ? name : "");
}

static const ZProgramGraphNode *graph_semantics_find_function(const ZProgramGraph *graph, const char *name) {
  for (size_t i = 0; graph && name && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind == Z_PROGRAM_GRAPH_NODE_FUNCTION && graph_semantics_text_eq(node->name, name)) return node;
  }
  return NULL;
}

static const ZProgramGraphNode *graph_semantics_find_c_import(const ZProgramGraph *graph, const char *alias) {
  for (size_t i = 0; graph && alias && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind == Z_PROGRAM_GRAPH_NODE_C_IMPORT && graph_semantics_text_eq(node->name, alias)) return node;
  }
  return NULL;
}

static const ZProgramGraphNode *graph_semantics_find_world_binding(const ZProgramGraph *graph, const char *name) {
  for (size_t i = 0; graph && name && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if ((node->kind == Z_PROGRAM_GRAPH_NODE_PARAM || node->kind == Z_PROGRAM_GRAPH_NODE_LET) &&
        graph_semantics_text_eq(node->name, name) &&
        graph_semantics_text_eq(node->type, "World")) {
      return node;
    }
  }
  return NULL;
}

static bool graph_semantics_is_under_kind(const ZProgramGraph *graph, const ZProgramGraphNode *node, ZProgramGraphNodeKind kind) {
  const char *current = node ? node->id : NULL;
  for (size_t depth = 0; graph && current && depth < graph->node_len; depth++) {
    const ZProgramGraphEdge *owner = graph_semantics_owner_edge(graph, current);
    if (!owner) return false;
    const ZProgramGraphNode *owner_node = graph_semantics_node(graph, graph_semantics_node_index(graph, owner->from));
    if (!owner_node) return false;
    if (owner_node->kind == kind) return true;
    current = owner_node->id;
  }
  return false;
}

static const ZProgramGraphNode *graph_semantics_world_stream_binding(const ZProgramGraph *graph, const ZProgramGraphNode *call, const char *qualified) {
  if (!call || !graph_semantics_text_eq(call->name, "write") || !qualified || !strstr(qualified, ".out.write")) return NULL;
  char *first = graph_semantics_first_segment(qualified);
  const ZProgramGraphNode *result = graph_semantics_find_world_binding(graph, first);
  free(first);
  return result;
}

static bool graph_semantics_node_has_type_fact(const ZProgramGraphNode *node) {
  return node && (graph_semantics_text_present(node->type) || graph_semantics_text_present(node->type_id));
}

static void graph_semantics_append_error_names_json(ZBuf *buf, const ZProgramGraph *graph, const ZProgramGraphNode *node) {
  zbuf_append(buf, "[");
  bool first = true;
  size_t count = graph_semantics_child_count(graph, node, "error");
  for (size_t order = 0; order < count; order++) {
    const ZProgramGraphNode *error = graph_semantics_child(graph, node, "error", order);
    if (!error) continue;
    if (!first) zbuf_append(buf, ",");
    graph_semantics_append_quoted(buf, error->name);
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_effect_refs_json(ZBuf *buf, const ZProgramGraph *graph, const ZProgramGraphNode *node) {
  zbuf_append(buf, "[");
  bool first = true;
  size_t count = graph_semantics_child_count(graph, node, "effect");
  for (size_t order = 0; order < count; order++) {
    const ZProgramGraphNode *effect = graph_semantics_child(graph, node, "effect", order);
    if (!effect) continue;
    if (!first) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"node\":");
    graph_semantics_append_quoted(buf, effect->id);
    zbuf_append(buf, ",\"name\":");
    graph_semantics_append_quoted(buf, effect->name);
    zbuf_append(buf, ",\"effectId\":");
    graph_semantics_append_quoted(buf, effect->effect_id);
    zbuf_append(buf, "}");
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_std_error_names_json(ZBuf *buf, const ZStdHelperInfo *helper) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; helper && i < Z_STD_HELPER_MAX_ERRORS; i++) {
    const char *name = z_std_helper_error_name(helper, i);
    if (!name) break;
    if (!first) zbuf_append(buf, ",");
    graph_semantics_append_quoted(buf, name);
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_std_arg_types_json(ZBuf *buf, const ZStdHelperInfo *helper) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; helper && helper->arg_count > 0 && i < (size_t)helper->arg_count && i < Z_STD_HELPER_MAX_ARGS; i++) {
    const char *type = helper->arg_types[i];
    if (!first) zbuf_append(buf, ",");
    if (type) graph_semantics_append_quoted(buf, type);
    else zbuf_append(buf, "null");
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_args_json(ZBuf *buf, const ZProgramGraph *graph, const ZProgramGraphNode *call) {
  zbuf_append(buf, "[");
  bool first = true;
  size_t count = graph_semantics_child_count(graph, call, "arg");
  for (size_t order = 0; order < count; order++) {
    const ZProgramGraphNode *arg = graph_semantics_child(graph, call, "arg", order);
    if (!arg) continue;
    if (!first) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"node\":");
    graph_semantics_append_quoted(buf, arg->id);
    zbuf_append(buf, ",\"kind\":");
    graph_semantics_append_quoted(buf, z_program_graph_node_kind_name(arg->kind));
    zbuf_append(buf, ",\"type\":");
    graph_semantics_append_quoted(buf, arg->type);
    zbuf_append(buf, ",\"typeId\":");
    graph_semantics_append_quoted(buf, arg->type_id);
    zbuf_appendf(buf, ",\"order\":%zu,\"sourceRange\":", order);
    graph_semantics_append_source_range_json(buf, arg);
    zbuf_append(buf, "}");
    first = false;
  }
  zbuf_append(buf, "]");
}

static ZGraphSemanticContract graph_semantics_contract(const ZProgramGraph *graph, const ZProgramGraphNode *call, const char *qualified) {
  ZGraphSemanticContract contract = {0};
  contract.helper = z_std_helper_find(qualified);
  contract.source_module = z_std_source_module_for_public_call(qualified);
  char *first = graph_semantics_first_segment(qualified);
  contract.c_import = first && strchr(qualified ? qualified : "", '.') ? graph_semantics_find_c_import(graph, first) : NULL;
  contract.target_function = !contract.helper && !contract.c_import && !contract.source_module ? graph_semantics_find_function(graph, graph_semantics_last_segment(qualified)) : NULL;
  contract.world_binding = graph_semantics_world_stream_binding(graph, call, qualified);
  contract.world_write = contract.world_binding != NULL;
  contract.fallible = contract.world_write || (contract.helper && z_std_helper_is_fallible(contract.helper)) || (contract.target_function && contract.target_function->fallible);
  contract.present = contract.helper || contract.c_import || contract.source_module || contract.target_function || contract.world_write;
  free(first);
  return contract;
}

static const char *graph_semantics_contract_kind(const ZGraphSemanticContract *contract) {
  if (!contract) return "language";
  if (contract->world_write) return "worldStreamWrite";
  if (contract->c_import) return "cAbi";
  if (contract->source_module) return "sourceBackedStdlib";
  if (contract->helper) return "stdlib";
  if (contract->target_function) return "function";
  return "language";
}

static const char *graph_semantics_contract_capability(const ZGraphSemanticContract *contract) {
  if (!contract) return "";
  if (contract->world_write) return "io";
  if (contract->c_import) return "c-abi";
  return contract->helper ? contract->helper->capability : "";
}

static const char *graph_semantics_contract_target_support(const ZGraphSemanticContract *contract) {
  if (!contract) return "";
  if (contract->world_write) return "world-io";
  if (contract->c_import) return "host-c-abi";
  return contract->helper ? contract->helper->target_support : "";
}

static const char *graph_semantics_contract_allocation(const ZGraphSemanticContract *contract) {
  if (!contract || !contract->helper) return "";
  return contract->helper->allocation_behavior;
}

static bool graph_semantics_contract_has_target_requirement(const ZGraphSemanticContract *contract) {
  const char *support = graph_semantics_contract_target_support(contract);
  return contract && contract->present &&
    (contract->world_write ||
     contract->c_import ||
     (support && support[0] && !graph_semantics_text_eq(support, "target-neutral")));
}

static bool graph_semantics_contract_has_repair(const ZGraphSemanticContract *contract) {
  return contract && contract->fallible;
}

static void graph_semantics_append_repair_json(ZBuf *buf, const ZGraphSemanticContract *contract, bool checked) {
  if (!graph_semantics_contract_has_repair(contract)) {
    zbuf_append(buf, "null");
    return;
  }
  zbuf_append(buf, "{\"id\":\"check-fallible-call\",\"appliesWhen\":");
  graph_semantics_append_quoted(buf, checked ? "alreadyChecked" : "requiresCheck");
  zbuf_append(buf, ",\"summary\":\"wrap this fallible call in check or rescue before using its value\"}");
}

static void graph_semantics_append_contract_target_node_json(ZBuf *buf, const ZGraphSemanticContract *contract) {
  graph_semantics_append_quoted(buf,
                                contract && contract->world_binding ? contract->world_binding->id :
                                (contract && contract->c_import ? contract->c_import->id :
                                 (contract && contract->target_function ? contract->target_function->id : "")));
}

static void graph_semantics_append_contract_symbol_json(ZBuf *buf, const ZGraphSemanticContract *contract, const char *qualified) {
  if (contract && contract->world_binding) {
    graph_semantics_append_quoted(buf, contract->world_binding->symbol_id);
    return;
  }
  if (contract && contract->c_import) {
    graph_semantics_append_quoted(buf, contract->c_import->symbol_id);
    return;
  }
  if (contract && contract->target_function) {
    graph_semantics_append_quoted(buf, contract->target_function->symbol_id);
    return;
  }
  if (contract && contract->helper) {
    zbuf_append(buf, "\"stdlib:");
    for (const char *p = qualified ? qualified : ""; *p; p++) {
      if (*p == '"' || *p == '\\') zbuf_append_char(buf, '_');
      else zbuf_append_char(buf, *p);
    }
    zbuf_append_char(buf, '"');
    return;
  }
  graph_semantics_append_quoted(buf, "");
}

static void graph_semantics_append_resolution_json(ZBuf *buf, const ZGraphSemanticContract *contract, const char *qualified) {
  zbuf_append(buf, "{\"referenceKind\":\"call\",\"qualifiedName\":");
  graph_semantics_append_quoted(buf, qualified);
  zbuf_append(buf, ",\"targetKind\":");
  graph_semantics_append_quoted(buf, graph_semantics_contract_kind(contract));
  zbuf_append(buf, ",\"targetNode\":");
  graph_semantics_append_contract_target_node_json(buf, contract);
  zbuf_append(buf, ",\"symbolId\":");
  graph_semantics_append_contract_symbol_json(buf, contract, qualified);
  zbuf_append(buf, "}");
}

static void graph_semantics_append_contract_json(ZBuf *buf, const ZProgramGraph *graph, const ZProgramGraphNode *call, const char *qualified, bool checked, bool *fallible_out) {
  ZGraphSemanticContract contract = graph_semantics_contract(graph, call, qualified);
  if (fallible_out) *fallible_out = contract.fallible;
  zbuf_append(buf, "{\"kind\":");
  graph_semantics_append_quoted(buf, graph_semantics_contract_kind(&contract));
  zbuf_appendf(buf, ",\"fallible\":%s,\"checked\":%s,\"requiresCheck\":%s", contract.fallible ? "true" : "false", checked ? "true" : "false", contract.fallible && !checked ? "true" : "false");
  zbuf_append(buf, ",\"targetNode\":");
  graph_semantics_append_contract_target_node_json(buf, &contract);
  zbuf_append(buf, ",\"symbolId\":");
  graph_semantics_append_contract_symbol_json(buf, &contract, qualified);
  zbuf_append(buf, ",\"returnType\":");
  graph_semantics_append_quoted(buf, contract.helper && contract.helper->return_type ? contract.helper->return_type : (contract.target_function ? contract.target_function->type : call ? call->type : ""));
  zbuf_append(buf, ",\"capability\":");
  graph_semantics_append_quoted(buf, graph_semantics_contract_capability(&contract));
  zbuf_append(buf, ",\"targetSupport\":");
  graph_semantics_append_quoted(buf, graph_semantics_contract_target_support(&contract));
  zbuf_append(buf, ",\"allocation\":");
  graph_semantics_append_quoted(buf, graph_semantics_contract_allocation(&contract));
  zbuf_append(buf, ",\"sourceModule\":");
  graph_semantics_append_quoted(buf, contract.source_module ? contract.source_module->module : "");
  zbuf_append(buf, ",\"expectedArgCount\":");
  if (contract.helper && contract.helper->arg_count >= 0) zbuf_appendf(buf, "%d", contract.helper->arg_count);
  else if (contract.target_function) zbuf_appendf(buf, "%zu", graph_semantics_child_count(graph, contract.target_function, "param"));
  else zbuf_append(buf, "null");
  zbuf_append(buf, ",\"expectedArgTypes\":");
  graph_semantics_append_std_arg_types_json(buf, contract.helper);
  zbuf_append(buf, ",\"errors\":");
  if (contract.helper) graph_semantics_append_std_error_names_json(buf, contract.helper);
  else if (contract.target_function) graph_semantics_append_error_names_json(buf, graph, contract.target_function);
  else zbuf_append(buf, "[]");
  zbuf_append(buf, ",\"repair\":");
  graph_semantics_append_repair_json(buf, &contract, checked);
  zbuf_append(buf, "}");
}

static size_t graph_semantics_typed_node_count(const ZProgramGraph *graph) {
  size_t count = 0;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    if (graph_semantics_node_has_type_fact(&graph->nodes[i])) count++;
  }
  return count;
}

static size_t graph_semantics_function_count(const ZProgramGraph *graph) {
  size_t count = 0;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    if (graph->nodes[i].kind == Z_PROGRAM_GRAPH_NODE_FUNCTION) count++;
  }
  return count;
}

static size_t graph_semantics_call_count(const ZProgramGraph *graph) {
  size_t count = 0;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    if (graph->nodes[i].kind == Z_PROGRAM_GRAPH_NODE_CALL || graph->nodes[i].kind == Z_PROGRAM_GRAPH_NODE_METHOD_CALL) count++;
  }
  return count;
}

static size_t graph_semantics_effect_fact_count(const ZProgramGraph *graph) {
  size_t count = 0;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind == Z_PROGRAM_GRAPH_NODE_FUNCTION && (node->fallible || graph_semantics_child_count(graph, node, "effect") > 0 || graph_semantics_child_count(graph, node, "error") > 0)) count++;
    else if (node->kind == Z_PROGRAM_GRAPH_NODE_CHECK || node->kind == Z_PROGRAM_GRAPH_NODE_RAISE || node->kind == Z_PROGRAM_GRAPH_NODE_RESCUE) count++;
  }
  return count;
}

static void graph_semantics_call_summary_counts(const ZProgramGraph *graph, size_t *fallible_calls, size_t *contracts) {
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_CALL && node->kind != Z_PROGRAM_GRAPH_NODE_METHOD_CALL) continue;
    char *qualified = graph_semantics_expr_chain(graph, node);
    ZGraphSemanticContract contract = graph_semantics_contract(graph, node, qualified && qualified[0] ? qualified : node->name);
    if (contract.fallible && fallible_calls) (*fallible_calls)++;
    if (contract.present && contracts) (*contracts)++;
    free(qualified);
  }
}

static bool graph_semantics_type_is_borrow(const char *type) {
  return graph_semantics_text_starts_with(type, "ref<") ||
         graph_semantics_text_starts_with(type, "mutref<") ||
         graph_semantics_text_starts_with(type, "Span<") ||
         graph_semantics_text_starts_with(type, "MutSpan<") ||
         graph_semantics_text_contains(type, "<ref<") ||
         graph_semantics_text_contains(type, "<mutref<") ||
         graph_semantics_text_contains(type, "<Span<") ||
         graph_semantics_text_contains(type, "<MutSpan<");
}

static bool graph_semantics_type_is_resource(const char *type) {
  return graph_semantics_text_contains(type, "File") ||
         graph_semantics_text_contains(type, "ByteBuf") ||
         graph_semantics_text_contains(type, "Alloc") ||
         graph_semantics_text_contains(type, "BufferedReader") ||
         graph_semantics_text_contains(type, "BufferedWriter") ||
         graph_semantics_text_eq(type, "Fs") ||
         graph_semantics_text_eq(type, "World");
}

static bool graph_semantics_type_is_ownership_fact(const char *type) {
  return graph_semantics_text_contains(type, "owned<") ||
         graph_semantics_type_is_borrow(type) ||
         graph_semantics_type_is_resource(type);
}

static const char *graph_semantics_ownership_kind(const char *type) {
  if (graph_semantics_text_contains(type, "owned<")) return "owned";
  if (graph_semantics_text_starts_with(type, "mutref<") || graph_semantics_text_contains(type, "<mutref<")) return "mut-borrow";
  if (graph_semantics_text_starts_with(type, "ref<") || graph_semantics_text_contains(type, "<ref<")) return "borrow";
  if (graph_semantics_text_starts_with(type, "MutSpan<") || graph_semantics_text_contains(type, "<MutSpan<")) return "mut-view";
  if (graph_semantics_text_starts_with(type, "Span<") || graph_semantics_text_contains(type, "<Span<")) return "view";
  if (graph_semantics_type_is_resource(type)) return "resource-handle";
  return "value";
}

static const char *graph_semantics_resource_kind(const char *type, const char *capability) {
  if (graph_semantics_text_contains(type, "File")) return "file";
  if (graph_semantics_text_contains(type, "ByteBuf")) return "byte-buffer";
  if (graph_semantics_text_contains(type, "Alloc")) return "allocator";
  if (graph_semantics_text_eq(type, "Fs") || graph_semantics_text_eq(capability, "fs")) return "filesystem";
  if (graph_semantics_text_eq(type, "World") || graph_semantics_text_eq(capability, "io")) return "world-io";
  if (graph_semantics_text_eq(capability, "c-abi")) return "c-abi";
  if (graph_semantics_text_eq(capability, "args")) return "process-args";
  if (graph_semantics_text_eq(capability, "env")) return "process-env";
  if (capability && capability[0]) return capability;
  return "resource";
}

static bool graph_semantics_contract_is_resource(const ZGraphSemanticContract *contract) {
  const char *capability = graph_semantics_contract_capability(contract);
  const char *allocation = graph_semantics_contract_allocation(contract);
  bool allocation_resource = allocation && allocation[0] &&
    !graph_semantics_text_eq(allocation, "no allocation") &&
    (graph_semantics_text_contains(allocation, "alloc") ||
     graph_semantics_text_contains(allocation, "caller storage") ||
     graph_semantics_text_contains(allocation, "buffer"));
  return contract && contract->present &&
    ((capability && capability[0] && !graph_semantics_text_eq(capability, "none") &&
      !graph_semantics_text_eq(capability, "memory") &&
      !graph_semantics_text_eq(capability, "parse") &&
      !graph_semantics_text_eq(capability, "path") &&
      !graph_semantics_text_eq(capability, "codec")) ||
     allocation_resource);
}

static size_t graph_semantics_ownership_fact_count(const ZProgramGraph *graph) {
  size_t count = 0;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    if (graph_semantics_node_has_type_fact(&graph->nodes[i]) && graph_semantics_type_is_ownership_fact(graph->nodes[i].type)) count++;
  }
  return count;
}

static size_t graph_semantics_borrowing_fact_count(const ZProgramGraph *graph) {
  size_t count = 0;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind == Z_PROGRAM_GRAPH_NODE_BORROW || graph_semantics_type_is_borrow(node->type)) count++;
  }
  return count;
}

static size_t graph_semantics_resource_fact_count(const ZProgramGraph *graph) {
  size_t count = 0;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (graph_semantics_node_has_type_fact(node) && graph_semantics_type_is_resource(node->type)) count++;
    if (node->kind != Z_PROGRAM_GRAPH_NODE_CALL && node->kind != Z_PROGRAM_GRAPH_NODE_METHOD_CALL) continue;
    char *qualified = graph_semantics_expr_chain(graph, node);
    ZGraphSemanticContract contract = graph_semantics_contract(graph, node, qualified && qualified[0] ? qualified : node->name);
    if (graph_semantics_contract_is_resource(&contract)) count++;
    free(qualified);
  }
  return count;
}

static size_t graph_semantics_target_requirement_count(const ZProgramGraph *graph) {
  size_t count = 0;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_CALL && node->kind != Z_PROGRAM_GRAPH_NODE_METHOD_CALL) continue;
    char *qualified = graph_semantics_expr_chain(graph, node);
    ZGraphSemanticContract contract = graph_semantics_contract(graph, node, qualified && qualified[0] ? qualified : node->name);
    if (graph_semantics_contract_has_target_requirement(&contract)) count++;
    free(qualified);
  }
  return count;
}

static size_t graph_semantics_repair_fact_count(const ZProgramGraph *graph) {
  size_t count = 0;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_CALL && node->kind != Z_PROGRAM_GRAPH_NODE_METHOD_CALL) continue;
    char *qualified = graph_semantics_expr_chain(graph, node);
    ZGraphSemanticContract contract = graph_semantics_contract(graph, node, qualified && qualified[0] ? qualified : node->name);
    if (graph_semantics_contract_has_repair(&contract)) count++;
    free(qualified);
  }
  return count;
}

static void graph_semantics_append_types_json(ZBuf *buf, const ZProgramGraph *graph) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (!graph_semantics_node_has_type_fact(node)) continue;
    if (!first) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"node\":");
    graph_semantics_append_quoted(buf, node->id);
    zbuf_append(buf, ",\"kind\":");
    graph_semantics_append_quoted(buf, z_program_graph_node_kind_name(node->kind));
    zbuf_append(buf, ",\"name\":");
    graph_semantics_append_quoted(buf, node->name);
    zbuf_append(buf, ",\"type\":");
    graph_semantics_append_quoted(buf, node->type);
    zbuf_append(buf, ",\"typeId\":");
    graph_semantics_append_quoted(buf, node->type_id);
    zbuf_append(buf, ",\"sourceRange\":");
    graph_semantics_append_source_range_json(buf, node);
    zbuf_append(buf, "}");
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_function_params_json(ZBuf *buf, const ZProgramGraph *graph, const ZProgramGraphNode *function) {
  zbuf_append(buf, "[");
  bool first = true;
  size_t count = graph_semantics_child_count(graph, function, "param");
  for (size_t order = 0; order < count; order++) {
    const ZProgramGraphNode *param = graph_semantics_child(graph, function, "param", order);
    if (!param) continue;
    if (!first) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"node\":");
    graph_semantics_append_quoted(buf, param->id);
    zbuf_append(buf, ",\"name\":");
    graph_semantics_append_quoted(buf, param->name);
    zbuf_append(buf, ",\"type\":");
    graph_semantics_append_quoted(buf, param->type);
    zbuf_append(buf, ",\"typeId\":");
    graph_semantics_append_quoted(buf, param->type_id);
    zbuf_appendf(buf, ",\"mutable\":%s,\"static\":%s,\"order\":%zu,\"sourceRange\":", param->is_mutable ? "true" : "false", param->is_static ? "true" : "false", order);
    graph_semantics_append_source_range_json(buf, param);
    zbuf_append(buf, "}");
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_functions_json(ZBuf *buf, const ZProgramGraph *graph) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_FUNCTION) continue;
    if (!first) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"node\":");
    graph_semantics_append_quoted(buf, node->id);
    zbuf_append(buf, ",\"name\":");
    graph_semantics_append_quoted(buf, node->name);
    zbuf_append(buf, ",\"symbolId\":");
    graph_semantics_append_quoted(buf, node->symbol_id);
    zbuf_append(buf, ",\"returnType\":");
    graph_semantics_append_quoted(buf, node->type);
    zbuf_append(buf, ",\"returnTypeId\":");
    graph_semantics_append_quoted(buf, node->type_id);
    zbuf_appendf(buf, ",\"public\":%s,\"fallible\":%s,\"exportC\":%s", node->is_public ? "true" : "false", node->fallible ? "true" : "false", node->export_c ? "true" : "false");
    zbuf_append(buf, ",\"params\":");
    graph_semantics_append_function_params_json(buf, graph, node);
    zbuf_append(buf, ",\"effects\":");
    graph_semantics_append_effect_refs_json(buf, graph, node);
    zbuf_append(buf, ",\"errors\":");
    graph_semantics_append_error_names_json(buf, graph, node);
    zbuf_append(buf, ",\"sourceRange\":");
    graph_semantics_append_source_range_json(buf, node);
    zbuf_append(buf, "}");
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_calls_json(ZBuf *buf, const ZProgramGraph *graph) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_CALL && node->kind != Z_PROGRAM_GRAPH_NODE_METHOD_CALL) continue;
    char *qualified = graph_semantics_expr_chain(graph, node);
    bool checked = graph_semantics_is_under_kind(graph, node, Z_PROGRAM_GRAPH_NODE_CHECK) ||
                   graph_semantics_is_under_kind(graph, node, Z_PROGRAM_GRAPH_NODE_RESCUE);
    bool fallible = false;
    if (!first) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"node\":");
    graph_semantics_append_quoted(buf, node->id);
    zbuf_append(buf, ",\"kind\":");
    graph_semantics_append_quoted(buf, z_program_graph_node_kind_name(node->kind));
    zbuf_append(buf, ",\"name\":");
    graph_semantics_append_quoted(buf, node->name);
    zbuf_append(buf, ",\"qualifiedName\":");
    graph_semantics_append_quoted(buf, qualified && qualified[0] ? qualified : node->name);
    zbuf_append(buf, ",\"returnType\":");
    graph_semantics_append_quoted(buf, node->type);
    zbuf_append(buf, ",\"returnTypeId\":");
    graph_semantics_append_quoted(buf, node->type_id);
    zbuf_append(buf, ",\"args\":");
    graph_semantics_append_args_json(buf, graph, node);
    zbuf_append(buf, ",\"contract\":");
    graph_semantics_append_contract_json(buf, graph, node, qualified && qualified[0] ? qualified : node->name, checked, &fallible);
    ZGraphSemanticContract resolution_contract = graph_semantics_contract(graph, node, qualified && qualified[0] ? qualified : node->name);
    zbuf_append(buf, ",\"resolution\":");
    graph_semantics_append_resolution_json(buf, &resolution_contract, qualified && qualified[0] ? qualified : node->name);
    zbuf_appendf(buf, ",\"fallible\":%s,\"checked\":%s,\"sourceRange\":", fallible ? "true" : "false", checked ? "true" : "false");
    graph_semantics_append_source_range_json(buf, node);
    zbuf_append(buf, "}");
    free(qualified);
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_effects_json(ZBuf *buf, const ZProgramGraph *graph) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    bool include_function = node->kind == Z_PROGRAM_GRAPH_NODE_FUNCTION &&
      (node->fallible || graph_semantics_child_count(graph, node, "effect") > 0 || graph_semantics_child_count(graph, node, "error") > 0);
    bool include_statement = node->kind == Z_PROGRAM_GRAPH_NODE_CHECK || node->kind == Z_PROGRAM_GRAPH_NODE_RAISE || node->kind == Z_PROGRAM_GRAPH_NODE_RESCUE;
    if (!include_function && !include_statement) continue;
    if (!first) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"node\":");
    graph_semantics_append_quoted(buf, node->id);
    zbuf_append(buf, ",\"kind\":");
    graph_semantics_append_quoted(buf, z_program_graph_node_kind_name(node->kind));
    zbuf_appendf(buf, ",\"fallible\":%s", node->fallible ? "true" : "false");
    zbuf_append(buf, ",\"effects\":");
    graph_semantics_append_effect_refs_json(buf, graph, node);
    zbuf_append(buf, ",\"errors\":");
    graph_semantics_append_error_names_json(buf, graph, node);
    zbuf_append(buf, ",\"sourceRange\":");
    graph_semantics_append_source_range_json(buf, node);
    zbuf_append(buf, "}");
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_ownership_json(ZBuf *buf, const ZProgramGraph *graph) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (!graph_semantics_node_has_type_fact(node) || !graph_semantics_type_is_ownership_fact(node->type)) continue;
    if (!first) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"node\":");
    graph_semantics_append_quoted(buf, node->id);
    zbuf_append(buf, ",\"kind\":");
    graph_semantics_append_quoted(buf, z_program_graph_node_kind_name(node->kind));
    zbuf_append(buf, ",\"name\":");
    graph_semantics_append_quoted(buf, node->name);
    zbuf_append(buf, ",\"type\":");
    graph_semantics_append_quoted(buf, node->type);
    zbuf_append(buf, ",\"ownership\":");
    graph_semantics_append_quoted(buf, graph_semantics_ownership_kind(node->type));
    zbuf_appendf(buf, ",\"mutable\":%s,\"resource\":%s,\"sourceRange\":", node->is_mutable ? "true" : "false", graph_semantics_type_is_resource(node->type) ? "true" : "false");
    graph_semantics_append_source_range_json(buf, node);
    zbuf_append(buf, "}");
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_borrowing_json(ZBuf *buf, const ZProgramGraph *graph) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_BORROW && !graph_semantics_type_is_borrow(node->type)) continue;
    if (!first) zbuf_append(buf, ",");
    const ZProgramGraphNode *target = node->kind == Z_PROGRAM_GRAPH_NODE_BORROW ? graph_semantics_child(graph, node, "left", 0) : NULL;
    zbuf_append(buf, "{\"node\":");
    graph_semantics_append_quoted(buf, node->id);
    zbuf_append(buf, ",\"kind\":");
    graph_semantics_append_quoted(buf, z_program_graph_node_kind_name(node->kind));
    zbuf_append(buf, ",\"type\":");
    graph_semantics_append_quoted(buf, node->type);
    zbuf_append(buf, ",\"borrowKind\":");
    graph_semantics_append_quoted(buf, graph_semantics_ownership_kind(node->type));
    zbuf_appendf(buf, ",\"mutable\":%s,\"target\":", (node->is_mutable || graph_semantics_text_contains(node->type, "mut")) ? "true" : "false");
    graph_semantics_append_quoted(buf, target ? target->id : "");
    zbuf_append(buf, ",\"sourceRange\":");
    graph_semantics_append_source_range_json(buf, node);
    zbuf_append(buf, "}");
    first = false;
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_resources_json(ZBuf *buf, const ZProgramGraph *graph) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (graph_semantics_node_has_type_fact(node) && graph_semantics_type_is_resource(node->type)) {
      if (!first) zbuf_append(buf, ",");
      zbuf_append(buf, "{\"node\":");
      graph_semantics_append_quoted(buf, node->id);
      zbuf_append(buf, ",\"kind\":\"binding\",\"resourceKind\":");
      graph_semantics_append_quoted(buf, graph_semantics_resource_kind(node->type, ""));
      zbuf_append(buf, ",\"type\":");
      graph_semantics_append_quoted(buf, node->type);
      zbuf_append(buf, ",\"sourceRange\":");
      graph_semantics_append_source_range_json(buf, node);
      zbuf_append(buf, "}");
      first = false;
    }
    if (node->kind != Z_PROGRAM_GRAPH_NODE_CALL && node->kind != Z_PROGRAM_GRAPH_NODE_METHOD_CALL) continue;
    char *qualified = graph_semantics_expr_chain(graph, node);
    ZGraphSemanticContract contract = graph_semantics_contract(graph, node, qualified && qualified[0] ? qualified : node->name);
    if (graph_semantics_contract_is_resource(&contract)) {
      if (!first) zbuf_append(buf, ",");
      const char *capability = graph_semantics_contract_capability(&contract);
      zbuf_append(buf, "{\"node\":");
      graph_semantics_append_quoted(buf, node->id);
      zbuf_append(buf, ",\"kind\":\"capabilityUse\",\"resourceKind\":");
      graph_semantics_append_quoted(buf, graph_semantics_resource_kind(node->type, capability));
      zbuf_append(buf, ",\"capability\":");
      graph_semantics_append_quoted(buf, capability);
      zbuf_append(buf, ",\"qualifiedName\":");
      graph_semantics_append_quoted(buf, qualified && qualified[0] ? qualified : node->name);
      zbuf_append(buf, ",\"sourceRange\":");
      graph_semantics_append_source_range_json(buf, node);
      zbuf_append(buf, "}");
      first = false;
    }
    free(qualified);
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_target_requirements_json(ZBuf *buf, const ZProgramGraph *graph) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_CALL && node->kind != Z_PROGRAM_GRAPH_NODE_METHOD_CALL) continue;
    char *qualified = graph_semantics_expr_chain(graph, node);
    ZGraphSemanticContract contract = graph_semantics_contract(graph, node, qualified && qualified[0] ? qualified : node->name);
    if (graph_semantics_contract_has_target_requirement(&contract)) {
      if (!first) zbuf_append(buf, ",");
      zbuf_append(buf, "{\"node\":");
      graph_semantics_append_quoted(buf, node->id);
      zbuf_append(buf, ",\"qualifiedName\":");
      graph_semantics_append_quoted(buf, qualified && qualified[0] ? qualified : node->name);
      zbuf_append(buf, ",\"contractKind\":");
      graph_semantics_append_quoted(buf, graph_semantics_contract_kind(&contract));
      zbuf_append(buf, ",\"capability\":");
      graph_semantics_append_quoted(buf, graph_semantics_contract_capability(&contract));
      zbuf_append(buf, ",\"targetSupport\":");
      graph_semantics_append_quoted(buf, graph_semantics_contract_target_support(&contract));
      zbuf_append(buf, ",\"sourceRange\":");
      graph_semantics_append_source_range_json(buf, node);
      zbuf_append(buf, "}");
      first = false;
    }
    free(qualified);
  }
  zbuf_append(buf, "]");
}

static void graph_semantics_append_repairs_json(ZBuf *buf, const ZProgramGraph *graph) {
  zbuf_append(buf, "[");
  bool first = true;
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_CALL && node->kind != Z_PROGRAM_GRAPH_NODE_METHOD_CALL) continue;
    char *qualified = graph_semantics_expr_chain(graph, node);
    bool checked = graph_semantics_is_under_kind(graph, node, Z_PROGRAM_GRAPH_NODE_CHECK) ||
                   graph_semantics_is_under_kind(graph, node, Z_PROGRAM_GRAPH_NODE_RESCUE);
    ZGraphSemanticContract contract = graph_semantics_contract(graph, node, qualified && qualified[0] ? qualified : node->name);
    if (graph_semantics_contract_has_repair(&contract)) {
      if (!first) zbuf_append(buf, ",");
      zbuf_append(buf, "{\"node\":");
      graph_semantics_append_quoted(buf, node->id);
      zbuf_append(buf, ",\"qualifiedName\":");
      graph_semantics_append_quoted(buf, qualified && qualified[0] ? qualified : node->name);
      zbuf_appendf(buf, ",\"requiresCheck\":%s,\"repair\":", contract.fallible && !checked ? "true" : "false");
      graph_semantics_append_repair_json(buf, &contract, checked);
      zbuf_append(buf, ",\"sourceRange\":");
      graph_semantics_append_source_range_json(buf, node);
      zbuf_append(buf, "}");
      first = false;
    }
    free(qualified);
  }
  zbuf_append(buf, "]");
}

void z_program_graph_append_semantics_json(ZBuf *buf, const ZProgramGraph *graph) {
  size_t fallible_calls = 0;
  size_t contracts = 0;
  graph_semantics_call_summary_counts(graph, &fallible_calls, &contracts);
  zbuf_append(buf, "{\"state\":\"typed-facts\",\"ok\":true");
  zbuf_appendf(buf,
               ",\"counts\":{\"typedNodes\":%zu,\"functions\":%zu,\"calls\":%zu,\"fallibleCalls\":%zu,\"effects\":%zu,\"contracts\":%zu,\"ownership\":%zu,\"borrowing\":%zu,\"resources\":%zu,\"targetRequirements\":%zu,\"repairs\":%zu,\"diagnostics\":0}",
               graph_semantics_typed_node_count(graph),
               graph_semantics_function_count(graph),
               graph_semantics_call_count(graph),
               fallible_calls,
               graph_semantics_effect_fact_count(graph),
               contracts,
               graph_semantics_ownership_fact_count(graph),
               graph_semantics_borrowing_fact_count(graph),
               graph_semantics_resource_fact_count(graph),
               graph_semantics_target_requirement_count(graph),
               graph_semantics_repair_fact_count(graph));
  zbuf_append(buf, ",\"types\":");
  graph_semantics_append_types_json(buf, graph);
  zbuf_append(buf, ",\"functions\":");
  graph_semantics_append_functions_json(buf, graph);
  zbuf_append(buf, ",\"calls\":");
  graph_semantics_append_calls_json(buf, graph);
  zbuf_append(buf, ",\"effects\":");
  graph_semantics_append_effects_json(buf, graph);
  zbuf_append(buf, ",\"ownership\":");
  graph_semantics_append_ownership_json(buf, graph);
  zbuf_append(buf, ",\"borrowing\":");
  graph_semantics_append_borrowing_json(buf, graph);
  zbuf_append(buf, ",\"resources\":");
  graph_semantics_append_resources_json(buf, graph);
  zbuf_append(buf, ",\"targetRequirements\":");
  graph_semantics_append_target_requirements_json(buf, graph);
  zbuf_append(buf, ",\"repairs\":");
  graph_semantics_append_repairs_json(buf, graph);
  zbuf_append(buf, ",\"diagnostics\":[]");
  zbuf_append(buf, "}");
}
