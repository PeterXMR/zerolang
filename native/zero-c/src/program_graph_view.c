#include "program_graph_view.h"

#include "canonical_text.h"
#include "program_graph_adjacency.h"
#include "program_graph_lower.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool view_text_eq(const char *left, const char *right) {
  return strcmp(left ? left : "", right ? right : "") == 0;
}

static bool view_starts_with(const char *text, const char *prefix) {
  return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool view_import_targets_rendered_module(const SourceInput *source, const char *module) {
  if (!source || !module || !module[0] || view_starts_with(module, "std.")) return false;
  for (size_t i = 0; i < source->module_count; i++) {
    if (view_text_eq(source->module_names[i], module)) return true;
  }
  return false;
}

static void view_drop_flattened_module_imports(Program *program, const SourceInput *source) {
  if (!program || !source || source->module_count <= 1) return;
  size_t write = 0;
  for (size_t read = 0; read < program->use_imports.len; read++) {
    UseImport item = program->use_imports.items[read];
    if (view_import_targets_rendered_module(source, item.module)) {
      free(item.module);
      free(item.alias);
      continue;
    }
    if (write != read) program->use_imports.items[write] = item;
    write++;
  }
  program->use_imports.len = write;
}

static void view_copy_node(ZProgramGraph *dst, const ZProgramGraphNode *src) {
  dst->nodes = z_checked_reallocarray(dst->nodes, dst->node_len + 1, sizeof(ZProgramGraphNode));
  ZProgramGraphNode *node = &dst->nodes[dst->node_len++];
  *node = (ZProgramGraphNode){
    .kind = src->kind,
    .line = src->line,
    .column = src->column,
    .is_public = src->is_public,
    .is_mutable = src->is_mutable,
    .is_static = src->is_static,
    .fallible = src->fallible,
    .export_c = src->export_c,
  };
  node->id = z_strdup(src->id ? src->id : "");
  node->name = z_strdup(src->name ? src->name : "");
  node->type = z_strdup(src->type ? src->type : "");
  node->value = z_strdup(src->value ? src->value : "");
  node->path = z_strdup(src->path ? src->path : "");
  node->symbol_id = z_strdup(src->symbol_id ? src->symbol_id : "");
  node->type_id = z_strdup(src->type_id ? src->type_id : "");
  node->effect_id = z_strdup(src->effect_id ? src->effect_id : "");
  node->node_hash = z_strdup(src->node_hash ? src->node_hash : "");
}

static void view_copy_edge(ZProgramGraph *dst, const ZProgramGraphEdge *src) {
  dst->edges = z_checked_reallocarray(dst->edges, dst->edge_len + 1, sizeof(ZProgramGraphEdge));
  ZProgramGraphEdge *edge = &dst->edges[dst->edge_len++];
  *edge = (ZProgramGraphEdge){
    .target = src->target,
    .order = src->order,
  };
  edge->from = z_strdup(src->from ? src->from : "");
  edge->to = z_strdup(src->to ? src->to : "");
  edge->kind = z_strdup(src->kind ? src->kind : "");
}

static bool view_included_node_id(const ZProgramGraphAdjacency *adjacency, const bool *included, const char *id) {
  size_t index = z_program_graph_adjacency_node_index(adjacency, id);
  return index != SIZE_MAX && included[index];
}

static void view_include_reachable_nodes(const ZProgramGraphAdjacency *adjacency, bool *included, size_t index) {
  const ZProgramGraph *graph = adjacency->graph;
  if (!graph || !included || index >= graph->node_len || included[index]) return;
  included[index] = true;
  size_t start = 0;
  size_t len = 0;
  z_program_graph_adjacency_owner_run(adjacency, graph->nodes[index].id, NULL, &start, &len);
  for (size_t i = start; i < start + len; i++) {
    const ZProgramGraphEdge *edge = z_program_graph_adjacency_owner_edge_at(adjacency, i);
    size_t target = z_program_graph_adjacency_node_index(adjacency, edge->to);
    if (target != SIZE_MAX) view_include_reachable_nodes(adjacency, included, target);
  }
}

static bool view_graph_has_symbol(const ZProgramGraph *graph, const bool *included, const char *symbol_id) {
  for (size_t i = 0; graph && symbol_id && i < graph->node_len; i++) {
    if (included[i] && view_text_eq(graph->nodes[i].symbol_id, symbol_id)) return true;
  }
  return false;
}

static bool view_graph_has_type(const ZProgramGraph *graph, const bool *included, const char *type_id) {
  for (size_t i = 0; graph && type_id && i < graph->node_len; i++) {
    if (included[i] && view_text_eq(graph->nodes[i].type_id, type_id)) return true;
  }
  return false;
}

static bool view_graph_has_effect(const ZProgramGraph *graph, const bool *included, const char *effect_id) {
  for (size_t i = 0; graph && effect_id && i < graph->node_len; i++) {
    if (included[i] && view_text_eq(graph->nodes[i].effect_id, effect_id)) return true;
  }
  return false;
}

static bool view_edge_target_included(const ZProgramGraph *graph, const ZProgramGraphAdjacency *adjacency, const bool *included, const ZProgramGraphEdge *edge) {
  switch (edge->target) {
    case Z_PROGRAM_GRAPH_EDGE_TARGET_NODE:
      return view_included_node_id(adjacency, included, edge->to);
    case Z_PROGRAM_GRAPH_EDGE_TARGET_SYMBOL:
      return view_graph_has_symbol(graph, included, edge->to);
    case Z_PROGRAM_GRAPH_EDGE_TARGET_TYPE:
      return view_graph_has_type(graph, included, edge->to);
    case Z_PROGRAM_GRAPH_EDGE_TARGET_EFFECT:
      return view_graph_has_effect(graph, included, edge->to);
  }
  return false;
}

static bool view_slice_graph_for_source(const ZProgramGraph *graph, const char *source_path, ZProgramGraph *out, ZDiag *diag) {
  if (!graph || !source_path || !source_path[0]) return false;
  ZProgramGraphAdjacency adjacency;
  z_program_graph_adjacency_init(&adjacency, graph);
  bool *included = calloc(graph->node_len ? graph->node_len : 1, sizeof(bool));
  if (!included) {
    z_program_graph_adjacency_free(&adjacency);
    if (diag) {
      diag->code = 2001;
      diag->path = source_path;
      diag->line = 1;
      diag->column = 1;
      diag->length = 1;
      snprintf(diag->message, sizeof(diag->message), "out of memory while rendering projection-backed graph view");
    }
    return false;
  }

  bool found_module = false;
  for (size_t i = 0; i < graph->node_len; i++) {
    if (graph->nodes[i].kind == Z_PROGRAM_GRAPH_NODE_MODULE && view_text_eq(graph->nodes[i].path, source_path)) {
      found_module = true;
      view_include_reachable_nodes(&adjacency, included, i);
    }
  }
  for (size_t i = 0; i < graph->node_len; i++) {
    if (graph->nodes[i].kind == Z_PROGRAM_GRAPH_NODE_MODULE) included[i] = true;
  }

  if (!found_module) {
    z_program_graph_adjacency_free(&adjacency);
    free(included);
    if (diag) {
      diag->code = 2002;
      diag->path = source_path;
      diag->line = 1;
      diag->column = 1;
      diag->length = 1;
      snprintf(diag->message, sizeof(diag->message), "projection-backed graph has no module for input file");
      snprintf(diag->expected, sizeof(diag->expected), "Module node with matching source path");
      snprintf(diag->actual, sizeof(diag->actual), "missing module");
      snprintf(diag->help, sizeof(diag->help), "dump the graph and patch a node that belongs to the input .0 file");
    }
    return false;
  }

  z_program_graph_init(out);
  free(out->module_identity);
  out->module_identity = z_strdup(graph->module_identity ? graph->module_identity : "module:main");
  out->validation_state = graph->validation_state;
  out->canonical_source = graph->canonical_source;
  out->next_id = graph->next_id;
  if (graph->graph_hash) out->graph_hash = z_strdup(graph->graph_hash);
  for (size_t i = 0; i < graph->node_len; i++) {
    if (included[i]) view_copy_node(out, &graph->nodes[i]);
  }
  for (size_t i = 0; i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (!view_included_node_id(&adjacency, included, edge->from)) continue;
    if (!view_edge_target_included(graph, &adjacency, included, edge)) continue;
    view_copy_edge(out, edge);
  }
  z_program_graph_adjacency_free(&adjacency);
  free(included);
  return true;
}

static bool view_append_program(ZBuf *buf, const ZProgramGraph *graph, const char *source_path, bool drop_flattened_imports, ZDiag *diag) {
  if (!buf || !graph) return false;
  Program program = {0};
  SourceInput input = {0};
  const char *path = source_path && source_path[0] ? source_path : "<program-graph>";
  bool ok = z_program_graph_lower_to_program_for_roundtrip(graph, path, &program, &input, diag);
  if (ok && drop_flattened_imports) view_drop_flattened_module_imports(&program, &input);
  if (ok) ok = z_canonical_text_write_program(&program, buf, diag);
  if (ok) {
    Program parsed = {0};
    ZDiag parse_diag = {0};
    ok = z_parse_canonical_text_program_source(buf->data ? buf->data : "", &parsed, &parse_diag);
    if (!ok && diag) {
      *diag = parse_diag;
      if (!diag->path) diag->path = path;
    }
    z_free_program(&parsed);
  }
  if (!ok && diag && diag->code == 0) {
    diag->code = 2002;
    diag->path = path;
    diag->line = 1;
    diag->column = 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "failed to render program graph as canonical source");
    snprintf(diag->expected, sizeof(diag->expected), "lowerable ProgramGraph");
    snprintf(diag->actual, sizeof(diag->actual), "invalid graph view");
    snprintf(diag->help, sizeof(diag->help), "run zero check to inspect graph lowering diagnostics");
  }
  z_free_program(&program);
  z_free_source(&input);
  return ok;
}

