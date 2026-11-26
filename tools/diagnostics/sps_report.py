"""Report the real geometry of an H.264 SPS.

Input is either an Annex-B file (the first SPS is used) or a hex string such as
the one `sps_probe` prints on the device:

    sps_probe /data/local/tmp/probe.h264 1920 900
    sps_report.py "27 42 00 1f 8d 68 07 80 22 7e 05 f8 ..."

The distinction between `coded` and `present` is the point: the RK3399 encoder
codes macroblock-aligned frames (1080 -> 1088) and the sender rewrites
frame_cropping so the sink resolves the canvas aspect. `present` is what a
decoder that honours frame_cropping actually shows.
"""
import sys


def _unescape(sps):
    out = bytearray()
    zeros = 0
    for b in sps[1:]:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return out


def _first_sps(data):
    i, n = 0, len(data)
    while i + 3 < n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            i += 3
        elif (data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 0 and
              data[i + 3] == 1):
            i += 4
        else:
            i += 1
            continue
        if i < n and (data[i] & 0x1F) == 7:
            end = n
            for j in range(i + 1, n - 2):
                if data[j] == 0 and data[j + 1] == 0 and data[j + 2] in (0, 1):
                    end = j
                    break
            return data[i:end]
    raise SystemExit('no SPS in input')


def parse(sps):
    bits = ''.join(format(b, '08b') for b in _unescape(sps))
    pos = [0]

    def u(k):
        v = int(bits[pos[0]:pos[0] + k], 2)
        pos[0] += k
        return v

    def ue():
        zeros = 0
        while bits[pos[0]] == '0':
            zeros += 1
            pos[0] += 1
        pos[0] += 1
        return (1 << zeros) - 1 + (u(zeros) if zeros else 0)

    def se():
        k = ue()
        return (k + 1) // 2 if k % 2 else -(k // 2)

    profile = u(8)
    u(8)
    level = u(8)
    ue()
    chroma = 1
    if profile in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        chroma = ue()
        if chroma == 3:
            u(1)
        ue()
        ue()
        u(1)
        if u(1):
            for i in range(8 if chroma != 3 else 12):
                if u(1):
                    last = nxt = 8
                    for _ in range(16 if i < 6 else 64):
                        if nxt:
                            nxt = (last + se() + 256) % 256
                        if nxt:
                            last = nxt
    ue()
    poc = ue()
    if poc == 0:
        ue()
    elif poc == 1:
        u(1)
        se()
        se()
        for _ in range(ue()):
            se()
    ue()
    u(1)
    w_mbs = ue() + 1
    h_map = ue() + 1
    mbs_only = u(1)
    if not mbs_only:
        u(1)
    u(1)
    crop_flag = u(1)
    cl = cr = ct = cb = 0
    if crop_flag:
        cl, cr, ct, cb = ue(), ue(), ue(), ue()
    coded_w = w_mbs * 16
    coded_h = h_map * 16 * (2 - mbs_only)
    unit_x = 1 if chroma in (0, 3) else 2
    unit_y = 1 if chroma == 0 else (1 if chroma == 3 else 2) * (2 - mbs_only)
    return {
        'profile': profile, 'level': level, 'chroma': chroma,
        'coded': (coded_w, coded_h),
        'crop': (cl, cr, ct, cb),
        'present': (coded_w - (cl + cr) * unit_x,
                    coded_h - (ct + cb) * unit_y),
    }


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    arg = argv[1]
    if arg.strip().replace(' ', '').replace(':', '').isalnum() and \
            arg.strip() and not arg.lower().endswith('.h264'):
        sps = bytes(int(x, 16) for x in arg.replace(':', ' ').split())
    else:
        sps = _first_sps(open(arg, 'rb').read())
    r = parse(sps)
    cw, ch = r['coded']
    pw, ph = r['present']
    print(f"profile={r['profile']} level={r['level']} chroma={r['chroma']}")
    print(f"coded   = {cw}x{ch}")
    print(f"crop    = l{r['crop'][0]} r{r['crop'][1]} t{r['crop'][2]} b{r['crop'][3]}")
    print(f"present = {pw}x{ph}  aspect={pw / ph:.4f}")


if __name__ == '__main__':
    main(sys.argv)
