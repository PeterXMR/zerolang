#include "program_graph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void graph_append_json_string(ZBuf *buf, const char *text) {
  zbuf_append_char(buf, '"');
  for (const char *p = text ? text : ""; *p; p++) {
    switch (*p) {
      case '\\': zbuf_append(buf, "\\\\"); break;
      case '"': zbuf_append(buf, "\\\""); break;
      case '\n': zbuf_append(buf, "\\n"); break;
      case '\r': zbuf_append(buf, "\\r"); break;
      case '\t': zbuf_append(buf, "\\t"); break;
      default: zbuf_append_char(buf, *p); break;
    }
  }
  zbuf_append_char(buf, '"');
}

static const char *graph_source_path(const SourceInput *input, int parser_line) {
  if (!input || parser_line <= 0) return input && input->source_file ? input->source_file : "";
  size_t index = (size_t)parser_line - 1;
  if (index < input->source_line_count && input->source_line_paths[index]) return input->source_line_paths[index];
  return input->source_file ? input->source_file : "";
}

static int graph_source_line(const SourceInput *input, int parser_line) {
  if (parser_line <= 0) return 0;
  if (input) {
    size_t index = (size_t)parser_line - 1;
    if (index < input->source_line_count && input->source_line_numbers[index] > 0) return input->source_line_numbers[index];
  }
  return parser_line;
}

static const char *graph_module_name_for_path(const SourceInput *input, const char *path) {
  if (!input || input->module_count == 0) return "main";
  if (!path) return input->module_names[0] ? input->module_names[0] : "main";
  for (size_t i = 0; i < input->module_count; i++) {
    if (input->module_paths[i] && strcmp(input->module_paths[i], path) == 0) return input->module_names[i];
  }
  return input->module_names[0] ? input->module_names[0] : "main";
}

static char *graph_next_node_id(ZProgramGraph *graph) {
  char id[32];
  snprintf(id, sizeof(id), "n%06zu", ++graph->next_id);
  return z_strdup(id);
}

void z_program_graph_init(ZProgramGraph *graph) {
  *graph = (ZProgramGraph){
    .schema_version = 1,
    .validation_state = "decoded",
    .id_strategy = "deterministic-traversal-r0",
  };
}

void z_program_graph_free(ZProgramGraph *graph) {
  if (!graph) return;
  for (size_t i = 0; i < graph->node_len; i++) {
    free(graph->nodes[i].id);
    free(graph->nodes[i].name);
    free(graph->nodes[i].type);
    free(graph->nodes[i].value);
    free(graph->nodes[i].path);
  }
  for (size_t i = 0; i < graph->edge_len; i++) {
    free(graph->edges[i].from);
    free(graph->edges[i].to);
  }
  free(graph->nodes);
  free(graph->edges);
  *graph = (ZProgramGraph){0};
}

static ZProgramGraphNode *graph_add_node(ZProgramGraph *graph, const char *kind, const char *name, const char *type, const char *value, const char *path, int line, int column) {
  if (graph->node_len == graph->node_cap) {
    size_t next = z_grow_capacity(graph->node_cap, graph->node_len + 1, 32);
    graph->nodes = z_checked_reallocarray(graph->nodes, next, sizeof(ZProgramGraphNode));
    for (size_t i = graph->node_cap; i < next; i++) graph->nodes[i] = (ZProgramGraphNode){0};
    graph->node_cap = next;
  }
  ZProgramGraphNode *node = &graph->nodes[graph->node_len++];
  node->id = graph_next_node_id(graph);
  node->kind = kind;
  node->name = name ? z_strdup(name) : NULL;
  node->type = type ? z_strdup(type) : NULL;
  node->value = value ? z_strdup(value) : NULL;
  node->path = path ? z_strdup(path) : NULL;
  node->line = line;
  node->column = column;
  return node;
}

