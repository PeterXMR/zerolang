#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "process_exec.h"
#include "process_path.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

void z_process_argv_init(ZProcessArgv *argv) {
  argv->items = NULL;
  argv->len = 0;
  argv->cap = 0;
}

bool z_process_argv_push(ZProcessArgv *argv, const char *value) {
  if (!argv || !value || !value[0]) return false;
  size_t required = argv->len + 2;
  if (required > argv->cap) {
    size_t next = z_grow_capacity(argv->cap, required, 8);
    argv->items = z_checked_reallocarray(argv->items, next, sizeof(char *));
    argv->cap = next;
  }
  argv->items[argv->len++] = z_strdup(value);
  argv->items[argv->len] = NULL;
  return true;
}

bool z_process_argv_append_flag_text(ZProcessArgv *argv, const char *text, bool *suppress_stderr) {
  const char *cursor = text ? text : "";
  while (*cursor) {
    while (isspace((unsigned char)*cursor)) cursor++;
    if (!*cursor) break;

    ZBuf token;
    zbuf_init(&token);
    while (*cursor && !isspace((unsigned char)*cursor)) {
      if (*cursor == '\'') {
        cursor++;
        while (*cursor && *cursor != '\'') zbuf_append_char(&token, *cursor++);
        if (*cursor == '\'') cursor++;
      } else if (*cursor == '\\' && cursor[1]) {
        cursor++;
        zbuf_append_char(&token, *cursor++);
      } else {
        zbuf_append_char(&token, *cursor++);
      }
    }

    bool ok = true;
    if (token.len > 0) {
      if (strcmp(token.data, "2>/dev/null") == 0) {
        if (suppress_stderr) *suppress_stderr = true;
      } else {
        ok = z_process_argv_push(argv, token.data);
      }
    }
    zbuf_free(&token);
    if (!ok) return false;
  }
  return true;
}

void z_process_argv_free(ZProcessArgv *argv) {
  if (!argv) return;
  for (size_t i = 0; i < argv->len; i++) free(argv->items[i]);
  free(argv->items);
  argv->items = NULL;
  argv->len = 0;
  argv->cap = 0;
}

bool z_process_ensure_dir(const char *path) {
  if (!path || !path[0]) return false;
#if defined(_WIN32)
  if (_mkdir(path) == 0) return true;
#else
  if (mkdir(path, 0777) == 0) return true;
#endif
  return errno == EEXIST;
}

#if !defined(_WIN32)
static bool z_process_suppress_stream(FILE *stream) {
  return freopen("/dev/null", "w", stream) != NULL;
}
#endif

bool z_process_run_argv(const ZProcessArgv *argv, bool suppress_stdout, bool suppress_stderr, bool uses_zig_env) {
  if (!argv || argv->len == 0 || !argv->items || !argv->items[0]) return false;
  char *executable = z_process_resolve_executable(argv->items[0]);
  if (!executable) return false;
#if defined(_WIN32)
  (void)suppress_stdout;
  (void)suppress_stderr;
  (void)uses_zig_env;
  int rc = _spawnv(_P_WAIT, executable, (const char *const *)argv->items);
  free(executable);
  return rc == 0;
#else
  pid_t pid = fork();
  if (pid < 0) {
    free(executable);
    return false;
  }
  if (pid == 0) {
    if (uses_zig_env) {
      if (setenv("ZIG_GLOBAL_CACHE_DIR", ".zero/zig-global-cache", 1) != 0) _exit(127);
      if (setenv("ZIG_LOCAL_CACHE_DIR", ".zero/zig-local-cache", 1) != 0) _exit(127);
    }
    if (suppress_stdout && !z_process_suppress_stream(stdout)) _exit(127);
    if (suppress_stderr && !z_process_suppress_stream(stderr)) _exit(127);
    execv(executable, argv->items);
    _exit(127);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      free(executable);
      return false;
    }
  }
  free(executable);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

char *z_process_first_stdout_line(const char *const *argv, bool suppress_stderr) {
  if (!argv || !argv[0]) return z_strdup("");
  char *executable = z_process_resolve_executable(argv[0]);
  if (!executable) return z_strdup("");
#if defined(_WIN32)
  (void)suppress_stderr;
  free(executable);
  return z_strdup("");
#else
  int pipe_fd[2];
  if (pipe(pipe_fd) != 0) {
    free(executable);
    return z_strdup("");
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    free(executable);
    return z_strdup("");
  }
  if (pid == 0) {
    close(pipe_fd[0]);
    if (dup2(pipe_fd[1], STDOUT_FILENO) < 0) _exit(127);
    close(pipe_fd[1]);
    if (suppress_stderr && !z_process_suppress_stream(stderr)) _exit(127);
    execv(executable, (char *const *)argv);
    _exit(127);
  }
  free(executable);
  close(pipe_fd[1]);
  ZBuf line;
  zbuf_init(&line);
  bool capturing = true;
  char bytes[256];
  while (true) {
    ssize_t n = read(pipe_fd[0], bytes, sizeof(bytes));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    for (ssize_t i = 0; capturing && i < n; i++) {
      if (bytes[i] == '\n' || bytes[i] == '\r') {
        capturing = false;
      } else if (line.len < 255) {
        zbuf_append_char(&line, bytes[i]);
      }
    }
  }
  close(pipe_fd[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) break;
  char *out = line.data ? line.data : z_strdup("");
  line.data = NULL;
  zbuf_free(&line);
  return out;
#endif
}
