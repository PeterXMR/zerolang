#ifndef ZERO_C_PROGRAM_GRAPH_SOURCE_MAP_H
#define ZERO_C_PROGRAM_GRAPH_SOURCE_MAP_H

#include "program_graph.h"

void z_program_graph_append_source_map_json(ZBuf *buf, const ZProgramGraph *graph, const char *input_path);
void z_program_graph_append_source_range_json(ZBuf *buf, const ZProgramGraphNode *node, const char *fallback_path);
size_t z_program_graph_source_map_count(const ZProgramGraph *graph);

#endif
