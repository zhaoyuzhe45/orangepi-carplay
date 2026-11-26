"""Generate the project's neutral display icon and 800x480 H.264 fallback.

Requires ffmpeg with libx264. PNG generation uses only Python's standard library.
"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def render(width, height):
    # Simple original monitor silhouette: no third-party logo or font asset.
    scale = min(width, height) / 192
    pixels = bytearray(bytes((17, 24, 39)) * (width * height))

    def box(x1, y1, x2, y2, color):
        ox = (width - 192 * scale) / 2
        oy = (height - 192 * scale) / 2
        for y in range(round(oy + y1 * scale), round(oy + y2 * scale)):
            for x in range(round(ox + x1 * scale), round(ox + x2 * scale)):
                offset = (y * width + x) * 3
                pixels[offset:offset + 3] = bytes(color)

    blue, dark = (96, 165, 250), (17, 24, 39)
    box(35, 48, 157, 132, blue)
    box(42, 55, 150, 125, dark)
    box(89, 132, 103, 147, blue)
    box(69, 147, 123, 154, blue)
    return bytes(pixels)


def png(width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory() as temp:
        ppm, stream = Path(temp) / 'screen.ppm', Path(temp) / 'screen.h264'
        ppm.write_bytes(b'P6\n800 480\n255\n' + render(800, 480))
        subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', str(ppm), '-c:v', 'libx264',
                        '-profile:v', 'baseline', '-level', '3.1', '-pix_fmt', 'yuv420p',
                        '-g', '1', '-bf', '0', '-crf', '26', '-f', 'h264', str(stream)], check=True)
        subprocess.run([sys.executable, str(root / 'tools/generate-screen-fixture.py'), str(stream),
                        str(root / 'src/airplay_screen_fixture.c')], check=True)
    icon = root / 'src/android_audio_capture/res/drawable-nodpi/car_ui_fallback.png'
    icon.write_bytes(png(192, 192, render(192, 192)))
    print('Generated neutral source assets; existing installed packages are unchanged.')


if __name__ == '__main__':
    main()
