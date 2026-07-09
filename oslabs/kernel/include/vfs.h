#ifndef VFS_H
#define VFS_H

// L4 vfs — shared types.  open() flags and lseek() whence are kept in sync with
// user/ulib.h.

#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREATE 0x200
#define O_TRUNC  0x400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// inode kinds
#define I_DIR  1     // directory
#define I_FILE 2     // regular ram file
#define I_DEV  3     // device (devfs) — read/write via ops
#define I_PROC 4     // procfs node — content generated on read

// stat structure returned by fstat(); must match the user-side layout
struct ustat {
  int type;
  int size;
  int ino;
};

#endif