static void graph_add_edge(ZProgramGraph *graph, const char *from, const char *to, const char *kind, size_t order) {
  if (!from || !to || !kind) return;
  if (graph->edge_len == graph->edge_cap) {
    size_t next = z_grow_capacity(graph->edge_cap, graph->edge_len + 1, 64);
    graph->edges = z_checked_reallocarray(graph->edges, next, sizeof(ZProgramGraphEdge));
    for (size_t i = graph->edge_cap; i < next; i++) graph->edges[i] = (ZProgramGraphEdge){0};
    graph->edge_cap = next;
  }
  ZProgramGraphEdge *edge = &graph->edges[graph->edge_len++];
  edge->from = z_strdup(from);
  edge->to = z_strdup(to);
  edge->kind = kind;
  edge->order = order;
}

static const char *graph_find_module_id(const ZProgramGraph *graph, const char *module_name) {
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind && strcmp(node->kind, "Module") == 0 && node->name && module_name && strcmp(node->name, module_name) == 0) return node->id;
  }
  return graph && graph->node_len > 0 ? graph->nodes[0].id : NULL;
}

static const char *graph_module_id_for_line(const ZProgramGraph *graph, const SourceInput *input, int parser_line) {
  const char *path = graph_source_path(input, parser_line);
  const char *module = graph_module_name_for_path(input, path);
  return graph_find_module_id(graph, module);
}

static const char *graph_add_type_ref(ZProgramGraph *graph, const char *owner_id, const char *edge_kind, const char *type, const char *path, int line, int column, size_t order) {
  if (!type || !type[0]) return NULL;
  ZProgramGraphNode *node = graph_add_node(graph, "TypeRef", NULL, type, NULL, path, line, column);
  graph_add_edge(graph, owner_id, node->id, edge_kind, order);
  return node->id;
}

static const char *graph_expr_name(const Expr *expr) {
  if (!expr) return NULL;
  if (expr->kind == EXPR_STRING || expr->kind == EXPR_CHAR || expr->kind == EXPR_NUMBER || expr->kind == EXPR_BOOL || expr->kind == EXPR_NULL) return NULL;
  if (expr->kind == EXPR_CALL && expr->left) return graph_expr_name(expr->left);
  if (expr->text) return expr->text;
  return NULL;
}

static const char *graph_expr_value(const Expr *expr) {
  if (!expr) return NULL;
  if (expr->kind == EXPR_BOOL) return expr->bool_value ? "true" : "false";
  if (expr->kind == EXPR_NULL) return "null";
  if (expr->kind == EXPR_STRING || expr->kind == EXPR_CHAR || expr->kind == EXPR_NUMBER) return expr->text;
  return NULL;
}

static const char *graph_expr_kind_name(const Expr *expr) {
  if (!expr) return "Expression";
  switch (expr->kind) {
    case EXPR_IDENT: return "Identifier";
    case EXPR_STRING:
    case EXPR_CHAR:
    case EXPR_NUMBER:
    case EXPR_BOOL:
    case EXPR_NULL: return "Literal";
    case EXPR_MEMBER: return "FieldAccess";
    case EXPR_INDEX: return "IndexAccess";
    case EXPR_SLICE: return "Slice";
    case EXPR_CALL: return expr->left && expr->left->kind == EXPR_MEMBER ? "MethodCall" : "Call";
    case EXPR_BINARY: return "Call";
    case EXPR_CAST: return "Cast";
    case EXPR_BORROW: return "Borrow";
    case EXPR_CHECK: return "Check";
    case EXPR_RESCUE: return "Rescue";
    case EXPR_META: return "Meta";
    case EXPR_SHAPE_LITERAL: return "ShapeLiteral";
    case EXPR_ARRAY_LITERAL: return "ArrayLiteral";
  }
  return "Expression";
}

static const char *graph_build_expr(ZProgramGraph *graph, const SourceInput *input, const Expr *expr);

