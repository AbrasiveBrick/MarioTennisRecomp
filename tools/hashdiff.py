"""Compare two MT_HASH_LOG files (src/game/mt_netplay.cpp) and report where the runs diverge.

Usage: python tools/hashdiff.py a.hl b.hl [--skip 0-3,40] [--frames N]

Prints the first frame whose RDRAM differs (ignoring --skip blocks), the blocks that differ there,
and for every block how often it differs over the compared frames, so blocks that never hold game
state (framebuffers, audio) can be told apart from a real divergence.
"""
import struct, sys


def load(path):
    data = open(path, "rb").read()
    magic, version, block, nblocks = struct.unpack_from("<4sIII", data, 0)
    assert magic == b"MTHL" and version == 1, path
    rec = 4 + 8 * nblocks
    frames = {}
    for off in range(16, len(data) - rec + 1, rec):
        f = struct.unpack_from("<I", data, off)[0]
        frames[f] = struct.unpack_from(f"<{nblocks}Q", data, off + 4)
    return block, nblocks, frames


def parse_ranges(s):
    out = set()
    for item in filter(None, s.split(",")):
        a, _, b = item.partition("-")
        out.update(range(int(a), int(b or a) + 1))
    return out


def main():
    args = sys.argv[1:]
    skip, limit = set(), None
    if "--skip" in args:
        i = args.index("--skip"); skip = parse_ranges(args[i + 1]); del args[i:i + 2]
    if "--frames" in args:
        i = args.index("--frames"); limit = int(args[i + 1]); del args[i:i + 2]
    block, nblocks, a = load(args[0])
    _, _, b = load(args[1])
    common = sorted(set(a) & set(b))
    if limit is not None:
        common = common[:limit]
    print(f"{len(a)} and {len(b)} frames, comparing {len(common)}")
    first = None
    counts = [0] * nblocks
    first_seen = [None] * nblocks
    for f in common:
        diff = [i for i in range(nblocks) if a[f][i] != b[f][i]]
        for i in diff:
            counts[i] += 1
            if first_seen[i] is None:
                first_seen[i] = f
        if first is None and any(i not in skip for i in diff):
            first = (f, [i for i in diff if i not in skip])
    if first is None:
        print("no divergence outside the skipped blocks")
    else:
        f, diff = first
        print(f"first divergence at frame {f}: blocks " +
              ", ".join(f"{i} (0x{0x80000000 + i * block:08X})" for i in diff))
    print("blocks that ever differ (block, address, frames differing, first frame):")
    for i in range(nblocks):
        if counts[i]:
            print(f"  {i:3d} 0x{0x80000000 + i * block:08X} {counts[i]:6d} {first_seen[i]}" + ("  [skipped]" if i in skip else ""))


if __name__ == "__main__":
    main()
