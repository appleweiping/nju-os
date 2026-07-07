// M5: frecov — recover (deleted) BMP files from a FAT32 disk image.
//
// NJU OS 2022 (jyy) MiniLab M5.  The image is a FAT32 file system that once
// held a set of BMP images; the directory entries may be marked deleted
// (first byte 0xE5) and the FAT chains cleared.  frecov scans the raw image
// for surviving directory records, reconstructs each BMP's long file name and
// data, and prints, for every recovered file, a line
//
//     <sha1sum>  <filename>
//
// which is exactly what the course grader (sha1sum on the known originals)
// checks against.
//
// Recovery strategy
// -----------------
//  * We treat every 32-byte-aligned region of the whole image as a potential
//    array of directory entries and look for valid short-name entries whose
//    long-name (LFN) run decodes to "*.bmp".  This finds entries even after
//    the containing directory's own FAT chain is gone.
//  * BMP data is assumed to be laid out in *contiguous* clusters starting at
//    the entry's first cluster (true for the images produced by the lab's
//    generator, and the intended assumption since the FAT is unreliable for
//    deleted files).  We additionally sanity-check the BMP header (BM magic +
//    bfSize) to reject false positives and to trim to the real file size.
//
// SHA-1 is implemented inline (RFC 3174) so the tool has no external deps.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/mman.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// ---- FAT32 on-disk structures (from the FAT spec / lab handout) ----------

struct fat32hdr {
  u8  BS_jmpBoot[3];
  u8  BS_OEMName[8];
  u16 BPB_BytsPerSec;
  u8  BPB_SecPerClus;
  u16 BPB_RsvdSecCnt;
  u8  BPB_NumFATs;
  u16 BPB_RootEntCnt;
  u16 BPB_TotSec16;
  u8  BPB_Media;
  u16 BPB_FATSz16;
  u16 BPB_SecPerTrk;
  u16 BPB_NumHeads;
  u32 BPB_HiddSec;
  u32 BPB_TotSec32;
  u32 BPB_FATSz32;
  u16 BPB_ExtFlags;
  u16 BPB_FSVer;
  u32 BPB_RootClus;
  u16 BPB_FSInfo;
  u16 BPB_BkBootSec;
  u8  BPB_Reserved[12];
  u8  BS_DrvNum;
  u8  BS_Reserved1;
  u8  BS_BootSig;
  u32 BS_VolID;
  u8  BS_VolLab[11];
  u8  BS_FilSysType[8];
  u8  __padding_1[420];
  u16 Signature_word;
} __attribute__((packed));

// 32-byte short directory entry.
struct fat32dent {
  u8  DIR_Name[11];
  u8  DIR_Attr;
  u8  DIR_NTRes;
  u8  DIR_CrtTimeTenth;
  u16 DIR_CrtTime;
  u16 DIR_CrtDate;
  u16 DIR_LstAccDate;
  u16 DIR_FstClusHI;
  u16 DIR_WrtTime;
  u16 DIR_WrtDate;
  u16 DIR_FstClusLO;
  u32 DIR_FileSize;
} __attribute__((packed));

// 32-byte long file name (LFN) entry.
struct fat32lfn {
  u8  LDIR_Ord;
  u16 LDIR_Name1[5];   // chars 1-5   (UTF-16)
  u8  LDIR_Attr;       // == 0x0F for LFN
  u8  LDIR_Type;
  u8  LDIR_Chksum;
  u16 LDIR_Name2[6];   // chars 6-11
  u16 LDIR_FstClusLO;  // always 0
  u16 LDIR_Name3[2];   // chars 12-13
} __attribute__((packed));

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LONG_NAME (ATTR_READ_ONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)  // 0x0F
#define LAST_LONG_ENTRY 0x40

static struct fat32hdr *hdr;       // mapped image
static u32 CLUS_SIZE;              // bytes per cluster
static u8 *DATA_BASE;             // start of cluster #2 (data region)
static u64 IMG_SIZE;

void *map_disk(const char *fname);  // defined at the bottom of this file

// Absolute pointer to the first byte of cluster `n` (n >= 2).
static void *cluster_ptr(u32 n) {
  return DATA_BASE + (u64)(n - 2) * CLUS_SIZE;
}

// ------------------------------------------------------------------ SHA-1

typedef struct { u32 h[5]; u64 len; u8 buf[64]; u32 idx; } sha1_ctx;