static void graph_build_expr_edges(ZProgramGraph *graph, const SourceInput *input, const Expr *expr, const char *node_id) {
  if (expr->left) graph_add_edge(graph, node_id, graph_build_expr(graph, input, expr->left), "left", 0);
  if (expr->right) graph_add_edge(graph, node_id, graph_build_expr(graph, input, expr->right), "right", 1);
  for (size_t i = 0; i < expr->args.len; i++) graph_add_edge(graph, node_id, graph_build_expr(graph, input, expr->args.items[i]), "arg", i);
  for (size_t i = 0; i < expr->fields.len; i++) {
    const FieldInit *field = &expr->fields.items[i];
    const char *field_value = graph_build_expr(graph, input, field->value);
    ZProgramGraphNode *field_node = graph_add_node(graph, "FieldInit", field->name, NULL, NULL, graph_source_path(input, field->line), graph_source_line(input, field->line), field->column);
    graph_add_edge(graph, field_node->id, field_value, "value", 0);
    graph_add_edge(graph, node_id, field_node->id, "field", i);
  }
  for (size_t i = 0; i < expr->type_args.len; i++) {
    const TypeArg *arg = &expr->type_args.items[i];
    graph_add_type_ref(graph, node_id, "typeArg", arg->type, graph_source_path(input, arg->line), graph_source_line(input, arg->line), arg->column, i);
  }
}

static const char *graph_build_expr(ZProgramGraph *graph, const SourceInput *input, const Expr *expr) {
  if (!expr) return NULL;
  const char *path = graph_source_path(input, expr->line);
  ZProgramGraphNode *node = graph_add_node(graph, graph_expr_kind_name(expr), graph_expr_name(expr), expr->resolved_type, graph_expr_value(expr), path, graph_source_line(input, expr->line), expr->column);
  const char *node_id = node->id;
  if (expr->kind == EXPR_BORROW) node->is_mutable = expr->mutable_borrow;
  graph_build_expr_edges(graph, input, expr, node_id);
  return node_id;
}

static const char *graph_stmt_kind_name(const Stmt *stmt) {
  if (!stmt) return "Statement";
  switch (stmt->kind) {
    case STMT_LET: return "Let";
    case STMT_ASSIGN: return "Assignment";
    case STMT_DEFER: return "Defer";
    case STMT_CHECK: return "Check";
    case STMT_RETURN: return "Return";
    case STMT_EXPR: return "ExpressionStatement";
    case STMT_IF: return "If";
    case STMT_WHILE: return "While";
    case STMT_FOR: return "For";
    case STMT_BREAK: return "Break";
    case STMT_CONTINUE: return "Continue";
    case STMT_MATCH: return "Match";
    case STMT_RAISE: return "Raise";
  }
  return "Statement";
}

static const char *graph_build_block(ZProgramGraph *graph, const SourceInput *input, const StmtVec *body, const char *name, const char *path, int line, int column);

static void graph_build_match_arms(ZProgramGraph *graph, const SourceInput *input, const Stmt *stmt, const char *stmt_id) {
  for (size_t i = 0; i < stmt->match_arms.len; i++) {
    const MatchArm *arm = &stmt->match_arms.items[i];
    const char *path = graph_source_path(input, arm->line);
    ZProgramGraphNode *arm_node = graph_add_node(graph, "MatchArm", arm->case_name, NULL, arm->payload_name, path, graph_source_line(input, arm->line), arm->column);
    const char *arm_id = arm_node->id;
    graph_add_edge(graph, stmt_id, arm_id, "arm", i);
    if (arm->guard) graph_add_edge(graph, arm_id, graph_build_expr(graph, input, arm->guard), "guard", 0);
    const char *body = graph_build_block(graph, input, &arm->body, "body", path, arm->line, arm->column);
    graph_add_edge(graph, arm_id, body, "body", 0);
  }
}

static const char *graph_build_stmt(ZProgramGraph *graph, const SourceInput *input, const Stmt *stmt) {
  const char *path = graph_source_path(input, stmt ? stmt->line : 0);
  ZProgramGraphNode *node = graph_add_node(graph, graph_stmt_kind_name(stmt), stmt ? stmt->name : NULL, stmt ? (stmt->type ? stmt->type : stmt->resolved_type) : NULL, NULL, path, graph_source_line(input, stmt ? stmt->line : 0), stmt ? stmt->column : 0);
  const char *node_id = node->id;
  if (!stmt) return node_id;
  node->is_mutable = stmt->mutable_binding;
  if (stmt->target) graph_add_edge(graph, node_id, graph_build_expr(graph, input, stmt->target), "target", 0);
  if (stmt->expr) graph_add_edge(graph, node_id, graph_build_expr(graph, input, stmt->expr), "expr", 0);
  if (stmt->range_end) graph_add_edge(graph, node_id, graph_build_expr(graph, input, stmt->range_end), "rangeEnd", 1);
  if (stmt->type) graph_add_type_ref(graph, node_id, "declaredType", stmt->type, path, graph_source_line(input, stmt->line), stmt->column, 0);
  if (stmt->then_body.len > 0) graph_add_edge(graph, node_id, graph_build_block(graph, input, &stmt->then_body, "then", path, stmt->line, stmt->column), "then", 0);
  if (stmt->else_body.len > 0) graph_add_edge(graph, node_id, graph_build_block(graph, input, &stmt->else_body, "else", path, stmt->line, stmt->column), "else", 1);
  graph_build_match_arms(graph, input, stmt, node_id);
  return node_id;
}

