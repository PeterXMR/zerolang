#include "program_graph_patch_body.h"
#include "canonical_text.h"
#include "program_graph_handle.h"
#include "program_graph_import.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *from;
  char *to;
} BodyIdMap;

static char *body_expr_source(const char *expr);

static bool body_text_eq(const char *left, const char *right) {
  return strcmp(left ? left : "", right ? right : "") == 0;
}

static void body_replace(char **slot, const char *value) {
  if (!slot) return;
  free(*slot);
  *slot = z_strdup(value ? value : "");
}

static void body_fail(ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op, const char *code, const char *message, const char *expected, const char *actual) {
  if (op) {
    op->ok = false;
    snprintf(op->code, sizeof(op->code), "%s", code ? code : "GPH000");
    snprintf(op->message, sizeof(op->message), "%s", message ? message : "program graph patch operation failed");
    body_replace(&op->expected, expected);
    body_replace(&op->actual, actual);
  }
  if (result) {
    result->ok = false;
    snprintf(result->code, sizeof(result->code), "%s", code ? code : "GPH000");
    snprintf(result->message, sizeof(result->message), "%s", message ? message : "program graph patch failed");
    body_replace(&result->expected, expected);
    body_replace(&result->actual, actual);
  }
}

static char *body_trim(char *line) {
  while (*line && isspace((unsigned char)*line)) line++;
  char *end = line + strlen(line);
  while (end > line && isspace((unsigned char)*(end - 1))) *(--end) = '\0';
  return line;
}

static bool body_identifier(const char *text) {
  if (!text || !text[0] || !(isalpha((unsigned char)text[0]) || text[0] == '_')) return false;
  for (const unsigned char *p = (const unsigned char *)text + 1; *p; p++) {
    if (!(isalnum(*p) || *p == '_')) return false;
  }
  return true;
}

static char *body_diag_row_actual(const char *rows, const ZDiag *diag, const char *fallback) {
  int wanted = diag && diag->line > 1 ? diag->line - 1 : 0;
  char *copy = z_strdup(rows ? rows : "");
  char *cursor = copy;
  int emitted = 0;
  int physical = 0;
  char *last = NULL;
  while (*cursor) {
    char *line = cursor;
    char *end = strchr(cursor, '\n');
    if (end) {
      *end = '\0';
      cursor = end + 1;
    } else {
      cursor += strlen(cursor);
    }
    physical++;
    char *trimmed = body_trim(line);
    if (!trimmed[0] || trimmed[0] == '#') continue;
    emitted++;
    free(last);
    ZBuf row;
    zbuf_init(&row);
    zbuf_appendf(&row, "row %d: ", physical);
    zbuf_append(&row, trimmed);
    last = row.data ? row.data : z_strdup("");
    if (emitted == wanted) {
      free(copy);
      return last;
    }
  }
  free(copy);
  if (last) return last;
  return z_strdup(fallback ? fallback : "");
}

static bool body_tokenize(const char *text, char ***out, size_t *out_len) {
  char **items = NULL;
  size_t len = 0, cap = 0;
  const char *cursor = text ? text : "";
  while (*cursor) {
    while (*cursor && isspace((unsigned char)*cursor)) cursor++;
    if (!*cursor) break;
    ZBuf token;
    zbuf_init(&token);
    if (*cursor == '"') {
      zbuf_append_char(&token, *cursor++);
      bool escaped = false;
      while (*cursor) {
        char ch = *cursor++;
        zbuf_append_char(&token, ch);
        if (escaped) {
          escaped = false;
        } else if (ch == '\\') {
          escaped = true;
        } else if (ch == '"') {
          break;
        }
      }
    } else if (*cursor == '(') {
      size_t depth = 0;
      bool quoted = false;
      bool escaped = false;
      while (*cursor) {
        char ch = *cursor++;
        zbuf_append_char(&token, ch);
        if (quoted) {
          if (escaped) escaped = false;
          else if (ch == '\\') escaped = true;
          else if (ch == '"') quoted = false;
        } else if (ch == '"') {
          quoted = true;
        } else if (ch == '(') {
          depth++;
        } else if (ch == ')') {
          if (depth) depth--;
          if (!depth) break;
        }
      }
    } else {
      while (*cursor && !isspace((unsigned char)*cursor)) zbuf_append_char(&token, *cursor++);
    }
    if (len == cap) {
      size_t next = cap ? cap * 2 : 4;
      items = z_checked_reallocarray(items, next, sizeof(char *));
      cap = next;
    }
    items[len++] = token.data ? token.data : z_strdup("");
  }
  *out = items;
  *out_len = len;
  return true;
}

static void body_tokens_free(char **items, size_t len) {
  for (size_t i = 0; i < len; i++) free(items[i]);
  free(items);
}

static void body_append_indent(ZBuf *out, size_t indent) {
  for (size_t i = 0; i < indent; i++) zbuf_append(out, "    ");
}

static char *body_call_source(char **tokens, size_t len) {
  ZBuf out;
  zbuf_init(&out);
  zbuf_append(&out, tokens[0]);
  zbuf_append_char(&out, '(');
  for (size_t i = 1; i < len; i++) {
    if (i > 1) zbuf_append(&out, ", ");
    char *arg = body_expr_source(tokens[i]);
    zbuf_append(&out, arg);
    free(arg);
  }
  zbuf_append_char(&out, ')');
  return out.data ? out.data : z_strdup("");
}

static char *body_find_operator(char *expr, const char *const *ops, size_t op_len, const char **out_op) {
  bool quoted = false;
  size_t depth = 0;
  for (char *cursor = expr; cursor && *cursor; cursor++) {
    if (*cursor == '"' && (cursor == expr || cursor[-1] != '\\')) { quoted = !quoted; continue; }
    if (quoted) continue;
    if (*cursor == '(' || *cursor == '[') { depth++; continue; }
    if ((*cursor == ')' || *cursor == ']') && depth) { depth--; continue; }
    if (depth) continue;
    for (size_t i = 0; i < op_len; i++) {
      if (strncmp(cursor, ops[i], strlen(ops[i])) == 0) { *out_op = ops[i]; return cursor; }
    }
  }
  return NULL;
}

