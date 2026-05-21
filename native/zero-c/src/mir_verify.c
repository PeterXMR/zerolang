#include "mir_verify.h"

#include <stdio.h>

static const char *mir_type_kind_name(IrTypeKind type) {
  switch (type) {
    case IR_TYPE_VOID: return "Void";
    case IR_TYPE_BOOL: return "Bool";
    case IR_TYPE_U8: return "u8";
    case IR_TYPE_U16: return "u16";
    case IR_TYPE_USIZE: return "usize";
    case IR_TYPE_I32: return "i32";
    case IR_TYPE_U32: return "u32";
    case IR_TYPE_I64: return "i64";
    case IR_TYPE_U64: return "u64";
    case IR_TYPE_BYTE_VIEW: return "ByteView";
    case IR_TYPE_ALLOC: return "Alloc";
    case IR_TYPE_VEC: return "Vec";
    case IR_TYPE_MAYBE_BYTE_VIEW: return "Maybe<ByteView>";
    case IR_TYPE_MAYBE_SCALAR: return "Maybe<scalar>";
    case IR_TYPE_RECORD: return "record";
    case IR_TYPE_UNSUPPORTED:
    default:
      return "unsupported";
  }
}

static bool mir_type_is_value(IrTypeKind type) {
  return type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_USIZE || type == IR_TYPE_I32 || type == IR_TYPE_U32 || type == IR_TYPE_I64 || type == IR_TYPE_U64;
}

static bool mir_type_is_direct_abi(IrTypeKind type) {
  return type == IR_TYPE_BOOL || mir_type_is_value(type);
}

static bool mir_type_is_direct_fallible_value(IrTypeKind type) {
  return type == IR_TYPE_VOID || type == IR_TYPE_BOOL || type == IR_TYPE_U8 ||
         type == IR_TYPE_U16 || type == IR_TYPE_USIZE || type == IR_TYPE_I32 ||
         type == IR_TYPE_U32;
}

static bool mir_type_is_integer_value(IrTypeKind type) {
  return type == IR_TYPE_U8 || type == IR_TYPE_U16 || type == IR_TYPE_USIZE ||
         type == IR_TYPE_I32 || type == IR_TYPE_U32 || type == IR_TYPE_I64 ||
         type == IR_TYPE_U64;
}

static unsigned mir_type_byte_size(IrTypeKind type) {
  switch (type) {
    case IR_TYPE_BOOL:
    case IR_TYPE_U8: return 1;
    case IR_TYPE_U16: return 2;
    case IR_TYPE_I32:
    case IR_TYPE_USIZE:
    case IR_TYPE_U32: return 4;
    case IR_TYPE_I64:
    case IR_TYPE_U64: return 8;
    default: return 0;
  }
}

static void mir_verify_mark_unsupported(IrProgram *ir, const char *message, int line, int column, const char *actual) {
  if (!ir || !ir->mir_valid) return;
  ir->mir_valid = false;
  ir->mir_line = line > 0 ? line : 1;
  ir->mir_column = column > 0 ? column : 1;
  snprintf(ir->mir_message, sizeof(ir->mir_message), "%s", message ? message : "MIR verification failed");
  snprintf(ir->mir_expected, sizeof(ir->mir_expected), "direct backend MIR contract");
  snprintf(ir->mir_actual, sizeof(ir->mir_actual), "%s", actual ? actual : "invalid MIR");
  snprintf(ir->mir_help, sizeof(ir->mir_help), "report this compiler bug with the source program that produced it");
  z_backend_blocker_set(&ir->backend_blocker, NULL, NULL, NULL, "lower", ir->mir_actual);
}

static bool mir_verify_local_index(IrProgram *ir, const IrFunction *fun, unsigned index, int line, int column, const char *message) {
  if (!ir || !ir->mir_valid || !fun) return false;
  if (index < fun->local_len) return true;
  char actual[160];
  snprintf(actual, sizeof(actual), "local index %u with %zu local(s) on %s", index, fun->local_len, fun->name ? fun->name : "<unnamed>");
  mir_verify_mark_unsupported(ir, message, line, column, actual);
  return false;
}

static bool mir_value_can_initialize_local(const IrLocal *local, const IrValue *value) {
  if (!local || !value) return false;
  if (value->type == local->type) return true;
  return local->type == IR_TYPE_MAYBE_SCALAR && value->type == IR_TYPE_I64;
}

typedef struct {
  size_t count;
  int line;
  int column;
  const char *reason;
} MirCountRequirement;

typedef struct {
  MirCountRequirement allocator_helpers;
  MirCountRequirement buffer_helpers;
  MirCountRequirement runtime_helpers;
  MirCountRequirement host_runtime_imports;
  MirCountRequirement http_runtime_imports;
} MirHelperRequirements;

static void mir_require_count(MirCountRequirement *req, size_t count, int line, int column, const char *reason) {
  if (!req || count <= req->count) return;
  req->count = count;
  req->line = line;
  req->column = column;
  req->reason = reason;
}