static const char *graph_build_block(ZProgramGraph *graph, const SourceInput *input, const StmtVec *body, const char *name, const char *path, int line, int column) {
  ZProgramGraphNode *node = graph_add_node(graph, "Block", name, NULL, NULL, path, graph_source_line(input, line), column);
  const char *node_id = node->id;
  for (size_t i = 0; body && i < body->len; i++) graph_add_edge(graph, node_id, graph_build_stmt(graph, input, body->items[i]), "statement", i);
  return node_id;
}

static void graph_build_params(ZProgramGraph *graph, const SourceInput *input, const ParamVec *params, const char *owner_id, const char *edge_kind) {
  for (size_t i = 0; params && i < params->len; i++) {
    const Param *param = &params->items[i];
    const char *path = graph_source_path(input, param->line);
    ZProgramGraphNode *node = graph_add_node(graph, "Param", param->name, param->type, NULL, path, graph_source_line(input, param->line), param->column);
    const char *node_id = node->id;
    node->is_static = param->is_static;
    graph_add_edge(graph, owner_id, node_id, edge_kind, i);
    if (param->type) graph_add_type_ref(graph, node_id, "type", param->type, path, graph_source_line(input, param->line), param->column, 0);
    if (param->default_value) graph_add_edge(graph, node_id, graph_build_expr(graph, input, param->default_value), "default", 0);
  }
}

static void graph_build_function(ZProgramGraph *graph, const SourceInput *input, const Function *fun, const char *owner_id, const char *edge_kind, size_t order) {
  const char *path = graph_source_path(input, fun->line);
  ZProgramGraphNode *node = graph_add_node(graph, "Function", fun->name, fun->return_type, fun->test_name, path, graph_source_line(input, fun->line), fun->column);
  const char *node_id = node->id;
  node->is_public = fun->is_public;
  node->fallible = fun->raises;
  graph_add_edge(graph, owner_id, node_id, edge_kind, order);
  graph_build_params(graph, input, &fun->type_params, node_id, "typeParam");
  graph_build_params(graph, input, &fun->params, node_id, "param");
  graph_add_type_ref(graph, node_id, "returnType", fun->return_type, path, graph_source_line(input, fun->line), fun->column, 0);
  if (fun->raises) graph_add_edge(graph, node_id, graph_add_node(graph, "EffectRef", "error", NULL, NULL, path, graph_source_line(input, fun->line), fun->column)->id, "effect", 0);
  for (size_t i = 0; i < fun->errors.len; i++) {
    const Param *error = &fun->errors.items[i];
    ZProgramGraphNode *error_node = graph_add_node(graph, "ErrorVariant", error->name, error->type, NULL, graph_source_path(input, error->line), graph_source_line(input, error->line), error->column);
    graph_add_edge(graph, node_id, error_node->id, "error", i);
  }
  graph_add_edge(graph, node_id, graph_build_block(graph, input, &fun->body, "body", path, fun->line, fun->column), "body", 0);
}

static void graph_build_top_level_params(ZProgramGraph *graph, const SourceInput *input, const ParamVec *items, const char *owner_id, const char *edge_kind, const char *node_kind) {
  for (size_t i = 0; items && i < items->len; i++) {
    const Param *item = &items->items[i];
    ZProgramGraphNode *node = graph_add_node(graph, node_kind, item->name, item->type, NULL, graph_source_path(input, item->line), graph_source_line(input, item->line), item->column);
    const char *node_id = node->id;
    graph_add_edge(graph, owner_id, node_id, edge_kind, i);
    if (item->type) graph_add_type_ref(graph, node_id, "type", item->type, graph_source_path(input, item->line), graph_source_line(input, item->line), item->column, 0);
  }
}

