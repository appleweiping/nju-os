#include <kernel.h>
#include <klib.h>

int main() {
  // NOTE: ioe_init() is intentionally NOT called.  On the AbstractMachine
  // native backend it brings up the SDL I/O stack (GPU window / input thread),
  // whose event thread calls halt() on SDL_QUIT -- which fires immediately on a
  // headless host and aborts the kernel at a random time.  This OS runs fully
  // headless: console output uses putch()/serial, timing uses the timer IRQ,
  // and L4's /dev is a self-contained devfs.  No AM I/O device is used.
  cte_init(os->trap);
  os->init();
  mpe_init(os->run);
  return 1;
}
