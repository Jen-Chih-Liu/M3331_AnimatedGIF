import re, sys

path = r".\emWin_GIFDecoder_CS_case5\Application\nuvoton.c"
text = open(path, "r", encoding="utf-8", errors="ignore").read()
# extract hex bytes
bytes_list = [int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{2})", text)]
data = bytes(bytes_list)
print("Extracted bytes:", len(data))

p = 0
def u16(i): return data[i] | (data[i+1] << 8)

sig = data[0:6].decode("ascii", "replace")
w = u16(6); h = u16(8)
packed = data[10]
gct_flag = (packed >> 7) & 1
color_res = ((packed >> 4) & 7) + 1
sort = (packed >> 3) & 1
gct_size = 2 ** (((packed) & 7) + 1)
bg = data[11]; aspect = data[12]
print(f"Signature: {sig}")
print(f"Logical screen: {w} x {h}")
print(f"Global Color Table: present={gct_flag} size={gct_size} colors  (palette {gct_size*3} bytes)")
print(f"Color resolution bits/pixel: {color_res}")
print(f"Background index: {bg}  Aspect: {aspect}")

p = 13
if gct_flag:
    p += gct_size * 3

frames = 0
delays = []
cur_delay = None
transparent_frames = 0
img_data_sizes = []
loop_count = None

while p < len(data):
    b = data[p]
    if b == 0x21:  # extension
        label = data[p+1]
        p += 2
        if label == 0xF9:  # Graphic Control Extension
            # block size (4), packed, delay(2), transparent idx, terminator
            bsize = data[p]
            gpacked = data[p+1]
            delay = u16(p+2)
            tidx = data[p+4]
            cur_delay = delay
            if gpacked & 1:
                transparent_frames += 1
            p += 1 + bsize + 1  # size byte + block + terminator
        elif label == 0xFF:  # Application Extension (loop)
            bsize = data[p]
            app = data[p+1:p+1+bsize]
            p += 1 + bsize
            # sub-blocks
            while data[p] != 0:
                sb = data[p]
                if app[:11] == b"NETSCAPE2.0" and sb >= 3:
                    loop_count = u16(p+2)
                p += 1 + sb
            p += 1
        else:
            # skip sub-blocks
            p += 1
            while p < len(data) and data[p] != 0:
                p += 1 + data[p]
            p += 1
    elif b == 0x2C:  # image descriptor
        frames += 1
        ix = u16(p+1); iy = u16(p+3); iw = u16(p+5); ih = u16(p+7)
        lpacked = data[p+9]
        lct = (lpacked >> 7) & 1
        lct_size = 2 ** ((lpacked & 7) + 1) if lct else 0
        p += 10
        if lct:
            p += lct_size * 3
        # LZW min code size
        p += 1
        start = p
        # image data sub-blocks
        while p < len(data) and data[p] != 0:
            p += 1 + data[p]
        p += 1
        img_data_sizes.append(p - start)
        delays.append(cur_delay if cur_delay is not None else 0)
        if frames <= 3:
            print(f"  frame {frames}: pos=({ix},{iy}) size={iw}x{ih} localCT={lct_size} "
                  f"compressed≈{p-start} bytes delay={delays[-1]*10}ms")
    elif b == 0x3B:  # trailer
        print("Trailer reached at", p)
        break
    else:
        p += 1

print(f"\nTotal frames: {frames}")
if delays:
    nz = [d for d in delays if d]
    avg = sum(delays)/len(delays)
    print(f"Delay per frame (GIF units, 1=10ms): min={min(delays)} max={max(delays)} avg={avg:.1f}  => avg {avg*10:.1f} ms/frame")
    if nz:
        print(f"Implied GIF fps (from delay): {1000/(avg*10):.1f}" if avg>0 else "delay=0 => as fast as possible")
print(f"Transparent frames: {transparent_frames}")
print(f"Loop count: {loop_count} (0 = infinite)")
if img_data_sizes:
    print(f"Compressed image-data per frame: min={min(img_data_sizes)} max={max(img_data_sizes)} "
          f"avg={sum(img_data_sizes)//len(img_data_sizes)} bytes")
print(f"\nDecoded canvas (RGB565) per frame: {w}*{h}*2 = {w*h*2} bytes = {w*h*2/1024:.1f} KB")
print(f"GIF file total: {len(data)} bytes = {len(data)/1024:.1f} KB")
print(f"Compression ratio vs one raw RGB565 frame: {len(data)/(w*h*2):.2f}x the file for {frames} frames")
