// L4: vfs — a virtual file system for NJU-OS (jyy OSLab 4).
//
// A small but real Unix-style VFS layered under the L3 system calls:
//
//   * inode / file abstractions and a per-process file-descriptor table;
//   * a mount table + path resolution (namei) spanning several file systems;
//   * ramfs   — a real read/write in-memory file system (files + directories),
//               mounted at "/", pre-populated with /bin (the user programs) and
//               an empty /tmp;
//   * devfs   — mounted at "/dev": /dev/zero, /dev/null, /dev/random, /dev/tty;
//   * procfs  — mounted at "/proc": /proc/meminfo, /proc/uptime, /proc/self.
//
// The L3 syscall layer (uproc.c) forwards open/read/write/close/lseek/fstat/
// dup/mkdir/link/unlink/chdir here; fork() dups the fd table, exit() closes it.
// exec() can load a program image straight out of /bin, so user programs are
// genuinely loaded from the file system.

#include <os.h>
#include <vfs.h>

#define NAMELEN   28
#define MAXDIRENT 32
#define NFILE     128       // system-wide open-file table
#define NMOUNT    8

// ------------------------------------------------------------------- inode
typedef struct inode {
  int  type;                 // I_DIR / I_FILE / I_DEV / I_PROC
  int  ino;
  int  size;                 // regular-file length
  char *data;                // regular-file contents (pmm buffer)
  int  cap;                  // capacity of data
  int  nlink;
  struct { char name[NAMELEN]; struct inode *ip; } ent[MAXDIRENT];
  int  nent;
  // device / procfs behaviour
  int (*dev_read)(struct inode *, int off, void *buf, int n);
  int (*dev_write)(struct inode *, int off, const void *buf, int n);
  int (*proc_gen)(struct inode *, char *buf, int max);
} inode_t;

typedef struct file {
  inode_t *ip;
  int off, readable, writable, ref;
} file_t;

typedef struct { char path[16]; inode_t *root; } mount_t;

static mount_t mounts[NMOUNT];
static int     nmount;
static file_t  ftable[NFILE];
static int     next_ino = 1;
static struct spinlock vlock;

// uproc accessors (defined in uproc.c)
int     uproc_nprog(void);
int     uproc_prog(int i, const char **name, const unsigned char **bin, unsigned *len);
int64_t uproc_uptime_ms(void);

// ------------------------------------------------------------------- inodes
static inode_t *ialloc(int type) {
  inode_t *ip = pmm->alloc(sizeof(inode_t));
  panic_on(!ip, "vfs: out of memory (inode)");
  memset(ip, 0, sizeof(*ip));
  ip->type = type; ip->ino = next_ino++; ip->nlink = 1;
  return ip;
}

static void igrow(inode_t *ip, int need) {
  if (need <= ip->cap) return;
  int newcap = ip->cap ? ip->cap : 256;
  while (newcap < need) newcap *= 2;
  char *nd = pmm->alloc(newcap);
  panic_on(!nd, "vfs: out of memory (file grow)");
  if (ip->data) { memcpy(nd, ip->data, ip->size); pmm->free(ip->data); }
  ip->data = nd; ip->cap = newcap;
}

static inode_t *dir_lookup(inode_t *dir, const char *name) {
  if (dir->type != I_DIR) return NULL;
  for (int i = 0; i < dir->nent; i++)
    if (strcmp(dir->ent[i].name, name) == 0) return dir->ent[i].ip;
  return NULL;
}

static void dir_add(inode_t *dir, const char *name, inode_t *ip) {
  panic_on(dir->nent >= MAXDIRENT, "vfs: directory full");
  strncpy(dir->ent[dir->nent].name, name, NAMELEN - 1);
  dir->ent[dir->nent].ip = ip;
  dir->nent++;
}

// ------------------------------------------------------------ path resolution
static const char *skipslash(const char *p) { while (*p == '/') p++; return p; }

static const char *nextcomp(const char *p, char *name) {
  p = skipslash(p);
  int i = 0;
  while (*p && *p != '/' && i < NAMELEN - 1) name[i++] = *p++;
  name[i] = 0;
  return p;
}

// Find the mount whose mount-point is the longest prefix of `path`.
static inode_t *resolve_mount(const char *path, const char **rest) {
  mount_t *best = &mounts[0];   // "/" is always mount 0
  int bestlen = 1;
  for (int i = 1; i < nmount; i++) {
    int l = strlen(mounts[i].path);
    if (strncmp(path, mounts[i].path, l) == 0 &&
        (path[l] == '/' || path[l] == 0) && l > bestlen) {
      best = &mounts[i]; bestlen = l;
    }
  }
  *rest = skipslash(path + bestlen);
  return best->root;
}

