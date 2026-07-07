// Renamed _init -> _initcode: on the AbstractMachine *native* backend the
// kernel links against the host libc, whose crti.o already defines a `_init`
// symbol.  Keeping the array named `_init` caused a "multiple definition of
// `_init`" link error, so it is renamed here (and referenced as _initcode in
// uproc.c).
unsigned char _initcode[] = {
  0xe8, 0x0c, 0x00, 0x00, 0x00, 0x48, 0x89, 0xc7, 0x48, 0xc7, 0xc0, 0x03,
  0x00, 0x00, 0x00, 0xcd, 0x80, 0xf3, 0x0f, 0x1e, 0xfa, 0xb8, 0x00, 0x00,
  0x00, 0x00, 0xc3
};
unsigned int _initcode_len = 27;
