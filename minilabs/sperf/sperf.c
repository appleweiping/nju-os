// M3: sperf — a system-call profiler built on top of strace.
//
// NJU OS 2022 (jyy) MiniLab M3.  `sperf COMMAND [ARGS]...` runs COMMAND under
// strace with per-call timing (`strace -T -f`), parses strace's stderr, and
// aggregates the wall-clock time spent inside each distinct system call.  It
// prints a live "top" of the most time-consuming syscalls, refreshed roughly
// once a second, and a final summary when COMMAND exits.
//
// Design:
//   - pipe(2) + fork(2): child redirects its stderr to the pipe write-end and
//     execvp("strace", "-T", "-f", "-e", "trace=all", COMMAND, ARGS...).
//   - parent reads strace output line by line, extracting "<seconds>" trailers
//     and the syscall name at the start of each traced line.
//   - a simple hash-less linear table accumulates per-syscall totals.
//   - every ~1s of wall time we clear the screen and reprint the ranking.
//
// strace must be found via PATH (we search a few standard locations too, as the
// skeleton did).  Times are approximate: strace itself perturbs timing, which
// is fine for a relative profiler.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/time.h>

#define MAX_SYS   1024
#define LINE_MAX_ 65536

typedef struct {
  char   name[64];
  double time;      // accumulated seconds
} syscall_stat;

static syscall_stat stats[MAX_SYS];
static int          nstats = 0;
static double       total_time = 0.0;

static double now_sec(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec / 1e6;
}

// Find (or create) the accumulator for a syscall name.
static syscall_stat *get_stat(const char *name) {
  for (int i = 0; i < nstats; i++)
    if (strcmp(stats[i].name, name) == 0) return &stats[i];
  if (nstats < MAX_SYS) {
    syscall_stat *s = &stats[nstats++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->time = 0.0;
    return s;
  }
  return NULL;   // table full; ignore extra names
}

static int cmp_desc(const void *a, const void *b) {
  double da = ((const syscall_stat *)a)->time;
  double db = ((const syscall_stat *)b)->time;
  if (da < db) return 1;
  if (da > db) return -1;
  return 0;
}

// Print the current ranking (top 10) to stdout.
static void print_report(void) {
  syscall_stat snapshot[MAX_SYS];
  memcpy(snapshot, stats, sizeof(syscall_stat) * nstats);
  qsort(snapshot, nstats, sizeof(syscall_stat), cmp_desc);

  printf("\033[H\033[2J");   // move home + clear screen (terminal "top" style)
  printf("=== sperf: system-call time profile ===\n");
  printf("total traced syscall time: %.6f s\n\n", total_time);
  int limit = nstats < 10 ? nstats : 10;
  for (int i = 0; i < limit; i++) {
    double pct = total_time > 0 ? 100.0 * snapshot[i].time / total_time : 0.0;
    printf("%-20s %10.6f s  (%5.1f%%)\n",
           snapshot[i].name, snapshot[i].time, pct);
  }
  fflush(stdout);
}

// Parse one strace line.  Timed lines look like:
//   openat(AT_FDCWD, "/etc/ld.so.cache", O_RDONLY|O_CLOEXEC) = 3 <0.000012>
// With -f, lines may be prefixed by "[pid  1234] ".  Unfinished/resumed and
// signal lines have no "<...>" trailer and are skipped.
static void parse_line(const char *line) {
  const char *p = line;

  // Skip an optional "[pid NNNN] " prefix.
  if (strncmp(p, "[pid", 4) == 0) {
    const char *close = strchr(p, ']');
    if (close) p = close + 1;
    while (*p == ' ') p++;
  }

  // The syscall name is the leading identifier up to '(' .
  if (!isalpha((unsigned char)*p) && *p != '_') return;
  char name[64];
  int i = 0;
  while ((isalnum((unsigned char)*p) || *p == '_') && i < 63) name[i++] = *p++;
  name[i] = '\0';
  if (*p != '(') return;         // not "name(...)"; ignore

  // The duration is the last "<...>" on the line.
  const char *lt = strrchr(line, '<');
  const char *gt = strrchr(line, '>');
  if (!lt || !gt || gt < lt) return;
  char dur[64];
  size_t dl = (size_t)(gt - lt - 1);
  if (dl >= sizeof(dur)) return;
  memcpy(dur, lt + 1, dl);
  dur[dl] = '\0';

  // Duration must be a plain float (skip things like "<unfinished ...>").
  char *end;
  double t = strtod(dur, &end);
  if (end == dur || *end != '\0') return;

  syscall_stat *s = get_stat(name);
  if (s) { s->time += t; total_time += t; }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s COMMAND [ARG]...\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  int pipefd[2];
  if (pipe(pipefd) < 0) { perror("pipe"); exit(EXIT_FAILURE); }

  pid_t pid = fork();
  if (pid < 0) { perror("fork"); exit(EXIT_FAILURE); }

  if (pid == 0) {
    // Child: strace writes diagnostics to stderr -> redirect to pipe.
    close(pipefd[0]);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    // Build: strace -T -f -e trace=all COMMAND ARGS...
    // argv layout: [strace, -T, -f, -e, trace=all, argv[1], argv[2], ..., NULL]
    int extra = 5;
    char **sargv = calloc(argc + extra, sizeof(char *));
    sargv[0] = "strace";
    sargv[1] = "-T";
    sargv[2] = "-f";
    sargv[3] = "-e";
    sargv[4] = "trace=all";
    for (int k = 1; k < argc; k++) sargv[extra + k - 1] = argv[k];
    sargv[extra + argc - 1] = NULL;

    // Let strace inherit our environment (so COMMAND resolves via PATH).
    execvp("strace", sargv);
    execv("/usr/bin/strace", sargv);
    execv("/bin/strace", sargv);
    perror("sperf: cannot exec strace");
    _exit(127);
  }

  // Parent: read strace output line by line.
  close(pipefd[1]);
  FILE *f = fdopen(pipefd[0], "r");
  if (!f) { perror("fdopen"); exit(EXIT_FAILURE); }

  static char line[LINE_MAX_];
  double last_print = now_sec();
  while (fgets(line, sizeof(line), f)) {
    parse_line(line);
    double t = now_sec();
    if (t - last_print >= 1.0) {   // refresh at most ~1 Hz
      print_report();
      last_print = t;
    }
  }
  fclose(f);

  int status;
  waitpid(pid, &status, 0);

  print_report();   // final summary
  return 0;
}
