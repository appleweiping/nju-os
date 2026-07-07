// M1: pstree — print the tree of process parent/child relationships.
//
// NJU OS 2022 (jyy) MiniLab M1. Reads /proc, reconstructs the process
// hierarchy from each process's (pid, ppid, comm) and prints it as an
// indented tree, GNU-pstree style.
//
// Supported options (long + short), matching the lab handout:
//   -p, --show-pids       show PID after each command name
//   -n, --numeric-sort    sort children numerically by PID (default: by name)
//   -V, --version         print version information and exit
//
// Combined short options are NOT required by the handout, but we accept the
// three flags in any order/position.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <assert.h>
#include <getopt.h>

#define MAX_PROC 65536
#define NAME_LEN 256

typedef struct {
  int pid;
  int ppid;
  char name[NAME_LEN];
  int *children;   // indices into procs[]
  int nchild;
  int cap_child;
} proc_t;

static proc_t procs[MAX_PROC];
static int nproc = 0;

// options
static int opt_show_pids = 0;
static int opt_numeric   = 0;

// Return 1 if `s` is all digits (a candidate /proc/<pid> directory).
static int is_number(const char *s) {
  if (!*s) return 0;
  for (const char *p = s; *p; p++)
    if (!isdigit((unsigned char)*p)) return 0;
  return 1;
}

// Parse /proc/<pid>/stat to extract pid, comm and ppid.
// Format: "pid (comm) state ppid ...".  comm may contain spaces and
// parentheses, so we locate the LAST ')' to delimit it robustly.
static int read_stat(int pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  FILE *f = fopen(path, "r");
  if (!f) return -1;             // process may have vanished; skip

  static char buf[8192];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0) return -1;
  buf[n] = '\0';

  char *lp = strchr(buf, '(');
  char *rp = strrchr(buf, ')');
  if (!lp || !rp || rp < lp) return -1;

  proc_t *p = &procs[nproc];
  p->pid = pid;

  size_t name_len = (size_t)(rp - lp - 1);
  if (name_len >= NAME_LEN) name_len = NAME_LEN - 1;
  memcpy(p->name, lp + 1, name_len);
  p->name[name_len] = '\0';

  // After ")": " state ppid ..."
  char state;
  int ppid;
  if (sscanf(rp + 1, " %c %d", &state, &ppid) != 2) return -1;
  p->ppid = ppid;
  p->children = NULL;
  p->nchild = 0;
  p->cap_child = 0;

  nproc++;
  return 0;
}

// Scan /proc for all numeric entries (processes).
static void scan_procs(void) {
  DIR *d = opendir("/proc");
  if (!d) { perror("opendir /proc"); exit(EXIT_FAILURE); }
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (is_number(e->d_name) && nproc < MAX_PROC) {
      read_stat(atoi(e->d_name));
    }
  }
  closedir(d);
}

static int find_index(int pid) {
  for (int i = 0; i < nproc; i++)
    if (procs[i].pid == pid) return i;
  return -1;
}

static void add_child(proc_t *parent, int child_idx) {
  if (parent->nchild == parent->cap_child) {
    parent->cap_child = parent->cap_child ? parent->cap_child * 2 : 4;
    parent->children = realloc(parent->children,
                               parent->cap_child * sizeof(int));
    assert(parent->children);
  }
  parent->children[parent->nchild++] = child_idx;
}

// Link every process to its parent (ppid). Roots (ppid not present, e.g.
// pid 0/1) are collected separately.
static void build_tree(int *roots, int *nroots) {
  *nroots = 0;
  for (int i = 0; i < nproc; i++) {
    int pi = find_index(procs[i].ppid);
    if (pi >= 0 && procs[i].ppid != procs[i].pid) {
      add_child(&procs[pi], i);
    } else {
      roots[(*nroots)++] = i;
    }
  }
}

static int cmp_by_name(const void *a, const void *b) {
  int ia = *(const int *)a, ib = *(const int *)b;
  int c = strcmp(procs[ia].name, procs[ib].name);
  if (c != 0) return c;
  return procs[ia].pid - procs[ib].pid;   // tie-break by pid
}

static int cmp_by_pid(const void *a, const void *b) {
  int ia = *(const int *)a, ib = *(const int *)b;
  return procs[ia].pid - procs[ib].pid;
}

static void sort_children(int idx) {
  proc_t *p = &procs[idx];
  if (p->nchild > 1) {
    qsort(p->children, p->nchild, sizeof(int),
          opt_numeric ? cmp_by_pid : cmp_by_name);
  }
  for (int i = 0; i < p->nchild; i++)
    sort_children(p->children[i]);
}

// Recursive tree printer. `prefix` holds the leading whitespace/│ characters
// carried down from ancestors; `is_last` marks whether this node is the last
// child of its parent (chooses └── vs ├──).
static void print_tree(int idx, const char *prefix, int is_last, int is_root) {
  proc_t *p = &procs[idx];

  if (is_root) {
    printf("%s", p->name);
  } else {
    printf("%s%s%s", prefix, is_last ? "└─ " : "├─ ", p->name);
  }
  if (opt_show_pids) printf("(%d)", p->pid);
  printf("\n");

  // Build the prefix passed to our children.
  char child_prefix[4096];
  if (is_root) {
    child_prefix[0] = '\0';
  } else {
    snprintf(child_prefix, sizeof(child_prefix), "%s%s",
             prefix, is_last ? "   " : "│  ");
  }

  for (int i = 0; i < p->nchild; i++) {
    print_tree(p->children[i], child_prefix, i == p->nchild - 1, 0);
  }
}

static void print_version(void) {
  fprintf(stderr,
          "pstree (NJU OS 2022 MiniLab M1) 1.0\n"
          "Copyright (C) 2022 An independent educational reimplementation.\n"
          "This is free software; see the source for copying conditions.\n");
}

int main(int argc, char *argv[]) {
  static struct option long_opts[] = {
    {"show-pids",    no_argument, 0, 'p'},
    {"numeric-sort", no_argument, 0, 'n'},
    {"version",      no_argument, 0, 'V'},
    {0, 0, 0, 0}
  };

  int c;
  while ((c = getopt_long(argc, argv, "pnV", long_opts, NULL)) != -1) {
    switch (c) {
      case 'p': opt_show_pids = 1; break;
      case 'n': opt_numeric   = 1; break;
      case 'V': print_version(); return 0;
      default:
        fprintf(stderr, "Usage: %s [-p|--show-pids] [-n|--numeric-sort] "
                        "[-V|--version]\n", argv[0]);
        return 1;
    }
  }

  scan_procs();

  int roots[MAX_PROC], nroots = 0;
  build_tree(roots, &nroots);

  // Sort the roots too, and each subtree.
  qsort(roots, nroots, sizeof(int),
        opt_numeric ? cmp_by_pid : cmp_by_name);
  for (int i = 0; i < nroots; i++) sort_children(roots[i]);

  for (int i = 0; i < nroots; i++)
    print_tree(roots[i], "", 1, 1);

  for (int i = 0; i < nproc; i++)
    free(procs[i].children);
  return 0;
}
