#!/bin/bash
# Verify frecov's LFN (long file name) recovery path with long-named BMPs.
set -e
LAB=/mnt/d/Project/operating-systems/nju-os/minilabs/frecov
WORK=/tmp/frecov-lfn
IMG=$WORK/lfn.img
rm -rf "$WORK"; mkdir -p "$WORK/bmps"
export MTOOLS_SKIP_CHECK=1

python3 - "$WORK/bmps" << 'PY'
import struct, sys, os, random
outdir=sys.argv[1]; random.seed(99)
def make_bmp(path,w,h):
    row=(w*3+3)//4*4; px=bytearray()
    for y in range(h):
        ln=bytearray()
        for x in range(w): ln+=bytes((random.randint(0,255),(x+y)%256,(x*3)%256))
        ln+=b'\x00'*(row-w*3); px+=ln
    size=54+len(px)
    with open(path,'wb') as f:
        f.write(b'BM'+struct.pack('<IHHI',size,0,0,54)+
                struct.pack('<IiiHHIIiiII',40,w,h,1,24,0,len(px),2835,2835,0,0)+px)
# long file names -> force LFN entries
make_bmp(os.path.join(outdir,"a_very_long_picture_name_one.bmp"),24,24)
make_bmp(os.path.join(outdir,"another_long_photograph_two.bmp"),30,20)
print("made 2 long-named BMPs")
PY

echo "=== originals ==="
( cd "$WORK/bmps" && sha1sum *.bmp )

dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
mkfs.fat -F 32 -S 512 -s 8 "$IMG" >/dev/null 2>&1
for f in "$WORK"/bmps/*.bmp; do mcopy -i "$IMG" "$f" ::/"$(basename "$f")"; done

cd "$LAB"
echo "=== frecov on LFN image (names must be full long names) ==="
./frecov-64 "$IMG" | sort

echo "=== match check ==="
( cd "$WORK/bmps" && sha1sum *.bmp | cut -d' ' -f1 | sort ) > "$WORK/orig"
./frecov-64 "$IMG" | cut -d' ' -f1 | sort -u > "$WORK/rec"
echo "matching: $(comm -12 "$WORK/orig" "$WORK/rec" | wc -l) / $(wc -l < "$WORK/orig")"
