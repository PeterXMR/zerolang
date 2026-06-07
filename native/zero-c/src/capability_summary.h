#ifndef ZERO_CAPABILITY_SUMMARY_H
#define ZERO_CAPABILITY_SUMMARY_H
#include "zero.h"

typedef struct {
  bool args;
  bool env;
  bool fs;
  bool memory;
  bool alloc;
  bool path;
  bool codec;
  bool parse;
  bool time;
  bool rand;
  bool net;
  bool proc;
  bool web;
  bool world;
} CapabilitySummary;

void z_capability_summary_merge(CapabilitySummary *caps, const CapabilitySummary *other);
CapabilitySummary z_ir_program_capabilities(const IrProgram *ir);
#endif