static char *body_infix_source(char *trimmed, const char *const *ops, size_t op_len) {
  const char *op = NULL;
  char *found = body_find_operator(trimmed, ops, op_len, &op);
  if (!found) return NULL;
  *found = '\0';
  char *left = body_expr_source(trimmed);
  char *right = body_expr_source(found + strlen(op));
  ZBuf out;
  zbuf_init(&out);
  zbuf_append(&out, left);
  zbuf_append_char(&out, ' ');
  zbuf_appendf(&out, "%.*s", (int)(strlen(op) - 2), op + 1);
  zbuf_append_char(&out, ' ');
  zbuf_append(&out, right);
  free(left);
  free(right);
  return out.data ? out.data : z_strdup("");
}

static bool body_outer_parens_wrap(const char *text) {
  if (!text || text[0] != '(') return false;
  size_t depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (size_t i = 0; text[i]; i++) {
    char ch = text[i];
    if (quoted) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') quoted = false;
      continue;
    }
    if (ch == '"') { quoted = true; continue; }
    if (ch == '(') depth++;
    if (ch == ')' && depth) {
      depth--;
      if (depth == 0 && text[i + 1] != '\0') return false;
    }
  }
  return depth == 0;
}

static char *body_expr_source(const char *expr) {
  char *copy = z_strdup(expr ? expr : "");
  char *trimmed = body_trim(copy);
  if (body_outer_parens_wrap(trimmed)) {
    trimmed[strlen(trimmed) - 1] = '\0';
    char *out = body_expr_source(trimmed + 1);
    free(copy);
    return out;
  }
  if (strncmp(trimmed, "check ", 6) == 0 || strncmp(trimmed, "meta ", 5) == 0) {
    const char *prefix = strncmp(trimmed, "check ", 6) == 0 ? "check" : "meta";
    size_t prefix_len = strlen(prefix);
    char *operand = body_expr_source(trimmed + prefix_len + 1);
    ZBuf out;
    zbuf_init(&out);
    zbuf_append(&out, prefix);
    zbuf_append_char(&out, ' ');
    zbuf_append(&out, operand);
    free(operand);
    free(copy);
    return out.data ? out.data : z_strdup("");
  }
  if (trimmed[0] == '!' && trimmed[1] != '=') {
    char *operand = body_expr_source(trimmed + 1);
    ZBuf out;
    zbuf_init(&out);
    zbuf_append_char(&out, '!');
    zbuf_append(&out, operand);
    free(operand);
    free(copy);
    return out.data ? out.data : z_strdup("");
  }
  const char *or_ops[] = {" || "};
  const char *and_ops[] = {" && "};
  const char *compare_ops[] = {" == ", " != ", " <= ", " >= ", " < ", " > "};
  const char *cast_ops[] = {" as "};
  const char *add_ops[] = {" + ", " - "};
  const char *mul_ops[] = {" * ", " / ", " % "};
  char *infix = body_infix_source(trimmed, or_ops, sizeof(or_ops) / sizeof(or_ops[0]));
  if (!infix) infix = body_infix_source(trimmed, and_ops, sizeof(and_ops) / sizeof(and_ops[0]));
  if (!infix) infix = body_infix_source(trimmed, compare_ops, sizeof(compare_ops) / sizeof(compare_ops[0]));
  if (!infix) infix = body_infix_source(trimmed, cast_ops, sizeof(cast_ops) / sizeof(cast_ops[0]));
  if (!infix) infix = body_infix_source(trimmed, add_ops, sizeof(add_ops) / sizeof(add_ops[0]));
  if (!infix) infix = body_infix_source(trimmed, mul_ops, sizeof(mul_ops) / sizeof(mul_ops[0]));
  if (infix) { free(copy); return infix; }
  if (trimmed[0] == '"' || trimmed[0] == '[' || trimmed[0] == 0) {
    char *out = z_strdup(trimmed);
    free(copy);
    return out;
  }
  char **tokens = NULL;
  size_t len = 0;
  body_tokenize(trimmed, &tokens, &len);
  char *out = NULL;
  if (len > 1 && (strchr(tokens[0], '.') || body_identifier(tokens[0]))) {
    out = body_call_source(tokens, len);
  } else if (len == 1 && strchr(trimmed, '(')) {
    out = z_strdup(trimmed);
  } else {
    out = z_strdup(trimmed);
  }
  body_tokens_free(tokens, len);
  free(copy);
  return out;
}

static bool body_translate_row(const char *row, ZBuf *source, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op) {
  char *copy = z_strdup(row ? row : "");
  char *line = body_trim(copy);
  if (strncmp(line, "let ", 4) == 0 || strncmp(line, "var ", 4) == 0) {
    bool mut = line[0] == 'v';
    char *cursor = line + 4;
    char *eq = strstr(cursor, " = ");
    if (!eq) {
      body_fail(result, op, "GPH001", "body row let/var is missing '='", "let name Type = expr", row);
      free(copy);
      return false;
    }
    *eq = '\0';
    char **head = NULL;
    size_t head_len = 0;
    body_tokenize(cursor, &head, &head_len);
    if (head_len != 2 || !body_identifier(head[0])) {
      body_tokens_free(head, head_len);
      body_fail(result, op, "GPH001", "body row let/var has invalid binding header", "let name Type = expr", row);
      free(copy);
      return false;
    }
    char *expr = body_expr_source(eq + 3);
    if (mut && strncmp(eq + 3, "repeat ", 7) == 0) {
      char **repeat = NULL;
      size_t repeat_len = 0;
      body_tokenize(eq + 3, &repeat, &repeat_len);
      free(expr);
      if (repeat_len == 3) {
        ZBuf array;
        zbuf_init(&array);
        zbuf_append_char(&array, '[');
        zbuf_append(&array, repeat[1]);
        zbuf_append(&array, "; ");
        zbuf_append(&array, repeat[2]);
        zbuf_append_char(&array, ']');
        expr = array.data ? array.data : z_strdup("");
      } else {
        expr = z_strdup(eq + 3);
      }
      body_tokens_free(repeat, repeat_len);
    }
    zbuf_append(source, mut ? "var " : "let ");
    zbuf_append(source, head[0]);
    zbuf_append(source, ": ");
    zbuf_append(source, head[1]);
    zbuf_append(source, " = ");
    zbuf_append(source, expr);
    body_tokens_free(head, head_len);
    free(expr);
    free(copy);
    return true;
  }
  char *assignment = strstr(line, " = ");
  if (assignment) {
    *assignment = '\0';
    char *target = body_trim(line);
    if (!target[0]) {
      body_fail(result, op, "GPH001", "body row assignment is missing a target", "target = expr", row);
      free(copy);
      return false;
    }
    char *expr = body_expr_source(assignment + 3);
    zbuf_append(source, target);
    zbuf_append(source, " = ");
    zbuf_append(source, expr);
    free(expr);
    free(copy);
    return true;
  }
  if (strncmp(line, "check ", 6) == 0) {
    char *expr = body_expr_source(line + 6);
    zbuf_append(source, "check ");
    zbuf_append(source, expr);
    free(expr);
    free(copy);
    return true;
  }
  if (strncmp(line, "return ", 7) == 0) {
    char *expr = body_expr_source(line + 7);
    zbuf_append(source, "return ");
    zbuf_append(source, expr);
    free(expr);
    free(copy);
    return true;
  }
  if (strcmp(line, "return") == 0) {
    zbuf_append(source, "return");
    free(copy);
    return true;
  }
  char *expr = body_expr_source(line);
  zbuf_append(source, expr);
  free(expr);
  free(copy);
  return true;
}