bool z_program_graph_append_view(ZBuf *buf, const ZProgramGraph *graph, const char *source_path, ZDiag *diag) {
  return view_append_program(buf, graph, source_path, true, diag);
}

static size_t view_name_distance(const char *left, const char *right) {
  size_t left_len = left ? strlen(left) : 0;
  size_t right_len = right ? strlen(right) : 0;
  enum { VIEW_NAME_DISTANCE_MAX = 64 };
  if (left_len > VIEW_NAME_DISTANCE_MAX || right_len > VIEW_NAME_DISTANCE_MAX) {
    return left_len > right_len ? left_len : right_len;
  }
  size_t row[VIEW_NAME_DISTANCE_MAX + 1];
  for (size_t j = 0; j <= right_len; j++) row[j] = j;
  for (size_t i = 1; i <= left_len; i++) {
    size_t previous_diagonal = row[0];
    row[0] = i;
    for (size_t j = 1; j <= right_len; j++) {
      size_t previous = row[j];
      size_t substitute = previous_diagonal + ((left[i - 1] == right[j - 1] || (left[i - 1] | 32) == (right[j - 1] | 32)) ? 0 : 1);
      size_t insert = row[j - 1] + 1;
      size_t delete_cost = previous + 1;
      size_t best = substitute < insert ? substitute : insert;
      row[j] = best < delete_cost ? best : delete_cost;
      previous_diagonal = previous;
    }
  }
  return row[right_len];
}