static bool mir_verify_direct_function_contract(IrProgram *ir, const IrFunction *fun) {
  if (!ir || !ir->mir_valid || !fun) return false;
  if (fun->param_count > fun->local_len) {
    char actual[128];
    snprintf(actual, sizeof(actual), "%zu parameter(s) with %zu local(s)", fun->param_count, fun->local_len);
    mir_verify_mark_unsupported(ir, "MIR verifier found parameter count outside the local table", fun->line, fun->column, actual);
    return false;
  }
  for (size_t i = 0; i < fun->param_count; i++) {
    const IrLocal *local = &fun->locals[i];
    if (!local->is_param || local->index != i) {
      char actual[160];
      snprintf(actual, sizeof(actual), "parameter slot %zu maps to local index %u", i, local->index);
      mir_verify_mark_unsupported(ir, "MIR verifier found invalid parameter local layout", local->line, local->column, actual);
      return false;
    }
    if (!mir_type_is_direct_abi(local->type)) {
      char actual[160];
      snprintf(actual, sizeof(actual), "parameter %s has %s", local->name ? local->name : "<unnamed>", mir_type_kind_name(local->type));
      mir_verify_mark_unsupported(ir, "MIR verifier found non-ABI parameter type", local->line, local->column, actual);
      return false;
    }
  }
  for (size_t i = 0; i < fun->local_len; i++) {
    const IrLocal *local = &fun->locals[i];
    if (local->index != i) {
      char actual[160];
      snprintf(actual, sizeof(actual), "local %s has index %u at slot %zu", local->name ? local->name : "<unnamed>", local->index, i);
      mir_verify_mark_unsupported(ir, "MIR verifier found local table index mismatch", local->line, local->column, actual);
      return false;
    }
    if (local->byte_size == 0 || local->alignment == 0) {
      char actual[160];
      snprintf(actual, sizeof(actual), "local %s has size %u alignment %u", local->name ? local->name : "<unnamed>", local->byte_size, local->alignment);
      mir_verify_mark_unsupported(ir, "MIR verifier found invalid local storage layout", local->line, local->column, actual);
      return false;
    }
    if (local->frame_offset < local->byte_size || local->frame_offset > fun->frame_bytes) {
      char actual[192];
      snprintf(actual, sizeof(actual), "local %s offset %u size %u frame %zu", local->name ? local->name : "<unnamed>", local->frame_offset, local->byte_size, fun->frame_bytes);
      mir_verify_mark_unsupported(ir, "MIR verifier found local outside the stack frame", local->line, local->column, actual);
      return false;
    }
    unsigned frame_start = local->frame_offset - local->byte_size;
    if (frame_start % local->alignment != 0) {
      char actual[192];
      snprintf(actual, sizeof(actual), "local %s starts at %u with alignment %u", local->name ? local->name : "<unnamed>", frame_start, local->alignment);
      mir_verify_mark_unsupported(ir, "MIR verifier found misaligned local storage", local->line, local->column, actual);
      return false;
    }
    if (i > 0 && frame_start < fun->locals[i - 1].frame_offset) {
      char actual[192];
      snprintf(actual, sizeof(actual), "local %s starts at %u before previous local ends at %u", local->name ? local->name : "<unnamed>", frame_start, fun->locals[i - 1].frame_offset);
      mir_verify_mark_unsupported(ir, "MIR verifier found overlapping local storage", local->line, local->column, actual);
      return false;
    }
  }
  if (fun->raises) {
    if (fun->return_type != IR_TYPE_I64 || !mir_type_is_direct_fallible_value(fun->value_return_type)) {
      char actual[160];
      snprintf(actual, sizeof(actual), "fallible return %s value %s", mir_type_kind_name(fun->return_type), mir_type_kind_name(fun->value_return_type));
      mir_verify_mark_unsupported(ir, "MIR verifier found invalid fallible return representation", fun->line, fun->column, actual);
      return false;
    }
  } else if (fun->return_type != IR_TYPE_VOID && !mir_type_is_direct_abi(fun->return_type)) {
    char actual[128];
    snprintf(actual, sizeof(actual), "return %s", mir_type_kind_name(fun->return_type));
    mir_verify_mark_unsupported(ir, "MIR verifier found non-ABI return type", fun->line, fun->column, actual);
    return false;
  }
  return true;
}