static char *body_diag_physical_row_actual(const char *rows, const ZDiag *diag, const char *fallback) {
  int wanted = diag && diag->line > 1 ? diag->line - 1 : 0;
  char *copy = z_strdup(rows ? rows : "");
  char *cursor = copy;
  int physical = 0;
  char *last = NULL;
  while (*cursor) {
    char *line = cursor;
    char *end = strchr(cursor, '\n');
    if (end) {
      *end = '\0';
      cursor = end + 1;
    } else {
      cursor += strlen(cursor);
    }
    physical++;
    char *trimmed = body_trim(line);
    if (!trimmed[0]) continue;
    free(last);
    ZBuf row;
    zbuf_init(&row);
    zbuf_appendf(&row, "row %d: ", physical);
    zbuf_append(&row, trimmed);
    last = row.data ? row.data : z_strdup("");
    if (physical >= wanted) {
      free(copy);
      return last;
    }
  }
  free(copy);
  if (last) return last;
  return z_strdup(fallback ? fallback : "");
}

static bool body_append_source_rows(ZBuf *source, const char *rows, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op) {
  char *copy = z_strdup(rows ? rows : "");
  char *cursor = copy;
  size_t open = 0;
  while (*cursor) {
    char *line = cursor;
    char *end = strchr(cursor, '\n');
    if (end) {
      *end = '\0';
      cursor = end + 1;
    } else {
      cursor += strlen(cursor);
    }
    size_t spaces = 0;
    while (line[spaces] == ' ') spaces++;
    char *trimmed = body_trim(line);
    if (!trimmed[0] || trimmed[0] == '#') continue;
    size_t indent = spaces / 2;
    if (strcmp(trimmed, "else") == 0) {
      while (open > indent + 1) {
        body_append_indent(source, open);
        zbuf_append(source, "}\n");
        open--;
      }
      body_append_indent(source, indent + 1);
      zbuf_append(source, "} else {\n");
      open = indent + 1;
      continue;
    }
    if (strncmp(trimmed, "else if ", 8) == 0) {
      while (open > indent + 1) {
        body_append_indent(source, open);
        zbuf_append(source, "}\n");
        open--;
      }
      char *expr = body_expr_source(trimmed + 8);
      body_append_indent(source, indent + 1);
      zbuf_append(source, "} else if ");
      zbuf_append(source, expr);
      zbuf_append(source, " {\n");
      free(expr);
      open = indent + 1;
      continue;
    }
    while (open > indent) {
      body_append_indent(source, open);
      zbuf_append(source, "}\n");
      open--;
    }
    body_append_indent(source, indent + 1);
    if (strncmp(trimmed, "if ", 3) == 0 || strncmp(trimmed, "while ", 6) == 0) {
      bool loop = strncmp(trimmed, "while ", 6) == 0;
      char *expr = body_expr_source(trimmed + (loop ? 6 : 3));
      zbuf_append(source, loop ? "while " : "if ");
      zbuf_append(source, expr);
      zbuf_append(source, " {\n");
      free(expr);
      open = indent + 1;
    } else {
      if (!body_translate_row(trimmed, source, result, op)) {
        free(copy);
        return false;
      }
      zbuf_append_char(source, '\n');
    }
  }
  while (open > 0) {
    body_append_indent(source, open);
    zbuf_append(source, "}\n");
    open--;
  }
  free(copy);
  return true;
}

static ZProgramGraphNode *body_find_node(ZProgramGraph *graph, const char *id) {
  for (size_t i = 0; graph && id && i < graph->node_len; i++) {
    if (body_text_eq(graph->nodes[i].id, id)) return &graph->nodes[i];
  }
  return NULL;
}

static ZProgramGraphNode *body_find_function(ZProgramGraph *graph, const char *name, size_t *out_count) {
  ZProgramGraphNode *found = NULL;
  size_t count = 0;
  for (size_t i = 0; graph && name && i < graph->node_len; i++) {
    if (graph->nodes[i].kind == Z_PROGRAM_GRAPH_NODE_FUNCTION && body_text_eq(graph->nodes[i].name, name)) {
      found = &graph->nodes[i];
      count++;
    }
  }
  if (out_count) *out_count = count;
  return count == 1 ? found : NULL;
}

static ZProgramGraphNode *body_child(ZProgramGraph *graph, const char *from, const char *kind) {
  for (size_t i = 0; graph && from && kind && i < graph->edge_len; i++) {
    ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && body_text_eq(edge->from, from) && body_text_eq(edge->kind, kind)) return body_find_node(graph, edge->to);
  }
  return NULL;
}

static void body_mark_reachable(ZProgramGraph *graph, const char *id, bool *remove) {
  for (size_t i = 0; graph && id && i < graph->node_len; i++) {
    if (body_text_eq(graph->nodes[i].id, id)) {
      if (remove[i]) return;
      remove[i] = true;
      break;
    }
  }
  for (size_t i = 0; graph && id && i < graph->edge_len; i++) {
    ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && body_text_eq(edge->from, id)) body_mark_reachable(graph, edge->to, remove);
  }
}

