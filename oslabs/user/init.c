#include "ulib.h"

// init: the first user process.  It exercises the L3 system-call interface
// (getpid/uptime/fork/exit/wait/exec/mmap) and, when the L4 vfs is present,
// the file system (open/read/write/close/lseek on a real file, plus /dev and
// /proc).  Every check prints a [ok]/[FAIL] line; the final status is 0 iff all
// pass, which becomes the machine's exit code.

static int fails = 0;
static void check(const char *what, int cond) {
  printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) fails++;
}

// ------------------------------------------------------------------- L3 tests
static void test_l3(void) {
  printf("== L3 uproc: user processes & syscalls ==\n");

  check("getpid() returns 1 for init", getpid() == 1);

  int64_t t0 = uptime();
  check("uptime() is monotonic", uptime() >= t0);

  // fork + exit + wait with an explicit status
  int pid = fork();
  if (pid == 0) {
    printf("  child: pid=%d, parent alive\n", getpid());
    _exit_(42);
  }
  check("fork() returns a child pid > 1", pid > 1);
  int status = -1;
  int wpid = wait(&status);
  check("wait() reaps the child", wpid == pid);
  check("wait() reports exit status 42", status == 42);

  // several concurrent children, each exits with its own code
  int kids[4];
  for (int i = 0; i < 4; i++) {
    int p = fork();
    if (p == 0) {
      for (volatile int s = 0; s < 200000; s++) ;   // do a little work
      _exit_(100 + getpid());
    }
    kids[i] = p;
  }
  int reaped = 0;
  for (int i = 0; i < 4; i++) {
    int st;
    int w = wait(&st);
    if (w > 0) reaped++;
  }
  check("wait() reaps all 4 children", reaped == 4);

  // exec: replace a child image with the 'hello' program
  int ep = fork();
  if (ep == 0) {
    char *argv[] = { "hello", 0 };
    exec("/bin/hello", argv);
    _exit_(7);              // only reached if exec failed
  }
  int est;
  wait(&est);
  check("exec() ran /bin/hello (exit 0)", est == 0);

  // mmap a fresh page and use it
  int *page = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE);
  check("mmap() returns a usable address", page != (int *)-1);
  if (page != (int *)-1) {
    page[0] = 0xdeadbeef; page[1023] = 0x1234;
    check("mmap page holds written data", page[0] == (int)0xdeadbeef && page[1023] == 0x1234);
  }

  // sleep() blocks against the timer for ~1s (no busy-wait) then returns
  int64_t before = uptime();
  sleep(1);
  int64_t slept = uptime() - before;
  check("sleep(1) blocks ~1 second", slept >= 900 && slept <= 3000);
}

// ------------------------------------------------------------------- L4 tests
static int has_vfs(void) {
  // open() of an always-present device tells us the vfs is wired up.
  int fd = open("/dev/zero", O_RDONLY);
  if (fd < 0) return 0;
  close(fd);
  return 1;
}

static void test_l4(void) {
  printf("== L4 vfs: files, /dev and /proc ==\n");

  // 1) a real file in the ram file system: write, seek, read back
  int fd = open("/tmp/hello.txt", O_CREATE | O_RDWR);
  check("open(/tmp/hello.txt, O_CREATE|O_RDWR)", fd >= 0);
  const char *msg = "NJU-OS vfs works!";
  int n = write(fd, msg, strlen(msg));
  check("write() returns byte count", n == (int)strlen(msg));
  lseek(fd, 0, SEEK_SET);
  char buf[64];
  int r = read(fd, buf, sizeof(buf) - 1);
  buf[r > 0 ? r : 0] = 0;
  check("read() returns the written bytes", r == (int)strlen(msg));
  int same = 1;
  for (int i = 0; i < r; i++) if (buf[i] != msg[i]) same = 0;
  check("file contents round-trip", same);
  printf("    /tmp/hello.txt = \"%s\"\n", buf);
  close(fd);

  // 2) /dev/zero yields zero bytes
  fd = open("/dev/zero", O_RDONLY);
  int z = read(fd, buf, 16), zeros = 1;
  for (int i = 0; i < z; i++) if (buf[i] != 0) zeros = 0;
  check("/dev/zero reads all-zero bytes", z == 16 && zeros);
  close(fd);

  // 3) /dev/random yields (probably) non-constant bytes
  fd = open("/dev/random", O_RDONLY);
  unsigned char r1[8], r2[8];
  read(fd, r1, 8); read(fd, r2, 8);
  int differ = 0;
  for (int i = 0; i < 8; i++) if (r1[i] != r2[i]) differ = 1;
  check("/dev/random produces varying bytes", differ);
  close(fd);

  // 4) /dev/null swallows writes
  fd = open("/dev/null", O_WRONLY);
  check("write to /dev/null succeeds", write(fd, "discard", 7) == 7);
  close(fd);

  // 5) /proc: the kernel exposes process info
  fd = open("/proc/meminfo", O_RDONLY);
  check("open(/proc/meminfo)", fd >= 0);
  if (fd >= 0) {
    r = read(fd, buf, sizeof(buf) - 1);
    buf[r > 0 ? r : 0] = 0;
    check("/proc/meminfo is non-empty", r > 0);
    printf("    /proc/meminfo: %s", buf);
    close(fd);
  }

  // 6) mkdir + a second file, then read it in a child (shared fs across procs)
  mkdir("/tmp/sub");
  fd = open("/tmp/sub/data", O_CREATE | O_WRONLY);
  write(fd, "42", 2);
  close(fd);
  int cp = fork();
  if (cp == 0) {
    int cfd = open("/tmp/sub/data", O_RDONLY);
    char cb[8]; int cn = read(cfd, cb, 8);
    _exit_(cn == 2 && cb[0] == '4' && cb[1] == '2' ? 0 : 1);
  }
  int cs; wait(&cs);
  check("child reads file created by parent", cs == 0);
}

int main() {
  printf("\n[init] NJU-OS user space up. pid=%d\n\n", getpid());

  test_l3();
  if (has_vfs()) test_l4();
  else printf("== L4 vfs not present (L3-only build) ==\n");

  printf("\n==================================\n");
  if (fails == 0) printf("ALL TESTS PASSED\n");
  else            printf("%d TEST(S) FAILED\n", fails);
  printf("==================================\n");
  return fails;
}
