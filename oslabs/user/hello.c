#include "ulib.h"

// A trivial program that init loads with exec() to demonstrate program loading.
int main() {
  printf("  hello: I am a separate program, pid=%d\n", getpid());
  return 0;
}