static bool mir_verify_direct_call_contract(IrProgram *ir, const IrValue *value) {
  if (!ir || !ir->mir_valid || !value || value->kind != IR_VALUE_CALL) return ir && ir->mir_valid;
  if (value->callee_index >= ir->function_len) {
    char actual[128];
    snprintf(actual, sizeof(actual), "callee index %u with %zu MIR function(s)", value->callee_index, ir->function_len);
    mir_verify_mark_unsupported(ir, "MIR verifier found direct call target outside the function table", value->line, value->column, actual);
    return false;
  }
  const IrFunction *callee = &ir->functions[value->callee_index];
  if (value->arg_len != callee->param_count) {
    char actual[160];
    snprintf(actual, sizeof(actual), "%zu argument(s) for %zu parameter(s) on %s", value->arg_len, callee->param_count, callee->name ? callee->name : "<unnamed>");
    mir_verify_mark_unsupported(ir, "MIR verifier found direct call arity mismatch", value->line, value->column, actual);
    return false;
  }
  IrTypeKind expected_call_type = callee->raises ? IR_TYPE_I64 : callee->return_type;
  if (value->type != expected_call_type || value->element_type != callee->value_return_type) {
    char actual[192];
    snprintf(actual, sizeof(actual), "call returns %s value %s but callee returns %s value %s", mir_type_kind_name(value->type), mir_type_kind_name(value->element_type), mir_type_kind_name(expected_call_type), mir_type_kind_name(callee->value_return_type));
    mir_verify_mark_unsupported(ir, "MIR verifier found direct call return mismatch", value->line, value->column, actual);
    return false;
  }
  for (size_t i = 0; i < value->arg_len; i++) {
    const IrValue *arg = value->args[i];
    const IrLocal *param = &callee->locals[i];
    if (!arg) {
      char actual[128];
      snprintf(actual, sizeof(actual), "missing argument %zu for %s", i, callee->name ? callee->name : "<unnamed>");
      mir_verify_mark_unsupported(ir, "MIR verifier found null direct call argument", value->line, value->column, actual);
      return false;
    }
    if (!param->is_param || arg->type != param->type) {
      char actual[192];
      snprintf(actual, sizeof(actual), "argument %zu has %s but parameter has %s", i, mir_type_kind_name(arg->type), mir_type_kind_name(param->type));
      mir_verify_mark_unsupported(ir, "MIR verifier found direct call argument ABI mismatch", arg->line, arg->column, actual);
      return false;
    }
  }
  return true;
}

static bool mir_verify_value_type(IrProgram *ir, const IrValue *value, IrTypeKind expected, const char *message, const char *role) {
  if (!ir || !ir->mir_valid) return false;
  if (value && value->type == expected) return true;
  char actual[160];
  snprintf(actual, sizeof(actual), "%s is %s but expected %s", role ? role : "value", value ? mir_type_kind_name(value->type) : "missing", mir_type_kind_name(expected));
  mir_verify_mark_unsupported(ir, message, value ? value->line : 1, value ? value->column : 1, actual);
  return false;
}

static bool mir_verify_helper_result_type(IrProgram *ir, const IrValue *value, IrTypeKind expected, const char *role) {
  return mir_verify_value_type(ir, value, expected, "MIR verifier found helper result type mismatch", role);
}

static bool mir_verify_value_is_integer(IrProgram *ir, const IrValue *value, const char *message, const char *role) {
  if (!ir || !ir->mir_valid) return false;
  if (value && mir_type_is_integer_value(value->type)) return true;
  char actual[160];
  snprintf(actual, sizeof(actual), "%s is %s", role ? role : "value", value ? mir_type_kind_name(value->type) : "missing");
  mir_verify_mark_unsupported(ir, message, value ? value->line : 1, value ? value->column : 1, actual);
  return false;
}

static bool mir_verify_local_value_kind(IrProgram *ir, const IrFunction *fun, unsigned index, IrTypeKind expected, int line, int column, const char *message, const char *role) {
  if (!mir_verify_local_index(ir, fun, index, line, column, message)) return false;
  const IrLocal *local = &fun->locals[index];
  if (local->type == expected) return true;
  char actual[192];
  snprintf(actual, sizeof(actual), "%s local %s has %s but expected %s", role ? role : "helper", local->name ? local->name : "<unnamed>", mir_type_kind_name(local->type), mir_type_kind_name(expected));
  mir_verify_mark_unsupported(ir, message, line, column, actual);
  return false;
}

static bool mir_verify_record_field_span(IrProgram *ir, const IrLocal *local, unsigned field_offset, IrTypeKind type, int line, int column, const char *message) {
  if (!ir || !ir->mir_valid || !local) return false;
  unsigned byte_size = mir_type_byte_size(type);
  if (byte_size > 0 && field_offset <= local->byte_size && byte_size <= local->byte_size - field_offset) return true;
  char actual[192];
  snprintf(actual, sizeof(actual), "field offset %u width %u in local size %u", field_offset, byte_size, local->byte_size);
  mir_verify_mark_unsupported(ir, message, line, column, actual);
  return false;
}