static bool body_node_marked(ZProgramGraph *graph, bool *marked, const char *id) {
  for (size_t i = 0; graph && id && i < graph->node_len; i++) {
    if (body_text_eq(graph->nodes[i].id, id)) return marked[i];
  }
  return false;
}

static void body_free_node(ZProgramGraphNode *node) {
  free(node->id); free(node->name); free(node->type); free(node->value); free(node->path);
  free(node->symbol_id); free(node->type_id); free(node->effect_id); free(node->node_hash);
}

static void body_free_edge(ZProgramGraphEdge *edge) {
  free(edge->from); free(edge->to); free(edge->kind);
}

static void body_clear_statements(ZProgramGraph *graph, const char *body_id) {
  bool *remove = z_checked_calloc(graph->node_len ? graph->node_len : 1, sizeof(bool));
  for (size_t i = 0; i < graph->edge_len; i++) {
    ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && body_text_eq(edge->from, body_id) && body_text_eq(edge->kind, "statement")) body_mark_reachable(graph, edge->to, remove);
  }
  size_t edge_out = 0;
  for (size_t i = 0; i < graph->edge_len; i++) {
    ZProgramGraphEdge edge = graph->edges[i];
    bool drop = (edge.target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && (body_text_eq(edge.from, body_id) || body_node_marked(graph, remove, edge.from) || body_node_marked(graph, remove, edge.to)));
    if (drop) body_free_edge(&edge);
    else graph->edges[edge_out++] = edge;
  }
  graph->edge_len = edge_out;
  size_t node_out = 0;
  for (size_t i = 0; i < graph->node_len; i++) {
    if (remove[i]) body_free_node(&graph->nodes[i]);
    else graph->nodes[node_out++] = graph->nodes[i];
  }
  graph->node_len = node_out;
  free(remove);
}

static void body_reserve_nodes(ZProgramGraph *graph, size_t len) {
  if (graph->node_cap >= len) return;
  size_t next = graph->node_cap ? graph->node_cap * 2 : 8;
  while (next < len) next *= 2;
  graph->nodes = z_checked_reallocarray(graph->nodes, next, sizeof(ZProgramGraphNode));
  for (size_t i = graph->node_cap; i < next; i++) graph->nodes[i] = (ZProgramGraphNode){0};
  graph->node_cap = next;
}

static void body_reserve_edges(ZProgramGraph *graph, size_t len) {
  if (graph->edge_cap >= len) return;
  size_t next = graph->edge_cap ? graph->edge_cap * 2 : 8;
  while (next < len) next *= 2;
  graph->edges = z_checked_reallocarray(graph->edges, next, sizeof(ZProgramGraphEdge));
  for (size_t i = graph->edge_cap; i < next; i++) graph->edges[i] = (ZProgramGraphEdge){0};
  graph->edge_cap = next;
}

static char *body_unique_id(ZProgramGraph *graph, const char *id) {
  if (!body_find_node(graph, id)) return z_strdup(id ? id : "#node");
  for (size_t suffix = 1; suffix < 100000; suffix++) {
    ZBuf out;
    zbuf_init(&out);
    zbuf_append(&out, id ? id : "#node");
    zbuf_appendf(&out, "_%zu", suffix);
    if (!body_find_node(graph, out.data)) return out.data;
    zbuf_free(&out);
  }
  return z_strdup("#node_conflict");
}

static const char *body_mapped_id(BodyIdMap *map, size_t len, const char *from) {
  for (size_t i = 0; i < len; i++) {
    if (body_text_eq(map[i].from, from)) return map[i].to;
  }
  return from;
}

static void body_copy_node(ZProgramGraph *graph, const ZProgramGraphNode *src, const char *id, const char *path) {
  body_reserve_nodes(graph, graph->node_len + 1);
  ZProgramGraphNode *node = &graph->nodes[graph->node_len++];
  *node = *src;
  node->id = z_strdup(id);
  node->name = src->name ? z_strdup(src->name) : NULL;
  node->type = src->type ? z_strdup(src->type) : NULL;
  node->value = src->value ? z_strdup(src->value) : NULL;
  node->path = z_strdup(path && path[0] ? path : (src->path ? src->path : "src/main.0"));
  node->symbol_id = src->symbol_id ? z_strdup(src->symbol_id) : NULL;
  node->type_id = src->type_id ? z_strdup(src->type_id) : NULL;
  node->effect_id = src->effect_id ? z_strdup(src->effect_id) : NULL;
  node->node_hash = src->node_hash ? z_strdup(src->node_hash) : NULL;
}

static void body_copy_edge(ZProgramGraph *graph, const ZProgramGraphEdge *src, const char *from, const char *to) {
  body_reserve_edges(graph, graph->edge_len + 1);
  ZProgramGraphEdge *edge = &graph->edges[graph->edge_len++];
  edge->from = z_strdup(from);
  edge->to = z_strdup(to);
  edge->kind = z_strdup(src->kind ? src->kind : "");
  edge->target = src->target;
  edge->order = src->order;
}

static bool body_splice_block(ZProgramGraph *target, ZProgramGraph *source, const char *target_body_id, const char *source_body_id, const char *target_path) {
  if (!target || !source || !target_body_id || !source_body_id) return false;
  body_clear_statements(target, target_body_id);
  bool *copy = z_checked_calloc(source->node_len ? source->node_len : 1, sizeof(bool));
  for (size_t i = 0; i < source->edge_len; i++) {
    ZProgramGraphEdge *edge = &source->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && body_text_eq(edge->from, source_body_id) && body_text_eq(edge->kind, "statement")) body_mark_reachable(source, edge->to, copy);
  }
  BodyIdMap *map = z_checked_calloc(source->node_len ? source->node_len : 1, sizeof(BodyIdMap));
  size_t map_len = 0;
  for (size_t i = 0; i < source->node_len; i++) {
    if (!copy[i]) continue;
    map[map_len].from = source->nodes[i].id;
    map[map_len].to = body_unique_id(target, source->nodes[i].id);
    body_copy_node(target, &source->nodes[i], map[map_len].to, target_path);
    map_len++;
  }
  for (size_t i = 0; i < source->edge_len; i++) {
    ZProgramGraphEdge *edge = &source->edges[i];
    if (edge->target != Z_PROGRAM_GRAPH_EDGE_TARGET_NODE) continue;
    bool from_body = body_text_eq(edge->from, source_body_id) && body_text_eq(edge->kind, "statement");
    bool from_copy = body_node_marked(source, copy, edge->from);
    bool to_copy = body_node_marked(source, copy, edge->to);
    if (from_body && to_copy) body_copy_edge(target, edge, target_body_id, body_mapped_id(map, map_len, edge->to));
    else if (from_copy && to_copy) body_copy_edge(target, edge, body_mapped_id(map, map_len, edge->from), body_mapped_id(map, map_len, edge->to));
  }
  for (size_t i = 0; i < map_len; i++) free(map[i].to);
  free(map);
  free(copy);
  return true;
}