static inode_t *namei(const char *path) {
  const char *rest; inode_t *ip = resolve_mount(path, &rest);
  char name[NAMELEN];
  while (*rest) {
    rest = nextcomp(rest, name);
    if (name[0] == 0) break;
    if (ip->type != I_DIR) return NULL;
    ip = dir_lookup(ip, name);
    if (!ip) return NULL;
  }
  return ip;
}

// Return the parent directory of `path` and copy the final component into name.
static inode_t *namei_parent(const char *path, char *namebuf) {
  const char *rest; inode_t *ip = resolve_mount(path, &rest);
  rest = skipslash(rest);
  if (*rest == 0) return NULL;         // path is a mount root: no parent here
  char name[NAMELEN];
  for (;;) {
    rest = nextcomp(rest, name);
    const char *n = skipslash(rest);
    if (*n == 0) { strcpy(namebuf, name); return ip; }
    if (ip->type != I_DIR) return NULL;
    ip = dir_lookup(ip, name);
    if (!ip) return NULL;
    rest = n;
  }
}

// ------------------------------------------------------------ file table / fds
static file_t *falloc(void) {
  kmt->spin_lock(&vlock);
  for (int i = 0; i < NFILE; i++)
    if (ftable[i].ref == 0) {
      ftable[i] = (file_t){ .ref = 1 };
      kmt->spin_unlock(&vlock);
      return &ftable[i];
    }
  kmt->spin_unlock(&vlock);
  return NULL;
}

static int fdalloc(task_t *p, file_t *f) {
  for (int fd = 0; fd < PROC_NOFILE; fd++)
    if (p->ofile[fd] == NULL) { p->ofile[fd] = f; return fd; }
  return -1;
}

static file_t *getfile(task_t *p, int fd) {
  if (fd < 0 || fd >= PROC_NOFILE) return NULL;
  return (file_t *)p->ofile[fd];
}

static void fclose(file_t *f) {
  if (!f) return;
  kmt->spin_lock(&vlock);
  if (--f->ref <= 0) f->ip = NULL;   // ram inodes persist; only the handle frees
  kmt->spin_unlock(&vlock);
}

// ------------------------------------------------------------------- devices
static int dev_zero_read (inode_t *i, int o, void *b, int n)       { (void)i;(void)o; memset(b, 0, n); return n; }
static int dev_zero_write(inode_t *i, int o, const void *b, int n) { (void)i;(void)o;(void)b; return n; }
static int dev_null_read (inode_t *i, int o, void *b, int n)       { (void)i;(void)o;(void)b;(void)n; return 0; }

static unsigned rng = 0x2545F491;
static int dev_rand_read(inode_t *i, int o, void *b, int n) {
  (void)i; (void)o;
  unsigned char *p = b;
  for (int k = 0; k < n; k++) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    p[k] = (unsigned char)rng;
  }
  return n;
}
static int dev_tty_write(inode_t *i, int o, const void *b, int n) {
  (void)i; (void)o;
  const char *p = b; for (int k = 0; k < n; k++) putch(p[k]); return n;
}
static int dev_tty_read(inode_t *i, int o, void *b, int n) { (void)i;(void)o;(void)b;(void)n; return 0; } // headless: no input

// ------------------------------------------------------------------- procfs
static int proc_meminfo(inode_t *i, char *buf, int max) {
  (void)i;
  extern Area heap;
  long total = ((char *)heap.end - (char *)heap.start) / 1024;
  return snprintf(buf, max, "MemTotal: %ld kB\nPageSize: 4 kB\n", total);
}
static int proc_uptime(inode_t *i, char *buf, int max) {
  (void)i;
  int64_t ms = uproc_uptime_ms();
  return snprintf(buf, max, "%d.%02d\n", (int)(ms / 1000), (int)((ms % 1000) / 10));
}
static int proc_self(inode_t *i, char *buf, int max) {
  (void)i;
  return snprintf(buf, max, "vfs: ramfs(/) devfs(/dev) procfs(/proc)\n");
}

static inode_t *mkdev(int (*rd)(inode_t*,int,void*,int),
                      int (*wr)(inode_t*,int,const void*,int)) {
  inode_t *ip = ialloc(I_DEV);
  ip->dev_read = rd; ip->dev_write = wr;
  return ip;
}
static inode_t *mkproc(int (*gen)(inode_t*,char*,int)) {
  inode_t *ip = ialloc(I_PROC);
  ip->proc_gen = gen;
  return ip;
}

