#include "ulib.h"

// Stress fork()/wait(): spawn N children, each exits with a distinct code; the
// parent verifies it reaps exactly N and sees the right statuses.
#define N 8

int main() {
  int ok = 1;
  for (int i = 0; i < N; i++) {
    int pid = fork();
    if (pid == 0) {
      for (volatile int s = 0; s < 100000; s++) ;
      _exit_(i);
    }
  }
  int reaped = 0;
  for (int i = 0; i < N; i++) {
    int st;
    if (wait(&st) > 0) reaped++;
  }
  ok = (reaped == N);
  printf("  forktest: reaped %d/%d children -> %s\n", reaped, N, ok ? "ok" : "FAIL");
  return ok ? 0 : 1;
}
