#!/usr/bin/env python3
"""Minimal Annex-B H.264 -> MP4 muxer (no B-frames, fixed fps).

Good enough for a one-shot WAM <video> test clip on the 2015 LG panel:
ftyp/moov(avc1+avcC)/mdat, no stss (all samples sync), stsz from AU sizes.
"""
import struct, sys


def find_nals(data):
    starts = []
    i = 0
    while True:
        p = data.find(b"\x00\x00\x01", i)
        q = data.find(b"\x00\x00\x00\x01", i)
        if p == -1 and q == -1:
            break
        if q != -1 and (p == -1 or q < p):
            starts.append(q + 4); i = q + 4
        else:
            starts.append(p + 3); i = p + 3
    nals = []
    for idx, off in enumerate(starts):
        end = starts[idx + 1] - 3 if idx + 1 < len(starts) else len(data)
        while end > off + 1 and data[end - 1] == 0:
            end -= 1
        if data[off:end]:
            nals.append(data[off:end])
    return nals


def ue_read(r, st):
    bit = st[0]; zeros = 0
    while zeros < 32 and bit < len(r) * 8 and (r[bit >> 3] & (0x80 >> (bit & 7))) == 0:
        zeros += 1; bit += 1
    bit += 1  # the terminating 1 bit
    val = 0
    for _ in range(zeros):
        val = (val << 1) | (1 if r[bit >> 3] & (0x80 >> (bit & 7)) else 0); bit += 1
    st[0] = bit
    return (1 << zeros) - 1 + val


def u1_read(r, st):
    bit = st[0]
    v = 1 if r[bit >> 3] & (0x80 >> (bit & 7)) else 0
    st[0] = bit + 1
    return v


def se_read(r, st):
    v = ue_read(r, st)
    return (v + 1) // 2 if v % 2 else -(v // 2)


def rbsp(nal):
    out = bytearray(); i = 0
    while i < len(nal):
        if i + 2 < len(nal) and nal[i] == 0 and nal[i + 1] == 0 and nal[i + 2] == 3:
            out += nal[i:i + 2]; i += 3
        else:
            out.append(nal[i]); i += 1
    return bytes(out)


def sps_size(nal):
    r = rbsp(nal[1:])
    st = [24]  # profile, compat/constraints, level
    prof = nal[1]
    ue_read(r, st)  # sps id
    chroma = 1
    if prof in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        chroma = ue_read(r, st)
        if chroma == 3:
            u1_read(r, st)
        ue_read(r, st); ue_read(r, st)  # bit depths minus 8
        u1_read(r, st)                  # qpprime bypass
        if u1_read(r, st):              # scaling matrices present
            for i in range(12 if chroma == 3 else 8):
                if u1_read(r, st):
                    size = 64 if i >= 6 else 16
                    last = nxt = 8
                    for _ in range(size):
                        if nxt != 0:
                            nxt = (last + se_read(r, st)) & 0xFF
                        if nxt != 0:
                            last = nxt
    ue_read(r, st)  # log2_max_frame_num_minus4
    poc = ue_read(r, st)
    if poc == 0:
        ue_read(r, st)
    elif poc == 1:
        u1_read(r, st)
        se_read(r, st); se_read(r, st); se_read(r, st)
        for _ in range(ue_read(r, st)):
            se_read(r, st)
    ue_read(r, st)  # max_num_ref_frames
    u1_read(r, st)  # gaps allowed
    wmb = ue_read(r, st); hmb = ue_read(r, st)
    frame_only = u1_read(r, st)
    if not frame_only:
        u1_read(r, st)
    cl = cr = ct = cb = 0
    if u1_read(r, st):  # crop
        cl = ue_read(r, st); cr = ue_read(r, st); ct = ue_read(r, st); cb = ue_read(r, st)
    cw, ch = 2, 2
    if chroma == 2:
        cw, ch = 2, 1
    elif chroma == 3:
        cw, ch = 1, 1
    w = (wmb + 1) * 16 - (cl + cr) * cw
    h = (2 - frame_only) * (hmb + 1) * 16 - (ct + cb) * ch * 2
    return w, h


def first_slice_new_pictures(nal):
    """True if this VCL NAL starts a new access unit (single-slice streams)."""
    t = nal[0] & 0x1F
    if t == 5:
        return True  # IDR: assume single slice per picture
    if t != 1:
        return True
    r = nal  # bit reader on NAL payload
    st = [8 + 3]  # skip nal header + forbidden + nal_ref_idc
    ue_read(r, st)  # slice_type
    fmib = ue_read(r, st)
    return fmib == 0