static void graph_build_modules(ZProgramGraph *graph, const SourceInput *input) {
  if (input && input->module_count > 0) {
    for (size_t i = 0; i < input->module_count; i++) {
      graph_add_node(graph, "Module", input->module_names[i], NULL, NULL, input->module_paths[i], 1, 1);
    }
    return;
  }
  graph_add_node(graph, "Module", "main", NULL, NULL, input ? input->source_file : "", 1, 1);
}

static void graph_build_imports(ZProgramGraph *graph, const SourceInput *input, const Program *program) {
  for (size_t i = 0; program && i < program->use_imports.len; i++) {
    const UseImport *item = &program->use_imports.items[i];
    const char *path = graph_source_path(input, item->line);
    ZProgramGraphNode *node = graph_add_node(graph, "Import", item->module, NULL, item->alias, path, graph_source_line(input, item->line), item->column);
    graph_add_edge(graph, graph_module_id_for_line(graph, input, item->line), node->id, "import", i);
  }
}

static void graph_build_consts_and_aliases(ZProgramGraph *graph, const SourceInput *input, const Program *program) {
  for (size_t i = 0; program && i < program->consts.len; i++) {
    const ConstDecl *item = &program->consts.items[i];
    ZProgramGraphNode *node = graph_add_node(graph, "Const", item->name, item->type, NULL, graph_source_path(input, item->line), graph_source_line(input, item->line), item->column);
    const char *node_id = node->id;
    node->is_public = item->is_public;
    graph_add_edge(graph, graph_module_id_for_line(graph, input, item->line), node_id, "const", i);
    if (item->expr) graph_add_edge(graph, node_id, graph_build_expr(graph, input, item->expr), "value", 0);
  }
  for (size_t i = 0; program && i < program->aliases.len; i++) {
    const TypeAlias *item = &program->aliases.items[i];
    ZProgramGraphNode *node = graph_add_node(graph, "TypeAlias", item->name, item->target, NULL, graph_source_path(input, item->line), graph_source_line(input, item->line), item->column);
    const char *node_id = node->id;
    node->is_public = item->is_public;
    graph_add_edge(graph, graph_module_id_for_line(graph, input, item->line), node_id, "alias", i);
    graph_add_type_ref(graph, node_id, "target", item->target, graph_source_path(input, item->line), graph_source_line(input, item->line), item->column, 0);
  }
}

static void graph_build_aggregate_decls(ZProgramGraph *graph, const SourceInput *input, const Program *program) {
  for (size_t i = 0; program && i < program->shapes.len; i++) {
    const Shape *shape = &program->shapes.items[i];
    ZProgramGraphNode *node = graph_add_node(graph, "Shape", shape->name, NULL, shape->layout, graph_source_path(input, shape->line), graph_source_line(input, shape->line), shape->column);
    const char *node_id = node->id;
    node->is_public = shape->is_public;
    graph_add_edge(graph, graph_module_id_for_line(graph, input, shape->line), node_id, "shape", i);
    graph_build_params(graph, input, &shape->type_params, node_id, "typeParam");
    graph_build_top_level_params(graph, input, &shape->fields, node_id, "field", "Field");
    for (size_t method = 0; method < shape->methods.len; method++) graph_build_function(graph, input, &shape->methods.items[method], node_id, "method", method);
  }
  for (size_t i = 0; program && i < program->interfaces.len; i++) {
    const InterfaceDecl *item = &program->interfaces.items[i];
    ZProgramGraphNode *node = graph_add_node(graph, "Interface", item->name, NULL, NULL, graph_source_path(input, item->line), graph_source_line(input, item->line), item->column);
    const char *node_id = node->id;
    node->is_public = item->is_public;
    graph_add_edge(graph, graph_module_id_for_line(graph, input, item->line), node_id, "interface", i);
    graph_build_params(graph, input, &item->type_params, node_id, "typeParam");
    for (size_t method = 0; method < item->methods.len; method++) graph_build_function(graph, input, &item->methods.items[method], node_id, "method", method);
  }
}