static bool mir_verify_direct_helper_value_contract(IrProgram *ir, const IrFunction *fun, const IrValue *value, MirHelperRequirements *requirements) {
  if (!ir || !ir->mir_valid || !value) return ir && ir->mir_valid;
  switch (value->kind) {
    case IR_VALUE_FIXED_BUF_ALLOC:
      mir_require_count(&requirements->allocator_helpers, 1, value->line, value->column, "std.mem.fixedBufAlloc");
      if (!mir_verify_helper_result_type(ir, value, IR_TYPE_ALLOC, "FixedBufAlloc result")) return false;
      return mir_verify_value_type(ir, value->left, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid FixedBufAlloc helper value", "allocator storage");
    case IR_VALUE_ALLOC_BYTES:
      mir_require_count(&requirements->allocator_helpers, 2, value->line, value->column, "std.mem.allocBytes");
      if (!mir_verify_helper_result_type(ir, value, IR_TYPE_MAYBE_BYTE_VIEW, "allocation result")) return false;
      if (!mir_verify_local_value_kind(ir, fun, value->local_index, IR_TYPE_ALLOC, value->line, value->column, "MIR verifier found invalid allocation helper target", "allocator")) return false;
      return mir_verify_value_is_integer(ir, value->left, "MIR verifier found invalid allocation helper length", "allocation length");
    case IR_VALUE_VEC_INIT:
      mir_require_count(&requirements->buffer_helpers, 1, value->line, value->column, "std.mem.vec");
      if (!mir_verify_helper_result_type(ir, value, IR_TYPE_VEC, "Vec result")) return false;
      return mir_verify_value_type(ir, value->left, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid Vec helper value", "Vec storage");
    case IR_VALUE_VEC_PUSH:
      mir_require_count(&requirements->buffer_helpers, 2, value->line, value->column, "std.mem.vecPush");
      if (!mir_verify_helper_result_type(ir, value, IR_TYPE_BOOL, "Vec push result")) return false;
      if (!mir_verify_local_value_kind(ir, fun, value->local_index, IR_TYPE_VEC, value->line, value->column, "MIR verifier found invalid Vec helper target", "Vec")) return false;
      return mir_verify_value_type(ir, value->left, IR_TYPE_U8, "MIR verifier found invalid Vec push value", "Vec item");
    case IR_VALUE_VEC_LEN:
    case IR_VALUE_VEC_CAPACITY:
      mir_require_count(&requirements->buffer_helpers, 3, value->line, value->column, value->kind == IR_VALUE_VEC_LEN ? "std.mem.vecLen" : "std.mem.vecCapacity");
      if (!mir_verify_helper_result_type(ir, value, IR_TYPE_USIZE, value->kind == IR_VALUE_VEC_LEN ? "Vec length result" : "Vec capacity result")) return false;
      return mir_verify_local_value_kind(ir, fun, value->local_index, IR_TYPE_VEC, value->line, value->column, "MIR verifier found invalid Vec helper target", "Vec");
    case IR_VALUE_JSON_PARSE_BYTES:
      mir_require_count(&requirements->allocator_helpers, 2, value->line, value->column, "std.json.parseBytes");
      mir_require_count(&requirements->runtime_helpers, 1, value->line, value->column, "std.json.parseBytes");
      mir_require_count(&requirements->host_runtime_imports, 1, value->line, value->column, "std.json.parseBytes");
      if (!mir_verify_helper_result_type(ir, value, IR_TYPE_I64, "JSON parse result")) return false;
      if (!mir_verify_local_value_kind(ir, fun, value->local_index, IR_TYPE_ALLOC, value->line, value->column, "MIR verifier found invalid JSON parse allocator", "allocator")) return false;
      return mir_verify_value_type(ir, value->left, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid JSON runtime helper input", "JSON bytes");
    case IR_VALUE_JSON_VALIDATE_BYTES:
    case IR_VALUE_JSON_STREAM_TOKENS_BYTES:
      mir_require_count(&requirements->runtime_helpers, 1, value->line, value->column, value->kind == IR_VALUE_JSON_VALIDATE_BYTES ? "std.json.validateBytes" : "std.json.streamTokensBytes");
      mir_require_count(&requirements->host_runtime_imports, 1, value->line, value->column, value->kind == IR_VALUE_JSON_VALIDATE_BYTES ? "std.json.validateBytes" : "std.json.streamTokensBytes");
      if (!mir_verify_helper_result_type(ir, value, value->kind == IR_VALUE_JSON_VALIDATE_BYTES ? IR_TYPE_BOOL : IR_TYPE_USIZE, value->kind == IR_VALUE_JSON_VALIDATE_BYTES ? "JSON validate result" : "JSON token count result")) return false;
      return mir_verify_value_type(ir, value->left, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid JSON runtime helper input", "JSON bytes");
    case IR_VALUE_HTTP_FETCH:
      mir_require_count(&requirements->runtime_helpers, 2, value->line, value->column, "std.http.fetch");
      mir_require_count(&requirements->host_runtime_imports, 2, value->line, value->column, "std.http.fetch");
      mir_require_count(&requirements->http_runtime_imports, 1, value->line, value->column, "std.http.fetch");
      if (!mir_verify_helper_result_type(ir, value, IR_TYPE_U64, "HTTP fetch result")) return false;
      if (!mir_verify_value_type(ir, value->left, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid HTTP fetch request", "HTTP request")) return false;
      if (!mir_verify_value_type(ir, value->right, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid HTTP fetch response buffer", "HTTP response buffer")) return false;
      return mir_verify_value_type(ir, value->index, IR_TYPE_I64, "MIR verifier found invalid HTTP fetch timeout", "HTTP timeout");
    case IR_VALUE_HTTP_RESULT_OK:
    case IR_VALUE_HTTP_RESULT_STATUS:
    case IR_VALUE_HTTP_RESULT_BODY_LEN:
    case IR_VALUE_HTTP_RESULT_ERROR:
      mir_require_count(&requirements->runtime_helpers, 1, value->line, value->column, "std.http result helper");
      mir_require_count(&requirements->host_runtime_imports, 1, value->line, value->column, "std.http result helper");
      if (!mir_verify_helper_result_type(ir, value, value->kind == IR_VALUE_HTTP_RESULT_OK ? IR_TYPE_BOOL : (value->kind == IR_VALUE_HTTP_RESULT_STATUS ? IR_TYPE_U16 : (value->kind == IR_VALUE_HTTP_RESULT_BODY_LEN ? IR_TYPE_USIZE : IR_TYPE_U32)), "HTTP result helper result")) return false;
      return mir_verify_value_type(ir, value->left, IR_TYPE_U64, "MIR verifier found invalid HTTP result helper input", "HTTP result");
    case IR_VALUE_HTTP_RESPONSE_LEN:
    case IR_VALUE_HTTP_RESPONSE_HEADERS_LEN:
    case IR_VALUE_HTTP_RESPONSE_BODY_OFFSET:
      mir_require_count(&requirements->runtime_helpers, 1, value->line, value->column, "std.http response helper");
      mir_require_count(&requirements->host_runtime_imports, 1, value->line, value->column, "std.http response helper");
      if (!mir_verify_helper_result_type(ir, value, IR_TYPE_USIZE, "HTTP response helper result")) return false;
      return mir_verify_value_type(ir, value->left, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid HTTP response helper input", "HTTP response");
    case IR_VALUE_HTTP_HEADER_VALUE:
      mir_require_count(&requirements->runtime_helpers, 1, value->line, value->column, "std.http.headerValue");
      mir_require_count(&requirements->host_runtime_imports, 1, value->line, value->column, "std.http.headerValue");
      if (!mir_verify_helper_result_type(ir, value, IR_TYPE_U64, "HTTP header value result")) return false;
      if (!mir_verify_value_type(ir, value->left, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid HTTP header helper input", "HTTP headers")) return false;
      return mir_verify_value_type(ir, value->right, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid HTTP header helper input", "HTTP header name");
    case IR_VALUE_HTTP_HEADER_FOUND:
    case IR_VALUE_HTTP_HEADER_OFFSET:
    case IR_VALUE_HTTP_HEADER_LEN:
      mir_require_count(&requirements->runtime_helpers, 1, value->line, value->column, "std.http header result helper");
      mir_require_count(&requirements->host_runtime_imports, 1, value->line, value->column, "std.http header result helper");
      if (!mir_verify_helper_result_type(ir, value, value->kind == IR_VALUE_HTTP_HEADER_FOUND ? IR_TYPE_BOOL : IR_TYPE_USIZE, "HTTP header result helper result")) return false;
      return mir_verify_value_type(ir, value->left, IR_TYPE_U64, "MIR verifier found invalid HTTP header result helper input", "HTTP header result");
    default:
      return true;
  }
}

static bool mir_verify_array_load_contract(IrProgram *ir, const IrFunction *fun, const IrValue *value) {
  if (!mir_verify_local_index(ir, fun, value->array_index, value->line, value->column, "MIR verifier found array load outside the local table")) return false;
  const IrLocal *local = &fun->locals[value->array_index];
  if (!local->is_array) {
    char actual[160];
    snprintf(actual, sizeof(actual), "local %s is %s", local->name ? local->name : "<unnamed>", mir_type_kind_name(local->type));
    mir_verify_mark_unsupported(ir, "MIR verifier found array load from a non-array local", value->line, value->column, actual);
    return false;
  }
  if (!mir_verify_value_is_integer(ir, value->index, "MIR verifier found invalid array load index", "array index")) return false;
  if (value->type != local->element_type) {
    char actual[160];
    snprintf(actual, sizeof(actual), "array load has %s but element is %s", mir_type_kind_name(value->type), mir_type_kind_name(local->element_type));
    mir_verify_mark_unsupported(ir, "MIR verifier found array load type mismatch", value->line, value->column, actual);
    return false;
  }
  return true;
}

static bool mir_verify_array_byte_view_contract(IrProgram *ir, const IrFunction *fun, const IrValue *value) {
  if (!mir_verify_value_type(ir, value, IR_TYPE_BYTE_VIEW, "MIR verifier found byte-view result type mismatch", "array byte view result")) return false;
  if (!mir_verify_local_index(ir, fun, value->array_index, value->line, value->column, "MIR verifier found array byte view outside the local table")) return false;
  const IrLocal *local = &fun->locals[value->array_index];
  if (!local->is_array || local->element_type != IR_TYPE_U8) {
    char actual[160];
    snprintf(actual, sizeof(actual), "local %s is %s array element %s", local->name ? local->name : "<unnamed>", local->is_array ? "an" : "not an", mir_type_kind_name(local->element_type));
    mir_verify_mark_unsupported(ir, "MIR verifier found array byte view from a non-byte array local", value->line, value->column, actual);
    return false;
  }
  if (value->data_len != local->array_len) {
    char actual[128];
    snprintf(actual, sizeof(actual), "byte view length %u but array length is %u", value->data_len, local->array_len);
    mir_verify_mark_unsupported(ir, "MIR verifier found array byte view length mismatch", value->line, value->column, actual);
    return false;
  }
  return true;
}

static bool mir_verify_maybe_value_contract(IrProgram *ir, const IrFunction *fun, const IrValue *value) {
  if (!mir_verify_local_index(ir, fun, value->local_index, value->line, value->column, "MIR verifier found maybe helper outside the local table")) return false;
  const IrLocal *local = &fun->locals[value->local_index];
  if (value->kind == IR_VALUE_MAYBE_HAS) {
    if (value->type != IR_TYPE_BOOL) {
      char actual[128];
      snprintf(actual, sizeof(actual), "Maybe.has result is %s", mir_type_kind_name(value->type));
      mir_verify_mark_unsupported(ir, "MIR verifier found maybe helper result type mismatch", value->line, value->column, actual);
      return false;
    }
    if (local->type == IR_TYPE_MAYBE_BYTE_VIEW || local->type == IR_TYPE_MAYBE_SCALAR) return true;
  } else if (value->kind == IR_VALUE_MAYBE_VALUE) {
    if (local->type == IR_TYPE_MAYBE_BYTE_VIEW) {
      return mir_verify_value_type(ir, value, IR_TYPE_BYTE_VIEW, "MIR verifier found maybe value type mismatch", "Maybe byte-view value");
    }
    if (local->type == IR_TYPE_MAYBE_SCALAR) {
      return mir_verify_value_is_integer(ir, value, "MIR verifier found maybe scalar value type mismatch", "Maybe scalar value");
    }
  }
  char actual[160];
  snprintf(actual, sizeof(actual), "local %s has %s", local->name ? local->name : "<unnamed>", mir_type_kind_name(local->type));
  mir_verify_mark_unsupported(ir, "MIR verifier found maybe helper for a non-Maybe local", value->line, value->column, actual);
  return false;
}

static bool mir_verify_byte_view_value_contract(IrProgram *ir, const IrValue *value) {
  if (!mir_verify_value_type(ir, value, IR_TYPE_BYTE_VIEW, "MIR verifier found byte-view result type mismatch", "byte-view result")) return false;
  if (value->kind == IR_VALUE_BYTE_SLICE) {
    if (!mir_verify_value_type(ir, value->left, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid byte slice base", "slice base")) return false;
    if (value->index && !mir_verify_value_is_integer(ir, value->index, "MIR verifier found invalid byte slice start", "slice start")) return false;
    if (value->right && !mir_verify_value_is_integer(ir, value->right, "MIR verifier found invalid byte slice end", "slice end")) return false;
  }
  return true;
}

static bool mir_verify_direct_value(IrProgram *ir, const IrFunction *fun, const IrValue *value, MirHelperRequirements *requirements) {
  if (!ir || !ir->mir_valid) return false;
  if (!value) return true;
  if (value->kind == IR_VALUE_CALL && !mir_verify_direct_call_contract(ir, value)) return false;
  if (!mir_verify_direct_helper_value_contract(ir, fun, value, requirements)) return false;
  if (value->kind == IR_VALUE_LOCAL) {
    if (!mir_verify_local_index(ir, fun, value->local_index, value->line, value->column, "MIR verifier found local value outside the local table")) return false;
    const IrLocal *local = &fun->locals[value->local_index];
    if (value->type != local->type) {
      char actual[192];
      snprintf(actual, sizeof(actual), "local value %u has %s but local %s has %s", value->local_index, mir_type_kind_name(value->type), local->name ? local->name : "<unnamed>", mir_type_kind_name(local->type));
      mir_verify_mark_unsupported(ir, "MIR verifier found local value type mismatch", value->line, value->column, actual);
      return false;
    }
  }
  if (value->kind == IR_VALUE_FIELD_LOAD) {
    if (!mir_verify_local_index(ir, fun, value->local_index, value->line, value->column, "MIR verifier found field load outside the local table")) return false;
    const IrLocal *local = &fun->locals[value->local_index];
    if (!local->is_record) {
      char actual[160];
      snprintf(actual, sizeof(actual), "local %s is %s", local->name ? local->name : "<unnamed>", mir_type_kind_name(local->type));
      mir_verify_mark_unsupported(ir, "MIR verifier found field load from a non-record local", value->line, value->column, actual);
      return false;
    }
    if (!mir_verify_record_field_span(ir, local, value->field_offset, value->type, value->line, value->column, "MIR verifier found field load outside the local storage")) return false;
  }
  if (value->kind == IR_VALUE_INDEX_LOAD && !mir_verify_array_load_contract(ir, fun, value)) return false;
  if (value->kind == IR_VALUE_ARRAY_BYTE_VIEW && !mir_verify_array_byte_view_contract(ir, fun, value)) return false;
  if ((value->kind == IR_VALUE_MAYBE_HAS || value->kind == IR_VALUE_MAYBE_VALUE) && !mir_verify_maybe_value_contract(ir, fun, value)) return false;
  if (value->kind == IR_VALUE_BYTE_SLICE && !mir_verify_byte_view_value_contract(ir, value)) return false;
  if (value->kind == IR_VALUE_BYTE_VIEW_LEN) {
    if (!mir_verify_value_type(ir, value, IR_TYPE_USIZE, "MIR verifier found byte-view length result type mismatch", "byte-view length")) return false;
    if (!mir_verify_value_type(ir, value->left, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid byte-view length input", "byte-view length input")) return false;
  }
  if (value->kind == IR_VALUE_BYTE_VIEW_INDEX_LOAD) {
    if (!mir_verify_value_type(ir, value, IR_TYPE_U8, "MIR verifier found byte-view index load result type mismatch", "byte-view index load")) return false;
    if (!mir_verify_value_type(ir, value->left, IR_TYPE_BYTE_VIEW, "MIR verifier found invalid byte-view index load input", "byte-view index load input")) return false;
    if (!mir_verify_value_is_integer(ir, value->index, "MIR verifier found invalid byte-view index load index", "byte-view index")) return false;
  }
  for (size_t i = 0; i < value->arg_len; i++) {
    if (!mir_verify_direct_value(ir, fun, value->args[i], requirements)) return false;
  }
  if (!mir_verify_direct_value(ir, fun, value->index, requirements)) return false;
  if (!mir_verify_direct_value(ir, fun, value->left, requirements)) return false;
  if (!mir_verify_direct_value(ir, fun, value->right, requirements)) return false;
  return true;
}

static bool mir_verify_direct_return_instr(IrProgram *ir, const IrFunction *fun, const IrInstr *instr) {
  if (!ir || !ir->mir_valid || !fun || !instr) return false;
  const IrValue *value = instr->value;
  if (fun->raises) {
    if (fun->value_return_type == IR_TYPE_VOID) {
      if (!value) return true;
      char actual[160];
      snprintf(actual, sizeof(actual), "fallible Void return carries %s", mir_type_kind_name(value->type));
      mir_verify_mark_unsupported(ir, "MIR verifier found return value mismatch", instr->line, instr->column, actual);
      return false;
    }
    if (value && value->type == fun->value_return_type) return true;
    char actual[160];
    snprintf(actual, sizeof(actual), "fallible return has %s but function value return is %s", value ? mir_type_kind_name(value->type) : "missing", mir_type_kind_name(fun->value_return_type));
    mir_verify_mark_unsupported(ir, "MIR verifier found return value mismatch", instr->line, instr->column, actual);
    return false;
  }
  if (fun->return_type == IR_TYPE_VOID) {
    if (!value) return true;
    char actual[160];
    snprintf(actual, sizeof(actual), "Void return carries %s", mir_type_kind_name(value->type));
    mir_verify_mark_unsupported(ir, "MIR verifier found return value mismatch", instr->line, instr->column, actual);
    return false;
  }
  if (value && value->type == fun->return_type) return true;
  char actual[160];
  snprintf(actual, sizeof(actual), "return has %s but function returns %s", value ? mir_type_kind_name(value->type) : "missing", mir_type_kind_name(fun->return_type));
  mir_verify_mark_unsupported(ir, "MIR verifier found return value mismatch", instr->line, instr->column, actual);
  return false;
}

static bool mir_verify_direct_instr_contract(IrProgram *ir, const IrFunction *fun, const IrInstr *instr, MirHelperRequirements *requirements) {
  if (!ir || !ir->mir_valid || !fun || !instr) return false;
  switch (instr->kind) {
    case IR_INSTR_LOCAL_SET: {
      if (!mir_verify_local_index(ir, fun, instr->local_index, instr->line, instr->column, "MIR verifier found local write outside the local table")) return false;
      if (!instr->value) {
        mir_verify_mark_unsupported(ir, "MIR verifier found local write without a value", instr->line, instr->column, "missing local initializer");
        return false;
      }
      const IrLocal *local = &fun->locals[instr->local_index];
      if (!mir_value_can_initialize_local(local, instr->value)) {
        char actual[192];
        snprintf(actual, sizeof(actual), "local %s has %s but value has %s", local->name ? local->name : "<unnamed>", mir_type_kind_name(local->type), mir_type_kind_name(instr->value->type));
        mir_verify_mark_unsupported(ir, "MIR verifier found local write type mismatch", instr->line, instr->column, actual);
        return false;
      }
      break;
    }
    case IR_INSTR_INDEX_STORE: {
      if (!mir_verify_local_index(ir, fun, instr->array_index, instr->line, instr->column, "MIR verifier found array write outside the local table")) return false;
      const IrLocal *local = &fun->locals[instr->array_index];
      if (!local->is_array) {
        char actual[160];
        snprintf(actual, sizeof(actual), "local %s is %s", local->name ? local->name : "<unnamed>", mir_type_kind_name(local->type));
        mir_verify_mark_unsupported(ir, "MIR verifier found array write to a non-array local", instr->line, instr->column, actual);
        return false;
      }
      if (!instr->index || !mir_type_is_integer_value(instr->index->type)) {
        char actual[128];
        snprintf(actual, sizeof(actual), "array index is %s", instr->index ? mir_type_kind_name(instr->index->type) : "missing");
        mir_verify_mark_unsupported(ir, "MIR verifier found invalid array write index", instr->line, instr->column, actual);
        return false;
      }
      if (!instr->value || instr->value->type != local->element_type) {
        char actual[160];
        snprintf(actual, sizeof(actual), "array write has %s but element is %s", instr->value ? mir_type_kind_name(instr->value->type) : "missing", mir_type_kind_name(local->element_type));
        mir_verify_mark_unsupported(ir, "MIR verifier found array write type mismatch", instr->line, instr->column, actual);
        return false;
      }
      break;
    }
    case IR_INSTR_FIELD_STORE: {
      if (!mir_verify_local_index(ir, fun, instr->local_index, instr->line, instr->column, "MIR verifier found field write outside the local table")) return false;
      const IrLocal *local = &fun->locals[instr->local_index];
      if (!local->is_record) {
        char actual[160];
        snprintf(actual, sizeof(actual), "local %s is %s", local->name ? local->name : "<unnamed>", mir_type_kind_name(local->type));
        mir_verify_mark_unsupported(ir, "MIR verifier found field write to a non-record local", instr->line, instr->column, actual);
        return false;
      }
      if (!instr->value) {
        mir_verify_mark_unsupported(ir, "MIR verifier found field write without a value", instr->line, instr->column, "missing field value");
        return false;
      }
      if (!mir_verify_record_field_span(ir, local, instr->field_offset, instr->value->type, instr->line, instr->column, "MIR verifier found field write outside the local storage")) return false;
      break;
    }
    case IR_INSTR_WORLD_WRITE:
      mir_require_count(&requirements->runtime_helpers, 1, instr->line, instr->column, "world.write");
      if (!instr->value || instr->value->type != IR_TYPE_BYTE_VIEW) {
        char actual[128];
        snprintf(actual, sizeof(actual), "world write value is %s", instr->value ? mir_type_kind_name(instr->value->type) : "missing");
        mir_verify_mark_unsupported(ir, "MIR verifier found invalid world write value", instr->line, instr->column, actual);
        return false;
      }
      break;
    case IR_INSTR_RETURN:
      if (!mir_verify_direct_return_instr(ir, fun, instr)) return false;
      break;
    case IR_INSTR_IF:
    case IR_INSTR_WHILE:
      if (!instr->value || instr->value->type != IR_TYPE_BOOL) {
        char actual[128];
        snprintf(actual, sizeof(actual), "branch condition is %s", instr->value ? mir_type_kind_name(instr->value->type) : "missing");
        mir_verify_mark_unsupported(ir, "MIR verifier found invalid branch condition", instr->line, instr->column, actual);
        return false;
      }
      break;
    case IR_INSTR_RAISE:
      if (!fun->raises && !fun->world_param_name) {
        mir_verify_mark_unsupported(ir, "MIR verifier found raise in a non-fallible function", instr->line, instr->column, fun->name ? fun->name : "<unnamed>");
        return false;
      }
      break;
    case IR_INSTR_EXPR:
      break;
  }
  return true;
}

static bool mir_verify_direct_instrs(IrProgram *ir, const IrFunction *fun, const IrInstr *instrs, size_t len, MirHelperRequirements *requirements) {
  if (!ir || !ir->mir_valid) return false;
  for (size_t i = 0; i < len; i++) {
    const IrInstr *instr = &instrs[i];
    if (!mir_verify_direct_value(ir, fun, instr->value, requirements)) return false;
    if (!mir_verify_direct_value(ir, fun, instr->index, requirements)) return false;
    if (!mir_verify_direct_instr_contract(ir, fun, instr, requirements)) return false;
    if (!mir_verify_direct_instrs(ir, fun, instr->then_instrs, instr->then_len, requirements)) return false;
    if (!mir_verify_direct_instrs(ir, fun, instr->else_instrs, instr->else_len, requirements)) return false;
  }
  return true;
}

static bool mir_verify_count_satisfies(IrProgram *ir, const MirCountRequirement *req, size_t actual, const char *name, const char *message) {
  if (!ir || !ir->mir_valid || !req || req->count <= actual) return ir && ir->mir_valid;
  char detail[192];
  snprintf(detail, sizeof(detail), "%s count %zu but MIR requires at least %zu for %s", name ? name : "helper", actual, req->count, req->reason ? req->reason : "helper use");
  mir_verify_mark_unsupported(ir, message, req->line, req->column, detail);
  return false;
}

static bool mir_verify_direct_helper_requirements(IrProgram *ir, const MirHelperRequirements *requirements) {
  if (!ir || !ir->mir_valid || !requirements) return false;
  if (!mir_verify_count_satisfies(ir, &requirements->allocator_helpers, ir->direct_allocator_helper_count, "allocator helper", "MIR verifier found missing allocator helper contract")) return false;
  if (!mir_verify_count_satisfies(ir, &requirements->buffer_helpers, ir->direct_buffer_helper_count, "buffer helper", "MIR verifier found missing buffer helper contract")) return false;
  if (!mir_verify_count_satisfies(ir, &requirements->runtime_helpers, ir->direct_runtime_helper_count, "runtime helper", "MIR verifier found missing runtime helper contract")) return false;
  if (!mir_verify_count_satisfies(ir, &requirements->host_runtime_imports, ir->direct_host_runtime_import_count, "host runtime import", "MIR verifier found missing host runtime import contract")) return false;
  if (!mir_verify_count_satisfies(ir, &requirements->http_runtime_imports, ir->direct_http_runtime_import_count, "HTTP runtime import", "MIR verifier found missing HTTP runtime import contract")) return false;
  return true;
}

bool z_mir_verify_direct_contracts(IrProgram *ir) {
  if (!ir || !ir->mir_valid) return false;
  for (size_t i = 0; i < ir->function_len; i++) {
    if (!mir_verify_direct_function_contract(ir, &ir->functions[i])) return false;
  }
  MirHelperRequirements requirements = {0};
  for (size_t i = 0; i < ir->function_len; i++) {
    if (!mir_verify_direct_instrs(ir, &ir->functions[i], ir->functions[i].instrs, ir->functions[i].instr_len, &requirements)) return false;
  }
  return mir_verify_direct_helper_requirements(ir, &requirements);
}
