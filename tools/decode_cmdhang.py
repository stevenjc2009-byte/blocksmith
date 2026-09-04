#!/usr/bin/env python3
"""Decode sdmc:/blocksmith/cmdhang.bin -- the PICA200 command list a freeze report caught the
GPU stuck inside, as written unconditionally by app/gputest.c's dumpCmdHang() (see the big
comment above that function for the file layout and why the dump exists at all).

Usage:
    python decode_cmdhang.py <path-to-cmdhang.bin>

Prints the header fields, then the command stream: word offset, register, byte-enable mask,
parameter words, and whether the write was a "consecutive" (incrementing-register) burst. Ends
by saying exactly where the decode stopped relative to the file's own recorded length, because
a dump that stops early -- truncated at LIST_MAX, or cut off mid-command -- is itself evidence.

------------------------------------------------------------------------------------------------
FILE FORMAT (must match app/gputest.c's dumpCmdHang(), CMDHANG_HDR_BYTES = 72)

  offset  0 (4)  magic        ASCII "CDH1"
  offset  4 (4)  hang_addr    u32 LE -- address the list was read from
  offset  8 (4)  hang_size    u32 LE -- byte length the GX queue entry recorded for the list
  offset 12 (4)  dumped_size  u32 LE -- bytes of raw list data appended after this header;
                                        <= hang_size and <= LIST_MAX (32768). A shorter value
                                        than hang_size means the list was truncated on write.
  offset 16 (4)  frame        u32 LE -- the frame counter gpuTestPostMortem() was called with
  offset 20 (4)  q_cap        u32 LE -- GX queue capacity at the moment of the wedge
  offset 24 (4)  q_queued     u32 LE
  offset 28 (4)  q_submitted  u32 LE
  offset 32 (4)  q_completed  u32 LE -- all four zero if the queue could not be read safely
  offset 36 (36) version      ASCII, NUL-padded, BLOCKSMITH_VERSION
  offset 72 (dumped_size)     the raw command-list bytes, verbatim, starting at hang_addr

------------------------------------------------------------------------------------------------
PICA200 COMMAND-LIST ENCODING

Each command is a run of words: word[0] is the first parameter, word[1] is the header, and any
further parameter words follow at word[2..]. The whole command is padded to an even word count.

Header word bit layout (word[1]):
    bits [15:0]   register id           (the real register space is 10 bits; the top 6 of this
                                          16-bit field are always 0 for a valid register)
    bits [19:16]  byte-enable mask
    bits [27:20]  extra parameter words (0 means the command carries just the one word at word[0])
    bit  [31]     consecutive-write flag -- when set, each extra parameter word targets the NEXT
                                             register (register id + 1, +2, ...) instead of the
                                             same one repeatedly

This is not guesswork: it is exactly what app/gputest.c's own cmdAt()/PARAM() walk decodes (see
the comment above cmdAt() in that file, "word0 = the parameter / word1 = the header"), and it was
cross-checked against two upstream sources for this tool:

  - libctru source/gpu/gpu.c, GPUCMD_AddInternal(): the actual command-list writer citro3d/libctru
    use to build every list this game ever submits. It packs the header exactly as above --
    `header |= (paramlength & 0xff) << 20` for the extra-word count, `BIT(31)` for the
    incremental/consecutive flag (see libctru's GPUCMD_HEADER macro, 3ds/gpu/gpu.h:
    `(((incremental)<<31)|(((mask)&0xF)<<16)|((reg)&0x3FF))`), and pads with one trailing zero
    word whenever the total command length would otherwise be odd.
    https://github.com/devkitPro/libctru/blob/master/libctru/source/gpu/gpu.c
    https://github.com/devkitPro/libctru/blob/master/libctru/include/3ds/gpu/gpu.h

  - 3dbrew, "GPU/Internal Registers": describes the same consecutive-write mechanism in prose and
    gives a worked example ("0xAAAAAAAA 0x802F011C 0xBBBBBBBB 0xCCCCCCCC" -> three writes, the
    first two sequential-register aliases of one logical register) that was hand-decoded against
    the bit layout above as a cross-check: 0x802F011C = bit31 set (consecutive), bits[27:20] = 2
    (two extra parameter words, matching 0xBBBBBBBB/0xCCCCCCCC), bits[19:16] = 0xF (full mask),
    bits[15:0] = 0x011C (the register id). It matched exactly.
    https://www.3dbrew.org/wiki/GPU/Internal_Registers
    (fetched via search; 3dbrew itself returns HTTP 403 to automated fetches from this tool's
    author's environment, so the page was read through search-engine excerpts, not scraped whole)
"""
import struct
import sys

HDR_BYTES = 72
MAGIC = b"CDH1"
LIST_MAX = 32 * 1024


class DecodeError(Exception):
    pass


def decode_header(buf):
    if len(buf) < HDR_BYTES:
        raise DecodeError(
            f"file is {len(buf)} bytes, shorter than the {HDR_BYTES}-byte header alone")

    magic = buf[0:4]
    if magic != MAGIC:
        raise DecodeError(
            f"bad magic {magic!r}, expected {MAGIC!r} -- this is not a cmdhang.bin "
            f"(or the header layout changed and this script is stale)")

    (hang_addr, hang_size, dumped_size, frame,
     q_cap, q_queued, q_submitted, q_completed) = struct.unpack_from("<8I", buf, 4)
    version = buf[36:72].split(b"\x00", 1)[0].decode("ascii", "replace")

    return {
        "hang_addr": hang_addr,
        "hang_size": hang_size,
        "dumped_size": dumped_size,
        "frame": frame,
        "q_cap": q_cap,
        "q_queued": q_queued,
        "q_submitted": q_submitted,
        "q_completed": q_completed,
        "version": version,
    }