static void graph_build_sum_decls(ZProgramGraph *graph, const SourceInput *input, const Program *program) {
  for (size_t i = 0; program && i < program->enums.len; i++) {
    const EnumDecl *item = &program->enums.items[i];
    ZProgramGraphNode *node = graph_add_node(graph, "Enum", item->name, item->type, NULL, graph_source_path(input, item->line), graph_source_line(input, item->line), item->column);
    const char *node_id = node->id;
    graph_add_edge(graph, graph_module_id_for_line(graph, input, item->line), node_id, "enum", i);
    graph_build_top_level_params(graph, input, &item->cases, node_id, "case", "EnumCase");
  }
  for (size_t i = 0; program && i < program->choices.len; i++) {
    const Choice *item = &program->choices.items[i];
    ZProgramGraphNode *node = graph_add_node(graph, "Choice", item->name, NULL, NULL, graph_source_path(input, item->line), graph_source_line(input, item->line), item->column);
    const char *node_id = node->id;
    graph_add_edge(graph, graph_module_id_for_line(graph, input, item->line), node_id, "choice", i);
    graph_build_top_level_params(graph, input, &item->cases, node_id, "case", "ChoiceCase");
  }
}

bool z_program_graph_from_program(const SourceInput *input, const Program *program, ZProgramGraph *graph) {
  if (!graph) return false;
  z_program_graph_init(graph);
  graph_build_modules(graph, input);
  graph_build_imports(graph, input, program);
  graph_build_consts_and_aliases(graph, input, program);
  graph_build_aggregate_decls(graph, input, program);
  graph_build_sum_decls(graph, input, program);
  for (size_t i = 0; program && i < program->functions.len; i++) {
    const Function *fun = &program->functions.items[i];
    graph_build_function(graph, input, fun, graph_module_id_for_line(graph, input, fun->line), "function", i);
  }
  return true;
}

static bool graph_has_node_id(const ZProgramGraph *graph, const char *id) {
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    if (graph->nodes[i].id && id && strcmp(graph->nodes[i].id, id) == 0) return true;
  }
  return false;
}

static bool graph_validation_fail(ZProgramGraphValidation *validation, const char *code, const char *message, const char *node_id, const char *edge_from, const char *edge_to) {
  if (validation) {
    validation->ok = false;
    snprintf(validation->code, sizeof(validation->code), "%s", code ? code : "GRF000");
    snprintf(validation->message, sizeof(validation->message), "%s", message ? message : "program graph validation failed");
    snprintf(validation->node_id, sizeof(validation->node_id), "%s", node_id ? node_id : "");
    snprintf(validation->edge_from, sizeof(validation->edge_from), "%s", edge_from ? edge_from : "");
    snprintf(validation->edge_to, sizeof(validation->edge_to), "%s", edge_to ? edge_to : "");
  }
  return false;
}

bool z_program_graph_validate(const ZProgramGraph *graph, ZProgramGraphValidation *validation) {
  if (validation) *validation = (ZProgramGraphValidation){.ok = true};
  if (!graph) return graph_validation_fail(validation, "GRF001", "program graph is missing", NULL, NULL, NULL);
  for (size_t i = 0; i < graph->node_len; i++) {
    if (!graph->nodes[i].id || !graph->nodes[i].kind) return graph_validation_fail(validation, "GRF002", "node is missing required identity", graph->nodes[i].id, NULL, NULL);
    for (size_t j = i + 1; j < graph->node_len; j++) {
      if (graph->nodes[j].id && strcmp(graph->nodes[i].id, graph->nodes[j].id) == 0) return graph_validation_fail(validation, "GRF003", "duplicate node id", graph->nodes[i].id, NULL, NULL);
    }
  }
  for (size_t i = 0; i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (!graph_has_node_id(graph, edge->from)) return graph_validation_fail(validation, "GRF004", "edge source is missing", NULL, edge->from, edge->to);
    if (!graph_has_node_id(graph, edge->to)) return graph_validation_fail(validation, "GRF005", "edge target is missing", NULL, edge->from, edge->to);
    for (size_t j = i + 1; j < graph->edge_len; j++) {
      const ZProgramGraphEdge *other = &graph->edges[j];
      if (edge->from && other->from && edge->kind && other->kind &&
          strcmp(edge->from, other->from) == 0 && strcmp(edge->kind, other->kind) == 0 && edge->order == other->order) {
        return graph_validation_fail(validation, "GRF006", "duplicate ordered child edge", NULL, edge->from, edge->to);
      }
    }
  }
  return true;
}

