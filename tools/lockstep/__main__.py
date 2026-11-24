"""Compare lockstep traces and memory dumps (src/kernel/recomp_lockstep.h).

    py -3 -m tools.lockstep trace <a.trace> <b.trace>
        tick by tick: the first tick each 64 KB chunk differs, how many ticks
        it differs in, and the first tick whose input differs (a replay that
        was fed something else is not a determinism bug)

    py -3 -m tools.lockstep dump <a.bin> <b.bin> [--context N] [--max N]
        the byte ranges that differ between two dumps of the same tick
        (RECOMP_LOCKSTEP_DUMP), as guest addresses, merged when closer than
        16 bytes, with the 32-bit words on each side

    py -3 -m tools.lockstep dumps <a.bin> <b.bin> <c.bin> ...
        which differing words stay different and which change: ranges that
        differ in every pair are where two runs went separate ways
"""
import argparse
import struct
import sys
from collections import OrderedDict


def read_trace(path):
    ticks = OrderedDict()
    with open(path, "r", encoding="ascii", errors="replace") as f:
        for line in f:
            if not line.startswith("t="):
                continue
            fields = {}
            for part in line.split():
                k, _, v = part.partition("=")
                fields[k] = v
            t = int(fields["t"])
            ticks[t] = (fields.get("m", ""), fields.get("p", ""), fields.get("h", ""), fields.get("c", ""))
    return ticks


def cmd_trace(a, b, chunk=0, base=0):
    ta, tb = read_trace(a), read_trace(b)
    common = [t for t in ta if t in tb]
    if not common:
        print("no ticks in common")
        return 1
    first_input = None
    chunk_first = {}
    chunk_count = {}
    chunk_changes = {}
    first_hash = None
    same = 0
    for t in common:
        ma, pa, ha, ca = ta[t]
        mb, pb, hb, cb = tb[t]
        if (ma, pa) != (mb, pb) and first_input is None:
            first_input = t
        if ha == hb:
            same += 1
            continue
        if first_hash is None:
            first_hash = t
        n = min(len(ca), len(cb)) // 8
        for i in range(n):
            if ca[8 * i:8 * i + 8] != cb[8 * i:8 * i + 8]:
                chunk_first.setdefault(i, t)
                chunk_count[i] = chunk_count.get(i, 0) + 1
    print(f"{len(common)} ticks in common ({common[0]}..{common[-1]}); {same} the same")
    print(f"first tick whose input differs: {first_input}")
    print(f"first tick whose state differs: {first_hash}")
    for i in sorted(chunk_first, key=lambda c: chunk_first[c]):
        where = f"  0x{base + i * chunk:08X}" if chunk else ""
        print(f"  chunk {i:3d}{where}: first differs at tick {chunk_first[i]:6d}, differs in {chunk_count[i]:6d} ticks")
    return 0


def read_dump(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"LSDUMP1\n":
        sys.exit(f"{path}: not a lockstep dump")
    (count,) = struct.unpack_from("<i", data, 8)
    off = 12
    regions = []
    for _ in range(count):
        va, size = struct.unpack_from("<II", data, off)
        name = data[off + 8:off + 40].split(b"\0", 1)[0].decode("ascii", "replace")
        off += 40
        regions.append((va, size, name, data[off:off + size]))
        off += size
    return regions


def diff_ranges(ba, bb, merge=16):
    n = min(len(ba), len(bb))
    ranges = []
    i = 0
    # Compare 8 bytes at a time to find candidates fast.
    while i < n:
        if ba[i:i + 64] == bb[i:i + 64]:
            i += 64
            continue
        j = i
        end = min(i + 64, n)
        while j < end:
            if ba[j] != bb[j]:
                if ranges and j - ranges[-1][1] <= merge:
                    ranges[-1][1] = j + 1
                else:
                    ranges.append([j, j + 1])
            j += 1
        i = end
    return ranges


def words(buf, lo, hi):
    lo &= ~3
    hi = (hi + 3) & ~3
    return " ".join(f"{struct.unpack_from('<I', buf, k)[0]:08X}" for k in range(lo, min(hi, len(buf) - 3), 4))


def cmd_dump(a, b, max_ranges):
    ra, rb = read_dump(a), read_dump(b)
    total = 0
    for (va, size, name, ba), (vb, sb, nb, bb) in zip(ra, rb):
        if (va, size) != (vb, sb):
            print(f"region {name}: different extents, skipped")
            continue
        ranges = diff_ranges(ba, bb)
        total += len(ranges)
        print(f"{name} 0x{va:08X}+0x{size:X}: {len(ranges)} differing range(s), "
              f"{sum(h - l for l, h in ranges)} bytes")
        for lo, hi in ranges[:max_ranges]:
            print(f"  0x{va + lo:08X}-0x{va + hi:08X} ({hi - lo:5d} B)  a: {words(ba, lo, min(hi, lo + 32))}")
            print(f"  {'':23s}            b: {words(bb, lo, min(hi, lo + 32))}")
        if len(ranges) > max_ranges:
            print(f"  ... {len(ranges) - max_ranges} more")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("trace")
    t.add_argument("a")
    t.add_argument("b")
    t.add_argument("--chunk", type=lambda v: int(v, 0), default=0,
                   help="chunk size the traces were written with, to print addresses")
    t.add_argument("--base", type=lambda v: int(v, 0), default=0,
                   help="the first region's address (one region only)")
    d = sub.add_parser("dump")
    d.add_argument("a")
    d.add_argument("b")
    d.add_argument("--max", type=int, default=60)
    c = sub.add_parser("compact", help="a recording small enough to pass around: a line "
                       "only where the input changes, and every --every ticks for the hash")
    c.add_argument("a")
    c.add_argument("out")
    c.add_argument("--every", type=int, default=60)
    args = ap.parse_args()
    if args.cmd == "trace":
        return cmd_trace(args.a, args.b, args.chunk, args.base)
    if args.cmd == "compact":
        return cmd_compact(args.a, args.out, args.every)
    return cmd_dump(args.a, args.b, args.max)


def cmd_compact(src, out, every):
    """Keep the epoch, each tick whose mask or pads differ from the tick
    before, and every `every`th tick for its hash; drop the chunk hashes.
    A replay carries the input of a missing tick forward from the last line,
    and checks the hash only where there is one."""
    kept = total = 0
    last = None
    pending = None              # the last tick, written at the end if skipped

    def line_of(fields, inp):
        parts = [f"t={fields['t']}", f"m={inp[0]}", f"p={inp[1]}"]
        if "h" in fields:
            parts.append(f"h={fields['h']}")
        return " ".join(parts) + "\n"

    with open(src, "r", encoding="ascii", errors="replace") as f, \
            open(out, "w", encoding="ascii", newline="\n") as o:
        for line in f:
            if line.startswith("#"):
                o.write(line)
                continue
            if not line.startswith("t="):
                continue
            total += 1
            fields = dict(part.split("=", 1) for part in line.split())
            t = int(fields["t"])
            inp = (fields.get("m", ""), fields.get("p", ""))
            if inp != last or t % every == 0:
                o.write(line_of(fields, inp))
                kept += 1
                pending = None
            else:
                pending = line_of(fields, inp)
            last = inp
        if pending:
            o.write(pending)        # so a replay runs to the recording's end
            kept += 1
    print(f"{out}: {kept} of {total} ticks kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())