static bool body_splice_function(ZProgramGraph *target, ZProgramGraph *source, const char *function_name, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op) {
  size_t target_count = 0, source_count = 0;
  ZProgramGraphNode *target_fn = body_find_function(target, function_name, &target_count);
  ZProgramGraphNode *source_fn = body_find_function(source, function_name, &source_count);
  if (!target_fn || !source_fn) {
    body_fail(result, op, "GPH004", "replaceFunctionBody function was not found", function_name, "");
    return false;
  }
  ZProgramGraphNode *target_body = body_child(target, target_fn->id, "body");
  ZProgramGraphNode *source_body = body_child(source, source_fn->id, "body");
  if (!target_body || !source_body) {
    body_fail(result, op, "GPH004", "replaceFunctionBody function body was not found", "body Block node", function_name);
    return false;
  }
  char *target_body_id = z_strdup(target_body->id ? target_body->id : "");
  char *source_body_id = z_strdup(source_body->id ? source_body->id : "");
  char *target_path = z_strdup(target_body->path && target_body->path[0] ? target_body->path : "src/main.0");
  bool ok = body_splice_block(target, source, target_body_id, source_body_id, target_path);
  free(target_body_id);
  free(source_body_id);
  free(target_path);
  return ok;
}

static bool body_signature_source(ZProgramGraph *graph, const char *function_name, ZBuf *source, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op) {
  size_t count = 0;
  ZProgramGraphNode *fn = body_find_function(graph, function_name, &count);
  if (!fn) {
    if (count > 1) {
      body_fail(result, op, "GPH003", "replaceFunctionBody function name is ambiguous", function_name, "");
      return false;
    }
    const ZProgramGraphNode *nearest = NULL;
    size_t nearest_distance = 0;
    size_t threshold = (function_name ? strlen(function_name) : 0) / 3 + 2;
    for (size_t i = 0; graph && function_name && i < graph->node_len; i++) {
      const ZProgramGraphNode *node = &graph->nodes[i];
      if (node->kind != Z_PROGRAM_GRAPH_NODE_FUNCTION || !node->name || !node->name[0]) continue;
      size_t distance = z_program_graph_handle_distance(function_name, node->name);
      if (distance > threshold) continue;
      if (!nearest || distance < nearest_distance) {
        nearest = node;
        nearest_distance = distance;
      }
    }
    char message[160];
    if (nearest) snprintf(message, sizeof(message), "replaceFunctionBody function was not found; nearest: %s %s", nearest->name, nearest->id ? nearest->id : "");
    else snprintf(message, sizeof(message), "replaceFunctionBody function was not found");
    body_fail(result, op, "GPH004", message, function_name, "");
    return false;
  }
  if (fn->is_public) zbuf_append(source, "pub ");
  zbuf_append(source, "fn ");
  zbuf_append(source, fn->name ? fn->name : function_name);
  zbuf_append_char(source, '(');
  bool wrote = false;
  for (size_t order = 0; order < graph->edge_len; order++) {
    for (size_t i = 0; i < graph->edge_len; i++) {
      ZProgramGraphEdge *edge = &graph->edges[i];
      if (edge->target != Z_PROGRAM_GRAPH_EDGE_TARGET_NODE || !body_text_eq(edge->from, fn->id) || !body_text_eq(edge->kind, "param") || edge->order != order) continue;
      ZProgramGraphNode *param = body_find_node(graph, edge->to);
      if (!param) continue;
      if (wrote) zbuf_append(source, ", ");
      zbuf_append(source, param->name ? param->name : "arg");
      zbuf_append(source, ": ");
      zbuf_append(source, param->type ? param->type : "Unknown");
      wrote = true;
    }
  }
  zbuf_append(source, ") -> ");
  zbuf_append(source, fn->type && fn->type[0] ? fn->type : "Void");
  if (fn->fallible) zbuf_append(source, " raises");
  zbuf_append(source, " {\n");
  return true;
}

// Parses the patch body rows into a canonical-text Program. The rows are
// tried verbatim first so every statement line printed by `zero view` is
// accepted unchanged (typed `let x: T = expr` bindings, parenthesized calls,
// brace-delimited blocks). When that fails, the legacy space-separated row
// grammar is translated to canonical text and parsed as before.
static bool body_parse_rows_program(const char *signature, const char *rows, const char *parse_fail_message, Program *program, char **source_out, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op) {
  ZBuf raw;
  zbuf_init(&raw);
  zbuf_append(&raw, signature ? signature : "");
  zbuf_append(&raw, rows ? rows : "");
  if (raw.len > 0 && raw.data[raw.len - 1] != '\n') zbuf_append_char(&raw, '\n');
  zbuf_append(&raw, "}\n");
  ZDiag raw_diag = {0};
  if (z_parse_canonical_text_program_source(raw.data ? raw.data : "", program, &raw_diag)) {
    *source_out = raw.data ? raw.data : z_strdup("");
    return true;
  }
  z_free_program(program);
  *program = (Program){0};
  ZBuf translated;
  zbuf_init(&translated);
  zbuf_append(&translated, signature ? signature : "");
  if (!body_append_source_rows(&translated, rows, result, op)) {
    // The rows are not legacy grammar either; the canonical parser diagnostic
    // is the more useful report because `zero view` output is the documented
    // row syntax.
    char *actual = body_diag_physical_row_actual(rows, &raw_diag, raw_diag.message[0] ? raw_diag.message : (rows ? rows : ""));
    body_fail(result, op, "GPH001", parse_fail_message, raw_diag.expected[0] ? raw_diag.expected : "statement rows in zero view syntax", actual);
    free(actual);
    zbuf_free(&raw);
    zbuf_free(&translated);
    return false;
  }
  zbuf_append(&translated, "}\n");
  ZDiag translated_diag = {0};
  if (!z_parse_canonical_text_program_source(translated.data ? translated.data : "", program, &translated_diag)) {
    char *actual = body_diag_row_actual(rows, &translated_diag, translated_diag.message[0] ? translated_diag.message : (translated.data ? translated.data : ""));
    body_fail(result, op, "GPH001", parse_fail_message, translated_diag.expected[0] ? translated_diag.expected : "valid body rows", actual);
    free(actual);
    zbuf_free(&raw);
    zbuf_free(&translated);
    return false;
  }
  zbuf_free(&raw);
  *source_out = translated.data ? translated.data : z_strdup("");
  return true;
}