static void graph_append_validation_json(ZBuf *buf, const ZProgramGraphValidation *validation) {
  bool ok = !validation || validation->ok;
  zbuf_appendf(buf, "{\"state\":%s,\"ok\":%s,\"diagnostics\":[", "\"shape-valid\"", ok ? "true" : "false");
  if (!ok) {
    zbuf_append(buf, "{\"code\":");
    graph_append_json_string(buf, validation->code);
    zbuf_append(buf, ",\"message\":");
    graph_append_json_string(buf, validation->message);
    zbuf_append(buf, ",\"node\":");
    graph_append_json_string(buf, validation->node_id);
    zbuf_append(buf, ",\"edge\":{\"from\":");
    graph_append_json_string(buf, validation->edge_from);
    zbuf_append(buf, ",\"to\":");
    graph_append_json_string(buf, validation->edge_to);
    zbuf_append(buf, "}}");
  }
  zbuf_append(buf, "]}");
}

void z_program_graph_append_json(ZBuf *buf, const ZProgramGraph *graph, const ZProgramGraphValidation *validation) {
  zbuf_appendf(buf, "{\"schemaVersion\":%u,\"canonicalSource\":false,\"idStrategy\":", graph ? graph->schema_version : 1);
  graph_append_json_string(buf, graph ? graph->id_strategy : "deterministic-traversal-r0");
  zbuf_append(buf, ",\"validation\":");
  graph_append_validation_json(buf, validation);
  zbuf_appendf(buf, ",\"counts\":{\"nodes\":%zu,\"edges\":%zu},\"nodes\":[", graph ? graph->node_len : 0, graph ? graph->edge_len : 0);
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (i > 0) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"id\":");
    graph_append_json_string(buf, node->id);
    zbuf_append(buf, ",\"kind\":");
    graph_append_json_string(buf, node->kind);
    zbuf_append(buf, ",\"name\":");
    graph_append_json_string(buf, node->name);
    zbuf_append(buf, ",\"type\":");
    graph_append_json_string(buf, node->type);
    zbuf_append(buf, ",\"value\":");
    graph_append_json_string(buf, node->value);
    zbuf_append(buf, ",\"path\":");
    graph_append_json_string(buf, node->path);
    zbuf_appendf(buf, ",\"line\":%d,\"column\":%d,\"public\":%s,\"mutable\":%s,\"static\":%s,\"fallible\":%s}",
                 node->line, node->column, node->is_public ? "true" : "false", node->is_mutable ? "true" : "false", node->is_static ? "true" : "false", node->fallible ? "true" : "false");
  }
  zbuf_append(buf, "],\"edges\":[");
  for (size_t i = 0; graph && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (i > 0) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"from\":");
    graph_append_json_string(buf, edge->from);
    zbuf_append(buf, ",\"to\":");
    graph_append_json_string(buf, edge->to);
    zbuf_append(buf, ",\"kind\":");
    graph_append_json_string(buf, edge->kind);
    zbuf_appendf(buf, ",\"order\":%zu}", edge->order);
  }
  zbuf_append(buf, "]}");
}

void z_append_program_graph_json(ZBuf *buf, const SourceInput *input, const Program *program) {
  ZProgramGraph graph;
  if (!z_program_graph_from_program(input, program, &graph)) {
    zbuf_append(buf, "{\"schemaVersion\":1,\"canonicalSource\":false,\"validation\":{\"state\":\"decoded\",\"ok\":false,\"diagnostics\":[{\"code\":\"GRF001\",\"message\":\"program graph construction failed\"}]},\"counts\":{\"nodes\":0,\"edges\":0},\"nodes\":[],\"edges\":[]}");
    return;
  }
  ZProgramGraphValidation validation = {0};
  z_program_graph_validate(&graph, &validation);
  z_program_graph_append_json(buf, &graph, &validation);
  z_program_graph_free(&graph);
}