static void view_collect_function_suggestions(const ZProgramGraph *graph, const char *function_name, char *out, size_t out_size) {
  enum { VIEW_SUGGESTION_MAX = 3 };
  const char *names[VIEW_SUGGESTION_MAX] = {0};
  size_t distances[VIEW_SUGGESTION_MAX] = {0};
  size_t count = 0;
  size_t name_len = function_name ? strlen(function_name) : 0;
  size_t threshold = name_len / 3 + 2;
  if (out_size) out[0] = '\0';
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_FUNCTION || !node->name || !node->name[0]) continue;
    size_t distance = view_name_distance(function_name, node->name);
    if (distance > threshold) continue;
    bool duplicate = false;
    for (size_t known = 0; known < count && !duplicate; known++) {
      duplicate = view_text_eq(names[known], node->name);
    }
    if (duplicate) continue;
    size_t slot = count < VIEW_SUGGESTION_MAX ? count : VIEW_SUGGESTION_MAX;
    for (size_t pos = 0; pos < count; pos++) {
      if (distance < distances[pos]) {
        slot = pos;
        break;
      }
    }
    if (slot >= VIEW_SUGGESTION_MAX) continue;
    size_t last = count < VIEW_SUGGESTION_MAX ? count : VIEW_SUGGESTION_MAX - 1;
    for (size_t pos = last; pos > slot; pos--) {
      names[pos] = names[pos - 1];
      distances[pos] = distances[pos - 1];
    }
    names[slot] = node->name;
    distances[slot] = distance;
    if (count < VIEW_SUGGESTION_MAX) count++;
  }
  size_t used = 0;
  for (size_t i = 0; i < count && out_size; i++) {
    int written = snprintf(out + used, out_size - used, "%s%s", i ? ", " : "", names[i]);
    if (written < 0 || (size_t)written >= out_size - used) break;
    used += (size_t)written;
  }
}