bool z_program_graph_patch_apply_replace_function_body(ZProgramGraph *graph, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op) {
  const char *function_name = op && op->function && op->function[0] ? op->function : "main";
  if (!body_identifier(function_name)) {
    body_fail(result, op, "GPH003", "replaceFunctionBody function must be a Zero identifier", "identifier", function_name);
    return false;
  }
  ZBuf signature;
  zbuf_init(&signature);
  if (!body_signature_source(graph, function_name, &signature, result, op)) {
    zbuf_free(&signature);
    return false;
  }
  Program program = {0};
  char *source_text = NULL;
  if (!body_parse_rows_program(signature.data, op ? op->value : "", "replaceFunctionBody rows did not parse as a Zero function body", &program, &source_text, result, op)) {
    zbuf_free(&signature);
    return false;
  }
  zbuf_free(&signature);
  SourceInput input = {0};
  input.source_file = z_strdup("src/main.0");
  input.source = z_strdup(source_text ? source_text : "");
  input.canonical_text_source = true;
  ZProgramGraph body_graph = {0};
  bool ok = z_program_graph_from_program(&input, &program, &body_graph);
  if (ok) ok = body_splice_function(graph, &body_graph, function_name, result, op);
  if (!ok && result && !result->message[0]) body_fail(result, op, "GPH006", "replaceFunctionBody could not build ProgramGraph body", "lowerable Zero body rows", source_text ? source_text : "");
  z_program_graph_free(&body_graph);
  z_free_source(&input);
  z_free_program(&program);
  free(source_text);
  if (!ok) return false;
  op->ok = true;
  return true;
}

bool z_program_graph_patch_apply_replace_block_body(ZProgramGraph *graph, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op) {
  const char *block_id = op && op->node && op->node[0] ? op->node : "";
  bool block_ambiguous = false;
  ZProgramGraphNode *target_block = (ZProgramGraphNode *)z_program_graph_resolve_handle(graph, block_id, &block_ambiguous);
  if (!target_block) {
    if (block_ambiguous) {
      body_fail(result, op, "GPH003", "replaceBlockBody block handle is ambiguous", "a unique node id or handle prefix", block_id);
      return false;
    }
    const char *nearest = z_program_graph_nearest_handle(graph, block_id);
    char message[160];
    if (nearest) snprintf(message, sizeof(message), "replaceBlockBody block was not found; nearest: %s", nearest);
    else snprintf(message, sizeof(message), "replaceBlockBody block was not found");
    body_fail(result, op, "GPH004", message, "Block node id", block_id);
    return false;
  }
  if (target_block->kind != Z_PROGRAM_GRAPH_NODE_BLOCK) { body_fail(result, op, "GPH003", "replaceBlockBody target must be a Block node", "Block", z_program_graph_node_kind_name(target_block->kind)); return false; }
  char *target_body_id = z_strdup(target_block->id ? target_block->id : "");
  char *target_path = z_strdup(target_block->path && target_block->path[0] ? target_block->path : "src/main.0");
  Program program = {0};
  char *source_text = NULL;
  if (!body_parse_rows_program("fn __zero_patch_block() -> Void raises {\n", op ? op->value : "", "replaceBlockBody rows did not parse as a Zero block body", &program, &source_text, result, op)) {
    free(target_body_id);
    free(target_path);
    return false;
  }
  SourceInput input = {0};
  input.source_file = z_strdup(target_path);
  input.source = z_strdup(source_text ? source_text : "");
  input.canonical_text_source = true;
  ZProgramGraph body_graph = {0};
  bool ok = z_program_graph_from_program(&input, &program, &body_graph);
  if (ok) {
    ZProgramGraphNode *source_fn = body_find_function(&body_graph, "__zero_patch_block", NULL);
    ZProgramGraphNode *source_body = source_fn ? body_child(&body_graph, source_fn->id, "body") : NULL;
    if (!source_fn || !source_body) { ok = false; body_fail(result, op, "GPH004", "replaceBlockBody source body was not found", "generated Block body", "__zero_patch_block"); }
    else ok = body_splice_block(graph, &body_graph, target_body_id, source_body->id, target_path);
  }
  if (!ok && result && !result->message[0]) body_fail(result, op, "GPH006", "replaceBlockBody could not build ProgramGraph block body", "lowerable Zero body rows", source_text ? source_text : "");
  z_program_graph_free(&body_graph);
  z_free_source(&input);
  z_free_program(&program);
  free(target_body_id);
  free(target_path);
  free(source_text);
  if (!ok) return false;
  op->ok = true;
  return true;
}

// ---- replaceExpr ----

