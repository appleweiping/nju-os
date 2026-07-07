#!/bin/bash
# Build a FAT32 test image with BMP files, then test frecov recovery.
set -e
LAB=/mnt/d/Project/operating-systems/nju-os/minilabs/frecov
WORK=/tmp/frecov-test
IMG=$WORK/fsrecov.img
rm -rf "$WORK"; mkdir -p "$WORK/mnt" "$WORK/bmps"

# Need dosfstools (mkfs.fat) + a loopback-free path. We'll build the FS with
# mtools if loop mount is unavailable (WSL often lacks loop). Prefer mtools.
command -v mkfs.fat >/dev/null || { echo "installing dosfstools mtools..."; sudo apt-get install -y -q dosfstools mtools >/dev/null 2>&1; }
command -v mcopy >/dev/null || sudo apt-get install -y -q mtools >/dev/null 2>&1

# 1) Generate a handful of REAL BMP files (24-bit, various small sizes).
python3 - "$WORK/bmps" << 'PY'
import struct, sys, os, random
outdir = sys.argv[1]
random.seed(1234)
def make_bmp(path, w, h):
    row = (w*3 + 3)//4*4
    pixels = bytearray()
    for y in range(h):
        line = bytearray()
        for x in range(w):
            line += bytes((random.randint(0,255), (x*7+y*3)%256, (x+y)%256))
        line += b'\x00'*(row - w*3)
        pixels += line
    size = 54 + len(pixels)
    hdr = b'BM' + struct.pack('<IHHI', size, 0, 0, 54)
    dib = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(pixels), 2835,2835,0,0)
    with open(path,'wb') as f:
        f.write(hdr+dib+pixels)
sizes = [(16,16),(20,24),(32,32),(40,30),(48,48)]
for i,(w,h) in enumerate(sizes):
    make_bmp(os.path.join(outdir, f"image_{i}.bmp"), w, h)
print("generated", len(sizes), "BMPs")
PY

echo "=== original sha1sums ==="
( cd "$WORK/bmps" && sha1sum *.bmp | sort ) | tee "$WORK/originals.sha1"

# 2) Make a 64 MiB FAT32 image and populate it via mtools (no root/loop needed).
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mkfs.fat -F 32 -S 512 -s 8 "$IMG" >/dev/null   # 512B/sector, 8 sec/clus = 4KiB clusters
export MTOOLS_SKIP_CHECK=1
for f in "$WORK"/bmps/*.bmp; do
  mcopy -i "$IMG" "$f" ::/$(basename "$f")
done
echo "=== files in image (mdir) ==="
mdir -i "$IMG" ::/ | head -20

# 3) Build frecov and run it on the LIVE image (entries present, LFNs intact).
cd "$LAB"
make frecov-64 >/dev/null 2>&1
echo "=== frecov on live image ==="
./frecov-64 "$IMG" | sort | tee "$WORK/recovered_live.sha1"

# 4) Compare recovered sha1s (col1) against originals (col1). Filenames differ
#    only if LFN decode failed; we compare the SET of hashes.
echo "=== comparison (live) ==="
cut -d' ' -f1 "$WORK/originals.sha1" | sort > "$WORK/orig_hashes"
cut -d' ' -f1 "$WORK/recovered_live.sha1" | sort -u > "$WORK/rec_hashes"
echo "original hashes: $(wc -l < "$WORK/orig_hashes")"
echo "recovered hashes: $(wc -l < "$WORK/rec_hashes")"
MATCH=$(comm -12 "$WORK/orig_hashes" "$WORK/rec_hashes" | wc -l)
echo "MATCHING hashes (live): $MATCH / $(wc -l < "$WORK/orig_hashes")"

# 5) Now DELETE the files (mdel), then run frecov on the deleted image.
for f in "$WORK"/bmps/*.bmp; do
  mdel -i "$IMG" ::/$(basename "$f") 2>/dev/null || true
done
echo "=== files after delete (mdir) ==="
mdir -i "$IMG" ::/ 2>/dev/null | head -8 || echo "(dir now empty)"
echo "=== frecov on DELETED image ==="
./frecov-64 "$IMG" | sort | tee "$WORK/recovered_del.sha1"
cut -d' ' -f1 "$WORK/recovered_del.sha1" | sort -u > "$WORK/rec_del_hashes"
MATCHD=$(comm -12 "$WORK/orig_hashes" "$WORK/rec_del_hashes" | wc -l)
echo "MATCHING hashes (after delete): $MATCHD / $(wc -l < "$WORK/orig_hashes")"