def group_aus(nals):
    aus, cur = [], []
    for nal in nals:
        t = nal[0] & 0x1F
        if t in (9,):           # AUD
            if cur: aus.append(cur); cur = []
            continue
        if t in (7, 8):         # SPS/PPS start a new AU
            if cur: aus.append(cur)
            cur = [nal]
        elif t in (1, 5):
            if cur and first_slice_new_pictures(nal):
                aus.append(cur); cur = []
            cur.append(nal)
        elif cur:
            cur.append(nal)
    if cur:
        aus.append(cur)
    return [au for au in aus if any((n[0] & 0x1F) in (1, 5) for n in au)]


def box(tag, payload):
    return struct.pack(">I", 8 + len(payload)) + tag + payload


def full(tag, ver, flags, payload):
    return box(tag, struct.pack(">B", ver) + flags.to_bytes(3, "big") + payload)


def main(src, dst, fps=30):
    data = open(src, "rb").read()
    nals = find_nals(data)
    sps = next(n for n in nals if (n[0] & 0x1F) == 7)
    pps = next(n for n in nals if (n[0] & 0x1F) == 8)
    aus = group_aus(nals)
    width, height = sps_size(sps)
    timescale = fps * 1000
    dur = 1000
    samples = [b"".join(struct.pack(">I", len(n)) + n for n in au) for au in aus]
    mdat_payload = b"".join(samples)

    avcc = (bytes([1, sps[1], sps[2], sps[3], 0xE1]) + struct.pack(">H", len(sps)) + sps
            + bytes([1]) + struct.pack(">H", len(pps)) + pps)
    matrix = struct.pack(">9I", 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x3F000000)
    avc1_payload = (b"\0" * 6 + struct.pack(">H", 1)
                    + struct.pack(">HH", 0, 0) + b"\0" * 12
                    + struct.pack(">HH", width, height)
                    + struct.pack(">II", 0x00480000, 0x00480000)
                    + struct.pack(">I", 0) + struct.pack(">H", 1) + b"\0" * 32
                    + struct.pack(">hhh", 24, -1, -1)
                    + box(b"avcC", avcc))
    avc1 = box(b"avc1", avc1_payload)
    stsd = full(b"stsd", 0, 0, struct.pack(">I", 1) + avc1)
    vmhd = full(b"vmhd", 0, 1, b"\0" * 8)
    dinf = box(b"dinf", full(b"dref", 0, 0, struct.pack(">I", 1) + full(b"url ", 0, 1, b"")))
    stts = full(b"stts", 0, 0, struct.pack(">I", 1) + struct.pack(">II", len(samples), dur))
    stsc = full(b"stsc", 0, 0, struct.pack(">I", 1)
                + struct.pack(">III", 1, len(samples), 1))
    stsz = full(b"stsz", 0, 0, struct.pack(">II", 0, len(samples))
                + b"".join(struct.pack(">I", len(s)) for s in samples))
    stco = full(b"stco", 0, 0, struct.pack(">I", 1) + struct.pack(">I", 0))
    stbl = box(b"stbl", stsd + stts + stsc + stsz + stco)
    minf = box(b"minf", vmhd + dinf + stbl)
    hdlr = full(b"hdlr", 0, 0, b"\0\0\0\0" + b"vide" + b"\0" * 12 + b"VideoHandler\0")
    mdhd = full(b"mdhd", 0, 0, struct.pack(">IIII", 0, 0, timescale, len(samples) * dur)
                + b"\x55\xC4\x00\x00")
    mdia = box(b"mdia", mdhd + hdlr + minf)
    tkhd = full(b"tkhd", 0, 3, struct.pack(">IIII", 0, 0, timescale, len(samples) * dur)
                + b"\0" * 8 + struct.pack(">HHHH", 0, 0, 0, 0)
                + matrix + struct.pack(">II", width << 16, height << 16))
    trak = box(b"trak", tkhd + mdia)
    mvhd = full(b"mvhd", 0, 0, struct.pack(">IIII", 0, 0, timescale, len(samples) * dur)
                + struct.pack(">I", 0x00010000) + struct.pack(">HH", 0x0100, 0)
                + b"\0" * 8 + matrix + b"\0" * 24 + struct.pack(">I", 2))
    moov = box(b"moov", mvhd + trak)
    ftyp = box(b"ftyp", b"isom" + struct.pack(">I", 0x200) + b"isomiso2avc1mp41")

    out = bytearray(ftyp + moov + box(b"mdat", mdat_payload))
    mdat_data_off = len(ftyp) + len(moov) + 8
    pos = bytes(out).rfind(stco)
    out[pos + 16:pos + 20] = struct.pack(">I", mdat_data_off)
    open(dst, "wb").write(bytes(out))
    print(f"{dst}: {width}x{height} {len(samples)} AUs @ {fps}fps -> {len(out)} bytes")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 30)
