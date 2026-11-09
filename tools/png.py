"""Convert the renderer's own PPM outputs into lossless PNG; stdlib only."""
import pathlib, struct, zlib
def chunk(tag, data):
    return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
for source in pathlib.Path('build').glob('*.ppm'):
    with source.open('rb') as f:
        assert f.readline() == b'P6\n'
        w, h = map(int, f.readline().split())
        assert f.readline() == b'255\n'
        pixels = f.read()
    assert len(pixels) == w*h*3
    rows = b''.join(b'\x00' + pixels[y*w*3:(y+1)*w*3] for y in range(h))
    source.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
