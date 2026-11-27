"""Write an input replay for MT_REPLAY_PLAY (src/game/mt_netplay.cpp) from a frame-based script.

Usage: python tools/make_replay.py out.mtr PLAYERS FRAMES "frame:BUTTONS:len[@port];..."
Buttons: A B Z START L R DU DD DL DR CU CD CL CR and stick UP DOWN LEFT RIGHT, joined with '+'.
Frames are game frames (30 per second); port defaults to 0.
"""
import struct, sys

BITS = {"A": 0x8000, "B": 0x4000, "Z": 0x2000, "START": 0x1000, "DU": 0x0800, "DD": 0x0400, "DL": 0x0200,
        "DR": 0x0100, "L": 0x0020, "R": 0x0010, "CU": 0x0008, "CD": 0x0004, "CL": 0x0002, "CR": 0x0001}
STICK = {"UP": (0, 127), "DOWN": (0, -127), "LEFT": (-127, 0), "RIGHT": (127, 0)}


def main():
    out, players, total, script = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    pads = [[[0, 0, 0] for _ in range(4)] for _ in range(total)]
    for entry in filter(None, script.split(";")):
        start, names, length = entry.split(":")
        port = 0
        if "@" in length:
            length, port = length.split("@")
        for f in range(int(start), min(total, int(start) + int(length))):
            for n in names.split("+"):
                if n in BITS:
                    pads[f][int(port)][0] |= BITS[n]
                elif n in STICK:
                    pads[f][int(port)][1], pads[f][int(port)][2] = STICK[n]
    with open(out, "wb") as fh:
        fh.write(b"MTRP" + struct.pack("<III", 1, players, 0))
        for frame in pads:
            for b, x, y in frame:
                fh.write(struct.pack("<Hbb", b, x, y))


if __name__ == "__main__":
    main()