static bool body_node_kind_is_expression(ZProgramGraphNodeKind kind) {
  switch (kind) {
    case Z_PROGRAM_GRAPH_NODE_IDENTIFIER:
    case Z_PROGRAM_GRAPH_NODE_LITERAL:
    case Z_PROGRAM_GRAPH_NODE_FIELD_ACCESS:
    case Z_PROGRAM_GRAPH_NODE_INDEX_ACCESS:
    case Z_PROGRAM_GRAPH_NODE_SLICE:
    case Z_PROGRAM_GRAPH_NODE_CALL:
    case Z_PROGRAM_GRAPH_NODE_METHOD_CALL:
    case Z_PROGRAM_GRAPH_NODE_CAST:
    case Z_PROGRAM_GRAPH_NODE_BORROW:
    case Z_PROGRAM_GRAPH_NODE_CHECK:
    case Z_PROGRAM_GRAPH_NODE_RESCUE:
    case Z_PROGRAM_GRAPH_NODE_META:
    case Z_PROGRAM_GRAPH_NODE_SHAPE_LITERAL:
    case Z_PROGRAM_GRAPH_NODE_ARRAY_LITERAL:
    case Z_PROGRAM_GRAPH_NODE_EXPRESSION:
      return true;
    default:
      return false;
  }
}

/* Statements whose primary expression lives behind an "expr" edge; a
 * replaceExpr aimed at the statement handle replaces that child. */
static bool body_statement_owns_expr(ZProgramGraphNodeKind kind) {
  switch (kind) {
    case Z_PROGRAM_GRAPH_NODE_LET:
    case Z_PROGRAM_GRAPH_NODE_ASSIGNMENT:
    case Z_PROGRAM_GRAPH_NODE_DEFER:
    case Z_PROGRAM_GRAPH_NODE_CHECK:
    case Z_PROGRAM_GRAPH_NODE_RETURN:
    case Z_PROGRAM_GRAPH_NODE_EXPRESSION_STATEMENT:
    case Z_PROGRAM_GRAPH_NODE_IF:
    case Z_PROGRAM_GRAPH_NODE_WHILE:
    case Z_PROGRAM_GRAPH_NODE_FOR:
    case Z_PROGRAM_GRAPH_NODE_MATCH:
      return true;
    default:
      return false;
  }
}

static ZProgramGraphNode *body_expr_child(ZProgramGraph *graph, const char *from) {
  for (size_t i = 0; graph && from && i < graph->edge_len; i++) {
    ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && edge->order == 0 && body_text_eq(edge->from, from) && body_text_eq(edge->kind, "expr")) {
      return body_find_node(graph, edge->to);
    }
  }
  return NULL;
}

/* Builds an expression-only ProgramGraph by parsing `return <expr>` inside a
 * synthetic function; on success *out_root_id names the expression root. */
static bool body_parse_expression_graph(const char *expr_text, ZProgramGraph *out_graph, char **out_root_id, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op) {
  ZBuf source;
  zbuf_init(&source);
  zbuf_append(&source, "fn __zero_patch_expr() -> Void raises {\n    return ");
  zbuf_append(&source, expr_text ? expr_text : "");
  zbuf_append(&source, "\n}\n");
  Program program = {0};
  ZDiag parse_diag = {0};
  if (!z_parse_canonical_text_program_source(source.data ? source.data : "", &program, &parse_diag)) {
    body_fail(result, op, "GPH001", "replaceExpr text did not parse as a Zero expression",
              parse_diag.expected[0] ? parse_diag.expected : "an expression in zero view syntax",
              parse_diag.message[0] ? parse_diag.message : (expr_text ? expr_text : ""));
    z_free_program(&program);
    zbuf_free(&source);
    return false;
  }
  SourceInput input = {0};
  input.source_file = z_strdup("src/main.0");
  input.source = z_strdup(source.data ? source.data : "");
  input.canonical_text_source = true;
  bool ok = z_program_graph_from_program(&input, &program, out_graph);
  char *root_id = NULL;
  if (ok) {
    ZProgramGraphNode *fn = body_find_function(out_graph, "__zero_patch_expr", NULL);
    ZProgramGraphNode *body = fn ? body_child(out_graph, fn->id, "body") : NULL;
    ZProgramGraphNode *ret = NULL;
    for (size_t i = 0; body && i < out_graph->edge_len; i++) {
      ZProgramGraphEdge *edge = &out_graph->edges[i];
      if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && body_text_eq(edge->from, body->id) && body_text_eq(edge->kind, "statement")) {
        ret = body_find_node(out_graph, edge->to);
        break;
      }
    }
    ZProgramGraphNode *root = ret ? body_expr_child(out_graph, ret->id) : NULL;
    if (root) root_id = z_strdup(root->id);
    else ok = false;
  }
  if (!ok) {
    body_fail(result, op, "GPH006", "replaceExpr could not build an expression subtree", "lowerable Zero expression", expr_text ? expr_text : "");
    z_free_source(&input);
    z_free_program(&program);
    zbuf_free(&source);
    free(root_id);
    return false;
  }
  z_free_source(&input);
  z_free_program(&program);
  zbuf_free(&source);
  *out_root_id = root_id;
  return true;
}

/* Copies the source expression subtree into the target graph and returns the
 * new root id (unique-id mapped). */
static char *body_splice_expression(ZProgramGraph *target, ZProgramGraph *source, const char *source_root_id, const char *target_path, int line, int column) {
  bool *copy = z_checked_calloc(source->node_len ? source->node_len : 1, sizeof(bool));
  body_mark_reachable(source, source_root_id, copy);
  BodyIdMap *map = z_checked_calloc(source->node_len ? source->node_len : 1, sizeof(BodyIdMap));
  size_t map_len = 0;
  char *new_root = NULL;
  for (size_t i = 0; i < source->node_len; i++) {
    if (!copy[i]) continue;
    map[map_len].from = source->nodes[i].id;
    map[map_len].to = body_unique_id(target, source->nodes[i].id);
    body_copy_node(target, &source->nodes[i], map[map_len].to, target_path);
    /* keep the replaced node's source position so ordered (let) bindings
     * stay visible to the spliced references */
    target->nodes[target->node_len - 1].line = line > 0 ? line : 1;
    target->nodes[target->node_len - 1].column = column > 0 ? column : 1;
    if (body_text_eq(source->nodes[i].id, source_root_id)) new_root = z_strdup(map[map_len].to);
    map_len++;
  }
  for (size_t i = 0; i < source->edge_len; i++) {
    ZProgramGraphEdge *edge = &source->edges[i];
    if (edge->target != Z_PROGRAM_GRAPH_EDGE_TARGET_NODE) continue;
    if (!body_node_marked(source, copy, edge->from) || !body_node_marked(source, copy, edge->to)) continue;
    body_copy_edge(target, edge, body_mapped_id(map, map_len, edge->from), body_mapped_id(map, map_len, edge->to));
  }
  for (size_t i = 0; i < map_len; i++) free(map[i].to);
  free(map);
  free(copy);
  return new_root;
}