def cmd_at(words, nwords, i):
    """Mirrors app/gputest.c's cmdAt(): returns (reg, mask, nparams, total_words, consec) for
    the command starting at word index i, or None if there isn't a full, padded command there."""
    if i + 1 >= nwords:
        return None
    hdr = words[i + 1]
    extra = (hdr >> 20) & 0xFF
    n = 2 + extra
    if n & 1:
        n += 1
    if i + n > nwords:
        return None
    reg = hdr & 0xFFFF
    mask = (hdr >> 16) & 0xF
    consec = bool((hdr >> 31) & 1)
    nparams = 1 + extra
    return reg, mask, nparams, n, consec


def param(words, i, p):
    return words[i] if p == 0 else words[i + 1 + p]


def decode_commands(data, dumped_size):
    """data is the raw command-list bytes (dumped_size of them). Returns (commands, trailing)
    where commands is a list of dicts and trailing is the count of bytes left over that could
    not be parsed as a complete, padded command."""
    nwords = dumped_size // 4
    words = struct.unpack_from(f"<{nwords}I", data, 0) if nwords else ()

    commands = []
    i = 0
    while True:
        c = cmd_at(words, nwords, i)
        if c is None:
            break
        reg, mask, nparams, n, consec = c
        params = [param(words, i, p) for p in range(nparams)]
        commands.append({
            "word": i,
            "reg": reg,
            "mask": mask,
            "consec": consec,
            "nparams": nparams,
            "words": n,
            "params": params,
        })
        i += n

    words_consumed = i
    trailing_words = nwords - words_consumed
    trailing_bytes = trailing_words * 4 + (dumped_size - nwords * 4)
    return commands, words_consumed, trailing_bytes


def format_report(path, buf):
    hdr = decode_header(buf)
    lines = []
    lines.append(f"file          : {path}")
    lines.append(f"version       : {hdr['version']}")
    lines.append(f"frame         : {hdr['frame']}")
    lines.append(f"hang_addr     : 0x{hdr['hang_addr']:08x}")
    lines.append(f"hang_size     : {hdr['hang_size']} bytes "
                 f"({hdr['hang_size'] / 4:.0f} words) -- recorded by the GX queue entry")
    lines.append(f"dumped_size   : {hdr['dumped_size']} bytes "
                 f"({hdr['dumped_size'] / 4:.0f} words) -- actually appended to this file")
    if hdr["dumped_size"] < hdr["hang_size"]:
        missing = hdr["hang_size"] - hdr["dumped_size"]
        why = ("LIST_MAX (32768 B) cap" if hdr["dumped_size"] == LIST_MAX
               else "unknown -- dumped_size is neither hang_size nor LIST_MAX")
        lines.append(f"              *** TRUNCATED: {missing} bytes of the list were NOT "
                     f"captured ({why}) ***")
    lines.append(f"gx queue      : cap {hdr['q_cap']}  queued {hdr['q_queued']}  "
                 f"submitted {hdr['q_submitted']}  completed {hdr['q_completed']}")

    body = buf[HDR_BYTES:HDR_BYTES + hdr["dumped_size"]]
    if len(body) < hdr["dumped_size"]:
        lines.append(f"\n*** FILE TRUNCATED ON DISK: header claims {hdr['dumped_size']} bytes "
                     f"of command data, only {len(body)} are actually present ***")
    commands, words_consumed, trailing_bytes = decode_commands(body, len(body))

    lines.append(f"\n{len(commands)} command(s) decoded, {words_consumed} of "
                 f"{len(body) // 4} word(s)\n")
    lines.append(f"{'word':>6}  {'reg':>5}  {'mask':>4}  {'np':>3}  {'flags':<11}  params")
    for c in commands:
        flags = "consecutive" if c["consec"] else "-"
        params_str = " ".join(f"{v:08x}" for v in c["params"][:8])
        if len(c["params"]) > 8:
            params_str += f"  ... (+{len(c['params']) - 8} more)"
        lines.append(f"{c['word']:>6}  {c['reg']:03x}{'':>2}  {c['mask']:>4x}  "
                     f"{c['nparams']:>3}  {flags:<11}  {params_str}")

    lines.append("")
    if trailing_bytes == 0:
        lines.append(f"stream ends cleanly at word {words_consumed} -- exactly "
                     f"{len(body)} bytes recorded, no leftover.")
    else:
        lines.append(f"*** {trailing_bytes} byte(s) after word {words_consumed} do NOT form a "
                     f"complete command -- the dump stopped mid-command. If dumped_size was\n"
                     f"    equal to LIST_MAX this is expected (the list was truncated on "
                     f"write); otherwise the recorded hang_size may be wrong, or the copy was\n"
                     f"    corrupted. ***")

    return "\n".join(lines)


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} <path-to-cmdhang.bin>", file=sys.stderr)
        return 2
    path = argv[1]
    try:
        with open(path, "rb") as f:
            buf = f.read()
    except OSError as e:
        print(f"error: could not read {path}: {e}", file=sys.stderr)
        return 1

    try:
        report = format_report(path, buf)
    except DecodeError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