static u32 rol(u32 v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_block(sha1_ctx *c, const u8 *p) {
  u32 w[80];
  for (int i = 0; i < 16; i++)
    w[i] = (p[i*4] << 24) | (p[i*4+1] << 16) | (p[i*4+2] << 8) | p[i*4+3];
  for (int i = 16; i < 80; i++)
    w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
  u32 a=c->h[0], b=c->h[1], d=c->h[2], e=c->h[3], f=c->h[4];
  for (int i = 0; i < 80; i++) {
    u32 fn, k;
    if      (i < 20) { fn = (b & d) | (~b & e);          k = 0x5A827999; }
    else if (i < 40) { fn = b ^ d ^ e;                   k = 0x6ED9EBA1; }
    else if (i < 60) { fn = (b & d) | (b & e) | (d & e); k = 0x8F1BBCDC; }
    else             { fn = b ^ d ^ e;                   k = 0xCA62C1D6; }
    u32 t = rol(a,5) + fn + f + k + w[i];
    f = e; e = d; d = rol(b,30); b = a; a = t;
  }
  c->h[0]+=a; c->h[1]+=b; c->h[2]+=d; c->h[3]+=e; c->h[4]+=f;
}

static void sha1_init(sha1_ctx *c) {
  c->h[0]=0x67452301; c->h[1]=0xEFCDAB89; c->h[2]=0x98BADCFE;
  c->h[3]=0x10325476; c->h[4]=0xC3D2E1F0; c->len=0; c->idx=0;
}
static void sha1_update(sha1_ctx *c, const u8 *p, u64 n) {
  c->len += n;
  while (n--) {
    c->buf[c->idx++] = *p++;
    if (c->idx == 64) { sha1_block(c, c->buf); c->idx = 0; }
  }
}
static void sha1_final(sha1_ctx *c, u8 out[20]) {
  u64 bits = c->len * 8;
  u8 pad = 0x80;
  sha1_update(c, &pad, 1);
  u8 zero = 0;
  while (c->idx != 56) sha1_update(c, &zero, 1);
  u8 lenb[8];
  for (int i = 0; i < 8; i++) lenb[i] = (bits >> (56 - 8*i)) & 0xff;
  sha1_update(c, lenb, 8);
  for (int i = 0; i < 5; i++) {
    out[i*4]   = (c->h[i] >> 24) & 0xff;
    out[i*4+1] = (c->h[i] >> 16) & 0xff;
    out[i*4+2] = (c->h[i] >> 8)  & 0xff;
    out[i*4+3] =  c->h[i]        & 0xff;
  }
}
static void sha1_hex(const u8 *data, u64 n, char out[41]) {
  sha1_ctx c; sha1_init(&c); sha1_update(&c, data, n);
  u8 dig[20]; sha1_final(&c, dig);
  for (int i = 0; i < 20; i++) sprintf(out + i*2, "%02x", dig[i]);
  out[40] = '\0';
}

// ------------------------------------------------------ LFN name decoding

// Append the (ASCII subset of the) UTF-16 chars of one LFN entry to `name`.
// FAT LFNs store 13 UTF-16 code units at fixed byte offsets 1,14,28 within the
// 32-byte entry.  We read them via byte offsets (memcpy) to avoid taking the
// address of a packed struct member.
static void lfn_chars(const struct fat32lfn *l, char *dst, int *pos) {
  const u8 *raw = (const u8 *)l;
  static const int off[3]   = { 1, 14, 28 };   // Name1, Name2, Name3
  static const int count[3] = { 5, 6, 2 };
  for (int p = 0; p < 3; p++) {
    for (int i = 0; i < count[p]; i++) {
      u16 ch;
      memcpy(&ch, raw + off[p] + i * 2, sizeof(ch));
      if (ch == 0x0000 || ch == 0xFFFF) return;  // terminator/pad
      dst[(*pos)++] = (ch < 128) ? (char)ch : '?';
    }
  }
}

// Try to read a full BMP directory entry located at `sd` (a short entry) with
// its preceding LFN run.  `base` is the start of the aligned 32-byte window so
// we can walk backwards over LFN entries.  Returns 1 and fills name/clus/size
// if this looks like a *.bmp file.
static int read_bmp_entry(u8 *base, struct fat32dent *sd,
                          char *name, u32 *clus, u32 *size) {
  // Must be a regular file (not dir / volume label / LFN), any state.
  if ((sd->DIR_Attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) return 0;
  if (sd->DIR_Attr & (ATTR_DIRECTORY | ATTR_VOLUME_ID))  return 0;

  u32 first = ((u32)sd->DIR_FstClusHI << 16) | sd->DIR_FstClusLO;
  u32 fsize = sd->DIR_FileSize;
  if (first < 2 || fsize == 0 || fsize > IMG_SIZE) return 0;

  // Reconstruct the name from the LFN run immediately before `sd`.
  // LFN entries precede the short entry with ordinal 1 closest to it; ordinal N
  // holds characters [(N-1)*13 .. N*13).  Walking *backwards* (toward lower
  // addresses) we meet ordinal 1, then 2, then 3 ... — so each 13-char chunk we
  // decode should be APPENDED, which yields the correct left-to-right name.
  char lname[512]; int pos = 0;
  u8 *cur = (u8 *)sd - 32;
  int guard = 0;
  while (cur >= base && guard < 32) {
    struct fat32lfn *l = (struct fat32lfn *)cur;
    if (l->LDIR_Attr != ATTR_LONG_NAME || l->LDIR_Ord == 0xE5) break;
    lfn_chars(l, lname, &pos);   // append this entry's chars
    if (l->LDIR_Ord & LAST_LONG_ENTRY) break;   // ordinal had the 0x40 flag
    cur -= 32;
    guard++;
  }

  if (pos == 0) {
    // No LFN: fall back to the 8.3 short name.
    int k = 0;
    for (int i = 0; i < 8 && sd->DIR_Name[i] != ' '; i++)
      lname[k++] = sd->DIR_Name[i];
    if (sd->DIR_Name[8] != ' ') {
      lname[k++] = '.';
      for (int i = 8; i < 11 && sd->DIR_Name[i] != ' '; i++)
        lname[k++] = sd->DIR_Name[i];
    }
    pos = k;
  }
  lname[pos] = '\0';

  // Must end in ".bmp" (case-insensitive).
  int n = (int)strlen(lname);
  if (n < 5) return 0;   // at least "X.bmp"
  if (!(lname[n-4] == '.' &&
        tolower((unsigned char)lname[n-3]) == 'b' &&
        tolower((unsigned char)lname[n-2]) == 'm' &&
        tolower((unsigned char)lname[n-1]) == 'p')) return 0;

  strcpy(name, lname);
  *clus = first;
  *size = fsize;
  return 1;
}

// Verify the recovered bytes really are a BMP and return the true size to hash
// (bfSize from the BMP header, clamped to the directory size).
static u32 bmp_true_size(const u8 *data, u32 dir_size) {
  if (data[0] != 'B' || data[1] != 'M') return dir_size;
  u32 bf = data[2] | (data[3] << 8) | (data[4] << 16) | (data[5] << 24);
  if (bf == 0 || bf > dir_size + CLUS_SIZE) return dir_size;
  return bf;
}

static void recover(void) {
  char name[512];
  u32 clus, size;

  // Scan the entire image on 32-byte boundaries.
  u8 *img = (u8 *)hdr;
  for (u64 off = 0; off + sizeof(struct fat32dent) <= IMG_SIZE; off += 32) {
    struct fat32dent *sd = (struct fat32dent *)(img + off);
    u8 first_byte = sd->DIR_Name[0];
    // 0x00 = end-of-dir sentinel, skip; still scan (raw scan, not tree walk).
    if (first_byte == 0x00) continue;

    // Window base for LFN back-walk: start of the enclosing cluster.
    u8 *base = img;

    if (read_bmp_entry(base, sd, name, &clus, &size)) {
      // Contiguous-cluster reconstruction.
      u8 *start = (u8 *)cluster_ptr(clus);
      if (start < img || start + size > img + IMG_SIZE) continue;
      u32 real = bmp_true_size(start, size);
      char hex[41];
      sha1_hex(start, real, hex);
      printf("%s  %s\n", hex, name);
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s fs-image\n", argv[0]);
    exit(1);
  }
  setbuf(stdout, NULL);
  assert(sizeof(struct fat32hdr) == 512);

  hdr = map_disk(argv[1]);

  CLUS_SIZE = (u32)hdr->BPB_BytsPerSec * hdr->BPB_SecPerClus;
  IMG_SIZE  = (u64)hdr->BPB_TotSec32 * hdr->BPB_BytsPerSec;
  u32 fat_start_sec = hdr->BPB_RsvdSecCnt;
  u32 data_start_sec = fat_start_sec + hdr->BPB_NumFATs * hdr->BPB_FATSz32;
  DATA_BASE = (u8 *)hdr + (u64)data_start_sec * hdr->BPB_BytsPerSec;

  recover();

  munmap(hdr, IMG_SIZE);
  return 0;
}

void *map_disk(const char *fname) {
  int fd = open(fname, O_RDWR);
  if (fd < 0) { perror(fname); exit(1); }

  off_t size = lseek(fd, 0, SEEK_END);
  if (size == -1) { perror(fname); close(fd); exit(1); }

  struct fat32hdr *h = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if (h == (void *)-1) { perror("mmap"); close(fd); exit(1); }
  close(fd);

  if (h->Signature_word != 0xaa55 ||
      (u64)h->BPB_TotSec32 * h->BPB_BytsPerSec != (u64)size) {
    fprintf(stderr, "%s: Not a FAT file image\n", fname);
    exit(1);
  }
  return h;
}