static bool view_line_is_function_header(const char *line, size_t line_len, const char *function_name) {
  size_t name_len = strlen(function_name);
  if (line_len >= 4 && strncmp(line, "pub ", 4) == 0) {
    line += 4;
    line_len -= 4;
  }
  if (line_len < 3 || strncmp(line, "fn ", 3) != 0) return false;
  line += 3;
  line_len -= 3;
  return line_len > name_len && strncmp(line, function_name, name_len) == 0 && line[name_len] == '(';
}

bool z_program_graph_append_view_function(ZBuf *buf, const ZProgramGraph *graph, const char *source_path, const char *function_name, ZDiag *diag) {
  if (!buf || !function_name || !function_name[0]) return false;
  ZBuf full;
  zbuf_init(&full);
  if (!view_append_program(&full, graph, source_path, true, diag)) {
    zbuf_free(&full);
    return false;
  }
  const char *text = full.data ? full.data : "";
  const char *line = text;
  const char *function_start = NULL;
  while (*line) {
    const char *line_end = strchr(line, '\n');
    size_t line_len = line_end ? (size_t)(line_end - line) : strlen(line);
    if (!function_start && view_line_is_function_header(line, line_len, function_name)) {
      function_start = line;
    } else if (function_start && line_len == 1 && line[0] == '}') {
      full.data[(size_t)(line + line_len - text)] = '\0';
      zbuf_append(buf, function_start);
      zbuf_append_char(buf, '\n');
      zbuf_free(&full);
      return true;
    }
    if (!line_end) break;
    line = line_end + 1;
  }
  zbuf_free(&full);
  if (diag) {
    char suggestions[160];
    view_collect_function_suggestions(graph, function_name, suggestions, sizeof(suggestions));
    diag->code = 2002;
    diag->path = source_path;
    diag->line = 1;
    diag->column = 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "function '%s' not found in graph view", function_name);
    snprintf(diag->expected, sizeof(diag->expected), "an existing function name");
    snprintf(diag->actual, sizeof(diag->actual), "--fn %s", function_name);
    if (suggestions[0]) {
      snprintf(diag->help, sizeof(diag->help), "close matches: %s; run zero view --fn <name> with one of them, or zero query --find %s to search the graph", suggestions, function_name);
    } else {
      snprintf(diag->help, sizeof(diag->help), "run zero query to list functions, or zero query --find %s to search the graph", function_name);
    }
  }
  return false;
}

bool z_program_graph_append_source_view(ZBuf *buf, const ZProgramGraph *graph, const char *source_path, ZDiag *diag) {
  ZProgramGraph sliced = {0};
  if (!view_slice_graph_for_source(graph, source_path, &sliced, diag)) return false;
  bool ok = view_append_program(buf, &sliced, source_path, false, diag);
  z_program_graph_free(&sliced);
  return ok;
}
