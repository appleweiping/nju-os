// M4: crepl — a C Read-Eval-Print Loop.
//
// NJU OS 2022 (jyy) MiniLab M4.  Reads lines from stdin; each line is either
//
//   * a function definition   e.g.  int go(int a, int b) { return a + b; }
//   * an integer expression   e.g.  go(1, 2) * 10 + 1
//
// A function definition is compiled into a fresh shared object and dlopen()ed,
// so its symbol becomes visible to everything compiled afterwards.  An
// expression is wrapped into a synthetic function
//
//   int __expr_wrapper_N() { return (EXPR); }
//
// compiled into its own shared object, dlopen()ed, resolved with dlsym(), and
// called — printing the returned int.  Because each wrapper .so is loaded with
// RTLD_GLOBAL, later expressions/functions can reference earlier functions
// (the classic "incremental linking via the dynamic loader" trick).
//
// The trick that makes cross-.so references resolve: we do NOT declare the
// referenced functions.  We compile every snippet with the *implicit
// declaration* fallback (a plain `f(...)` call in C is assumed to return int),
// and the dynamic loader binds the call to the globally-loaded symbol at
// dlopen time.  We therefore must compile snippets as C (not C++), keep symbols
// with default visibility, and load with RTLD_GLOBAL.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/wait.h>
#include <fcntl.h>

static int expr_counter = 0;

// Heuristic used by the lab: a line that begins with "int " is treated as a
// function definition; anything else is an expression.
static int is_function_definition(const char *line) {
  const char *p = line;
  while (*p == ' ' || *p == '\t') p++;
  return strncmp(p, "int ", 4) == 0 || strncmp(p, "int\t", 4) == 0;
}

// Compile `src` (a full C translation unit written to a temp .c file) into a
// shared object at `so_path`.  Returns 0 on success.  Compiler diagnostics are
// forwarded to our stderr so the user sees syntax errors.
static int compile_to_so(const char *src, const char *so_path) {
  char c_path[] = "/tmp/crepl-XXXXXX.c";
  int fd = mkstemps(c_path, 2);      // 2 = length of the ".c" suffix
  if (fd < 0) { perror("mkstemps"); return -1; }
  if (write(fd, src, strlen(src)) < 0) { perror("write"); close(fd); return -1; }
  close(fd);

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); unlink(c_path); return -1; }

  if (pid == 0) {
    // gcc -m64 -fPIC -shared -w -o SO C   (-w silences implicit-decl warnings)
    execlp("gcc", "gcc",
#if __x86_64__
           "-m64",
#else
           "-m32",
#endif
           "-fPIC", "-shared", "-w",
           "-o", so_path, c_path, (char *)NULL);
    perror("crepl: exec gcc");
    _exit(127);
  }

  int status;
  waitpid(pid, &status, 0);
  unlink(c_path);
  return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

// Handle a function-definition line: compile it verbatim into a .so and load
// it globally so its symbols are available to later snippets.
static void handle_function(const char *line) {
  static int fn_counter = 0;
  char so_path[64];
  snprintf(so_path, sizeof(so_path), "/tmp/crepl-fn-%d.so", fn_counter++);

  if (compile_to_so(line, so_path) != 0) {
    printf("Compile error.\n");
    return;
  }
  void *handle = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    printf("Load error: %s\n", dlerror());
    return;
  }
  // Keep the handle open (leak intentionally) so its symbols persist for the
  // rest of the session.  Report success like the reference implementation.
  printf("OK.\n");
}

// Handle an expression line: wrap it, compile, load, resolve, call, print.
static void handle_expression(const char *line) {
  int id = expr_counter++;
  char fname[32];
  snprintf(fname, sizeof(fname), "__expr_wrapper_%d", id);

  // Build the wrapper translation unit.  We do NOT forward-declare the
  // functions the expression may call; C treats unknown calls as returning
  // int, and the loader binds them to the already-loaded globals.
  char src[8192];
  snprintf(src, sizeof(src),
           "int %s() { return (%s); }\n", fname, line);

  char so_path[64];
  snprintf(so_path, sizeof(so_path), "/tmp/crepl-expr-%d.so", id);

  if (compile_to_so(src, so_path) != 0) {
    printf("Compile error.\n");
    return;
  }

  void *handle = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
  if (!handle) {
    printf("Load error: %s\n", dlerror());
    return;
  }

  int (*fn)() = (int (*)())dlsym(handle, fname);
  if (!fn) {
    printf("Symbol error: %s\n", dlerror());
    dlclose(handle);
    return;
  }

  int result = fn();
  printf("= %d\n", result);

  dlclose(handle);
  unlink(so_path);
}

int main(int argc, char *argv[]) {
  static char line[4096];
  setbuf(stdout, NULL);

  while (1) {
    printf("crepl> ");
    if (!fgets(line, sizeof(line), stdin)) break;

    // Trim trailing newline.
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';

    // Skip blank lines.
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') continue;

    if (is_function_definition(line)) {
      handle_function(line);
    } else {
      handle_expression(line);
    }
  }
  return 0;
}