// ------------------------------------------------------------------- vfs API
void vfs_init(void) {
  kmt->spin_init(&vlock, "vfs");
  nmount = 0;

  // ramfs at "/"
  inode_t *root = ialloc(I_DIR);
  strcpy(mounts[nmount].path, "/"); mounts[nmount].root = root; nmount++;

  inode_t *tmp = ialloc(I_DIR); dir_add(root, "tmp", tmp);
  inode_t *bin = ialloc(I_DIR); dir_add(root, "bin", bin);
  dir_add(root, "dev", ialloc(I_DIR));    // placeholders so `/` listing shows them
  dir_add(root, "proc", ialloc(I_DIR));

  // populate /bin with the embedded user programs (as real files)
  for (int i = 0; i < uproc_nprog(); i++) {
    const char *name; const unsigned char *data; unsigned len;
    uproc_prog(i, &name, &data, &len);
    inode_t *f = ialloc(I_FILE);
    igrow(f, len);
    memcpy(f->data, data, len);
    f->size = len;
    dir_add(bin, name, f);
  }

  // devfs at "/dev"
  inode_t *dev = ialloc(I_DIR);
  dir_add(dev, "zero",   mkdev(dev_zero_read, dev_zero_write));
  dir_add(dev, "null",   mkdev(dev_null_read, dev_zero_write));
  dir_add(dev, "random", mkdev(dev_rand_read, dev_zero_write));
  dir_add(dev, "tty",    mkdev(dev_tty_read,  dev_tty_write));
  strcpy(mounts[nmount].path, "/dev"); mounts[nmount].root = dev; nmount++;

  // procfs at "/proc"
  inode_t *proc = ialloc(I_DIR);
  dir_add(proc, "meminfo", mkproc(proc_meminfo));
  dir_add(proc, "uptime",  mkproc(proc_uptime));
  dir_add(proc, "self",    mkproc(proc_self));
  strcpy(mounts[nmount].path, "/proc"); mounts[nmount].root = proc; nmount++;

  printf("vfs: mounted ramfs(/), devfs(/dev), procfs(/proc); /bin has %d programs\n",
         uproc_nprog());
}

int vfs_open(task_t *p, const char *path, int flags) {
  inode_t *ip = namei(path);
  if (!ip) {
    if (!(flags & O_CREATE)) return -1;
    char name[NAMELEN];
    inode_t *dir = namei_parent(path, name);
    if (!dir || dir->type != I_DIR) return -1;
    ip = ialloc(I_FILE);
    dir_add(dir, name, ip);
  }
  if (ip->type == I_DIR && (flags & (O_WRONLY | O_RDWR))) return -1;
  if ((flags & O_TRUNC) && ip->type == I_FILE) ip->size = 0;

  file_t *f = falloc();
  if (!f) return -1;
  f->ip = ip; f->off = 0;
  int acc = flags & 3;
  f->readable = (acc == O_RDONLY || acc == O_RDWR);
  f->writable = (acc == O_WRONLY || acc == O_RDWR);
  int fd = fdalloc(p, f);
  if (fd < 0) { f->ref = 0; return -1; }
  return fd;
}

int vfs_read(task_t *p, int fd, void *buf, int n) {
  file_t *f = getfile(p, fd);
  if (!f || !f->readable || n < 0) return -1;
  inode_t *ip = f->ip;
  if (ip->type == I_DEV) {
    int r = ip->dev_read(ip, f->off, buf, n); if (r > 0) f->off += r; return r;
  }
  if (ip->type == I_PROC) {
    char tmp[512]; int len = ip->proc_gen(ip, tmp, sizeof(tmp));
    if (f->off >= len) return 0;
    int c = (n < len - f->off) ? n : len - f->off;
    memcpy(buf, tmp + f->off, c); f->off += c; return c;
  }
  if (ip->type == I_FILE) {
    if (f->off >= ip->size) return 0;
    int c = (n < ip->size - f->off) ? n : ip->size - f->off;
    memcpy(buf, ip->data + f->off, c); f->off += c; return c;
  }
  return -1;
}

int vfs_write(task_t *p, int fd, const void *buf, int n) {
  file_t *f = getfile(p, fd);
  if (!f || !f->writable || n < 0) return -1;
  inode_t *ip = f->ip;
  if (ip->type == I_DEV)  { int r = ip->dev_write(ip, f->off, buf, n); if (r>0) f->off+=r; return r; }
  if (ip->type == I_FILE) {
    igrow(ip, f->off + n);
    memcpy(ip->data + f->off, buf, n);
    f->off += n;
    if (f->off > ip->size) ip->size = f->off;
    return n;
  }
  return -1;
}