/* Removes the old expression subtree (nodes plus edges touching them). */
static void body_remove_subtree(ZProgramGraph *graph, const char *root_id) {
  bool *remove = z_checked_calloc(graph->node_len ? graph->node_len : 1, sizeof(bool));
  body_mark_reachable(graph, root_id, remove);
  size_t edge_out = 0;
  for (size_t i = 0; i < graph->edge_len; i++) {
    ZProgramGraphEdge edge = graph->edges[i];
    bool drop = body_node_marked(graph, remove, edge.from) ||
                (edge.target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && body_node_marked(graph, remove, edge.to));
    if (drop) body_free_edge(&edge);
    else graph->edges[edge_out++] = edge;
  }
  graph->edge_len = edge_out;
  size_t node_out = 0;
  for (size_t i = 0; i < graph->node_len; i++) {
    if (remove[i]) body_free_node(&graph->nodes[i]);
    else graph->nodes[node_out++] = graph->nodes[i];
  }
  graph->node_len = node_out;
  free(remove);
}

bool z_program_graph_patch_apply_replace_expr(ZProgramGraph *graph, ZProgramGraphPatchResult *result, ZProgramGraphPatchOpResult *op) {
  const char *handle = op && op->node && op->node[0] ? op->node : "";
  bool ambiguous = false;
  ZProgramGraphNode *target = (ZProgramGraphNode *)z_program_graph_resolve_handle(graph, handle, &ambiguous);
  if (!target) {
    if (ambiguous) {
      body_fail(result, op, "GPH003", "replaceExpr handle is ambiguous", "a unique node id or handle prefix", handle);
      return false;
    }
    const char *nearest = z_program_graph_nearest_handle(graph, handle);
    char message[160];
    if (nearest) snprintf(message, sizeof(message), "replaceExpr node was not found; nearest: %s", nearest);
    else snprintf(message, sizeof(message), "replaceExpr node was not found");
    body_fail(result, op, "GPH004", message, "an expression or statement handle; zero view --fn <name> --handles prints them", handle);
    return false;
  }
  if (!body_node_kind_is_expression(target->kind)) {
    if (!body_statement_owns_expr(target->kind)) {
      body_fail(result, op, "GPH003", "replaceExpr target must be an expression or a statement that owns one", "expression node, or Let/Assignment/Check/Return/If/While/For/Match statement", z_program_graph_node_kind_name(target->kind));
      return false;
    }
    ZProgramGraphNode *child = body_expr_child(graph, target->id);
    if (!child) {
      body_fail(result, op, "GPH004", "replaceExpr statement has no expression child", "statement with an expr edge", target->id ? target->id : handle);
      return false;
    }
    target = child;
  }
  body_replace(&op->actual, target->node_hash ? target->node_hash : "");
  if (op->has_expected && !body_text_eq(op->expected, op->actual)) {
    body_fail(result, op, "GPH005", "replaceExpr node hash precondition failed", op->expected, op->actual);
    return false;
  }
  /* the expression must have exactly one owning edge to repoint */
  size_t owner_index = (size_t)-1;
  size_t owner_count = 0;
  for (size_t i = 0; i < graph->edge_len; i++) {
    ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target != Z_PROGRAM_GRAPH_EDGE_TARGET_NODE || !body_text_eq(edge->to, target->id)) continue;
    owner_index = i;
    owner_count++;
  }
  if (owner_count != 1) {
    body_fail(result, op, "GPH003", "replaceExpr target must have exactly one owning edge", "an expression owned by one parent", target->id ? target->id : handle);
    return false;
  }
  char *old_root_id = z_strdup(target->id ? target->id : "");
  char *target_path = z_strdup(target->path && target->path[0] ? target->path : "src/main.0");
  /* Anchor spliced nodes at the enclosing statement's position so ordered
   * (let) bindings declared above stay visible to the new references. */
  int target_line = target->line;
  int target_column = target->column;
  const char *position_cursor = target->id;
  for (size_t depth = 0; position_cursor && depth < graph->node_len; depth++) {
    const ZProgramGraphEdge *owner_edge = NULL;
    for (size_t i = 0; i < graph->edge_len; i++) {
      ZProgramGraphEdge *edge = &graph->edges[i];
      if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && body_text_eq(edge->to, position_cursor)) {
        owner_edge = edge;
        break;
      }
    }
    if (!owner_edge) break;
    ZProgramGraphNode *owner_node = body_find_node(graph, owner_edge->from);
    if (!owner_node) break;
    if (body_text_eq(owner_edge->kind, "statement")) {
      ZProgramGraphNode *statement = body_find_node(graph, position_cursor);
      if (statement && statement->line > target_line) {
        target_line = statement->line;
        target_column = statement->column;
      }
      break;
    }
    if (owner_node->line > target_line) {
      target_line = owner_node->line;
      target_column = owner_node->column;
    }
    if (owner_node->kind == Z_PROGRAM_GRAPH_NODE_FUNCTION || owner_node->kind == Z_PROGRAM_GRAPH_NODE_MODULE) break;
    position_cursor = owner_node->id;
  }
  ZProgramGraph expr_graph = {0};
  char *source_root_id = NULL;
  bool ok = body_parse_expression_graph(op ? op->value : "", &expr_graph, &source_root_id, result, op);
  char *new_root_id = NULL;
  if (ok) {
    new_root_id = body_splice_expression(graph, &expr_graph, source_root_id, target_path, target_line, target_column);
    ok = new_root_id != NULL;
    if (!ok) body_fail(result, op, "GPH006", "replaceExpr could not splice the expression subtree", "lowerable Zero expression", op && op->value ? op->value : "");
  }
  if (ok) {
    ZProgramGraphEdge *owner = &graph->edges[owner_index];
    body_replace(&owner->to, new_root_id);
    body_remove_subtree(graph, old_root_id);
  }
  z_program_graph_free(&expr_graph);
  free(source_root_id);
  free(new_root_id);
  free(old_root_id);
  free(target_path);
  if (!ok) return false;
  op->ok = true;
  return true;
}
