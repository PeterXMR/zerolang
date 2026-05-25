#ifndef ZERO_C_PROGRAM_GRAPH_H
#define ZERO_C_PROGRAM_GRAPH_H

#include "zero.h"

typedef struct {
  char *id;
  const char *kind;
  char *name;
  char *type;
  char *value;
  char *path;
  int line;
  int column;
  bool is_public;
  bool is_mutable;
  bool is_static;
  bool fallible;
} ZProgramGraphNode;

typedef struct {
  char *from;
  char *to;
  const char *kind;
  size_t order;
} ZProgramGraphEdge;

typedef struct {
  unsigned schema_version;
  const char *validation_state;
  const char *id_strategy;
  ZProgramGraphNode *nodes;
  size_t node_len;
  size_t node_cap;
  ZProgramGraphEdge *edges;
  size_t edge_len;
  size_t edge_cap;
  size_t next_id;
} ZProgramGraph;

typedef struct {
  bool ok;
  char code[16];
  char message[160];
  char node_id[64];
  char edge_from[64];
  char edge_to[64];
} ZProgramGraphValidation;

void z_program_graph_init(ZProgramGraph *graph);
void z_program_graph_free(ZProgramGraph *graph);
bool z_program_graph_from_program(const SourceInput *input, const Program *program, ZProgramGraph *graph);
bool z_program_graph_validate(const ZProgramGraph *graph, ZProgramGraphValidation *validation);
void z_program_graph_append_json(ZBuf *buf, const ZProgramGraph *graph, const ZProgramGraphValidation *validation);
void z_append_program_graph_json(ZBuf *buf, const SourceInput *input, const Program *program);

#endif
