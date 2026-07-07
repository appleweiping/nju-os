NAME           := kernel
SRCS           := framework/main.c $(shell find -L ./src/ -name "*.c")
INC_PATH       := include/ framework/

export AM_HOME := $(PWD)/../abstract-machine
ifeq ($(ARCH),)
export ARCH    := x86_64-qemu
endif

# Optional built-in self-tests, e.g. `make ARCH=native TEST=pmm run`.
# Each TEST=<name> adds -D<NAME>_TEST to CFLAGS so the corresponding module's
# stress test is compiled in. Left empty for a normal boot.
ifneq ($(TEST),)
CFLAGS += -D$(shell echo $(TEST) | tr a-z A-Z)_TEST
endif

include $(AM_HOME)/Makefile
include ../Makefile.lab
image: git
