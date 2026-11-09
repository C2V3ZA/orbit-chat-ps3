"""Builds build/ui-intro.png (animated PNG, plays in every browser) from build/intro/f*.ppm."""
import pathlib, struct, zlib


def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)


frames = sorted(pathlib.Path('build/intro').glob('f*.ppm'))
images = []
for f in frames:
    with f.open('rb') as h:
        assert h.readline() == b'P6\n'
        w, hh = map(int, h.readline().split())
        assert h.readline() == b'255\n'
        px = h.read()
    rows = b''.join(b'\x00' + px[y * w * 3:(y + 1) * w * 3] for y in range(hh))
    images.append(zlib.compress(rows, 9))
out = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, hh, 8, 2, 0, 0, 0))
out += chunk(b'acTL', struct.pack('>II', len(images), 0))
seq = 0
for i, data in enumerate(images):
    delay = (300, 100) if i == len(images) - 1 else (5, 100)  # 20 fps, 3 s pause at the end
    out += chunk(b'fcTL', struct.pack('>IIIIIHHBB', seq, w, hh, 0, 0, delay[0], delay[1], 0, 0))
    seq += 1
    if i == 0:
        out += chunk(b'IDAT', data)
    else:
        out += chunk(b'fdAT', struct.pack('>I', seq) + data)
        seq += 1
out += chunk(b'IEND', b'')
pathlib.Path('build/ui-intro.png').write_bytes(out)
print('frames', len(images), 'bytes', len(out))