int vfs_close(task_t *p, int fd) {
  file_t *f = getfile(p, fd);
  if (!f) return -1;
  p->ofile[fd] = NULL;
  fclose(f);
  return 0;
}

int vfs_lseek(task_t *p, int fd, int off, int whence) {
  file_t *f = getfile(p, fd);
  if (!f) return -1;
  int base = 0;
  if (whence == SEEK_SET) base = 0;
  else if (whence == SEEK_CUR) base = f->off;
  else if (whence == SEEK_END) base = (f->ip->type == I_FILE) ? f->ip->size : 0;
  else return -1;
  int np = base + off;
  if (np < 0) return -1;
  f->off = np;
  return np;
}

int vfs_fstat(task_t *p, int fd, void *st) {
  file_t *f = getfile(p, fd);
  if (!f || !st) return -1;
  struct ustat *s = st;
  s->type = f->ip->type;
  s->size = (f->ip->type == I_FILE) ? f->ip->size : 0;
  s->ino  = f->ip->ino;
  return 0;
}

int vfs_dup(task_t *p, int fd) {
  file_t *f = getfile(p, fd);
  if (!f) return -1;
  int nfd = fdalloc(p, f);
  if (nfd < 0) return -1;
  kmt->spin_lock(&vlock); f->ref++; kmt->spin_unlock(&vlock);
  return nfd;
}

int vfs_mkdir(task_t *p, const char *path) {
  (void)p;
  if (namei(path)) return -1;
  char name[NAMELEN];
  inode_t *dir = namei_parent(path, name);
  if (!dir || dir->type != I_DIR) return -1;
  dir_add(dir, name, ialloc(I_DIR));
  return 0;
}

int vfs_link(task_t *p, const char *a, const char *b) {
  (void)p;
  inode_t *ip = namei(a);
  if (!ip || ip->type == I_DIR) return -1;
  char name[NAMELEN];
  inode_t *dir = namei_parent(b, name);
  if (!dir) return -1;
  dir_add(dir, name, ip);
  ip->nlink++;
  return 0;
}

int vfs_unlink(task_t *p, const char *path) {
  (void)p;
  char name[NAMELEN];
  inode_t *dir = namei_parent(path, name);
  if (!dir) return -1;
  for (int i = 0; i < dir->nent; i++)
    if (strcmp(dir->ent[i].name, name) == 0) {
      dir->ent[i].ip->nlink--;
      dir->ent[i] = dir->ent[--dir->nent];   // remove entry
      return 0;
    }
  return -1;
}

int vfs_chdir(task_t *p, const char *path) {
  inode_t *ip = namei(path);
  if (!ip || ip->type != I_DIR) return -1;
  strncpy(p->cwd, path, sizeof(p->cwd) - 1);
  return 0;
}

// fork(): child shares the parent's open files (dup the handles).
void vfs_fork(task_t *parent, task_t *child) {
  for (int fd = 0; fd < PROC_NOFILE; fd++) {
    file_t *f = (file_t *)parent->ofile[fd];
    child->ofile[fd] = f;
    if (f) { kmt->spin_lock(&vlock); f->ref++; kmt->spin_unlock(&vlock); }
  }
}

void vfs_close_all(task_t *p) {
  for (int fd = 0; fd < PROC_NOFILE; fd++)
    if (p->ofile[fd]) { fclose((file_t *)p->ofile[fd]); p->ofile[fd] = NULL; }
}

// Give a new process stdin/stdout/stderr bound to /dev/tty.
void vfs_setup_stdio(task_t *p) {
  inode_t *tty = namei("/dev/tty");
  if (!tty) return;
  for (int fd = 0; fd < 3; fd++) {
    file_t *f = falloc();
    if (!f) return;
    f->ip = tty; f->off = 0;
    f->readable = (fd == 0); f->writable = (fd != 0);
    p->ofile[fd] = f;
  }
}

// exec() support: hand back a /bin program image so uproc can load it.
const unsigned char *vfs_prog_data(const char *name, unsigned *len) {
  char path[64] = "/bin/";
  strncpy(path + 5, name, sizeof(path) - 6);
  inode_t *ip = namei(path);
  if (!ip || ip->type != I_FILE) return NULL;
  *len = ip->size;
  return (const unsigned char *)ip->data;
}
