#!/usr/bin/env python3
"""Verify a .qtsq file against docs/FORMAT.md, using only that document.

The point is that the spec cannot silently drift from the code: this
parses a container byte-for-byte using the offsets written in
docs/FORMAT.md and nothing else, so a layout change that is not also a
spec change shows up here.

    python3 tools/verify_format.py model.qtsq [more.qtsq ...]

Exit status 0 if every file checks out, 1 otherwise.
"""
import struct
import sys

HDR_V3 = 112
HDR_V2 = 96
HDR_V1 = 80
MAGIC = b"QTS\x01"
TOC_ENTRY = 32
BLOB_MAGIC = b"BLB1"
TNS2_MAGIC = b"TNS2"
QTC_MAGIC = b"SGC\x01"
SECURITY_BLOCK_V3 = 4501
MAX_STREAMS = 65536

# V3 security block (packed): security_version@0, encryption_type@1,
# signature_type@2, geocrypt_mode@3, nonce@4 (12), auth_tag@16 (16),
# salt@32 (32). A plaintext file carries a fully zeroed stub, so a
# zero salt is what distinguishes it (§8).
SEC_ENC_TYPE = 1
SEC_SALT = 32

# Per-stream flags (§6).
FLAG_ENCRYPTED = 0x0001

KIND = {0: "tensor", 1: "blob", 2: "text", 3: "image", 4: "audio", 5: "table"}


def fail(msg, errs):
    errs.append(msg)


def align4(n):
    return (n + 1 + 3) & ~3


def _name_at(blob, data_at, doff, base, sizes):
    """Best-effort name read for the final entry, where the next offset
    is unknown: the padding rules make the length recoverable."""
    start = data_at + doff
    stop = base + sizes["singularity"]
    n = blob.find(b"\x00", start, stop)
    return blob[start:stop] if n < 0 else blob[start:n]


def verify(path):
    errs = []
    notes = []
    with open(path, "rb") as fh:
        blob = fh.read()
    size = len(blob)
    if size < HDR_V3:
        fail("file shorter than a v3 header (%d bytes)" % size, errs)
        return errs, notes
    if blob[0:4] != MAGIC:
        fail("bad magic %r, expected %r" % (blob[0:4], MAGIC), errs)
        return errs, notes

    version = blob[4]
    if version == 1:
        hdr_len = HDR_V1
    elif version == 2:
        hdr_len = HDR_V2
    elif version == 3:
        hdr_len = HDR_V3
    else:
        # §2.1: an unknown version MUST be rejected, not parsed as v3.
        fail("version %d is not 1/2/3 (must be rejected)" % version, errs)
        return errs, notes

    flags = struct.unpack_from("<I", blob, 6)[0]
    data_type = blob[10]

    if version == 3:
        sizes = {}
        for name, off in (("horizon", 62), ("accretion", 70),
                          ("singularity", 78), ("radiation", 86),
                          ("security", 94)):
            sizes[name] = struct.unpack_from("<Q", blob, off)[0]
    else:
        # v1/v2 have four u32 sizes at 64 and no security block.
        sizes = {}
        for name, off in (("horizon", 64), ("accretion", 68),
                          ("singularity", 72), ("radiation", 76)):
            sizes[name] = struct.unpack_from("<I", blob, off)[0]
        sizes["security"] = 0

    # §2: regions follow header, in this order, located only from these.
    off = hdr_len
    for name in ("horizon", "accretion", "singularity", "radiation", "security"):
        n = sizes[name]
        if n > size - off:
            fail("region %s claims %d bytes but only %d remain"
                 % (name, n, size - off), errs)
            return errs, notes
        if name == "singularity":
            sing_at = off
        off += n
    if off != size:
        # A recovery block may follow when HAS_ECC is set.
        ecc = bool(flags & 0x0100)
        if not ecc:
            fail("region sizes cover %d of %d bytes" % (off, size), errs)
            return errs, notes

    if data_type != 0xF0:
        return errs  # not a container; header checks only

    # §8: the security block is plaintext in both cases; a zero salt means
    # the file is unencrypted and the TOC below is readable as-is.
    sec_at = sing_at + sizes["singularity"] + sizes["radiation"]
    salt = blob[sec_at + SEC_SALT:sec_at + SEC_SALT + 32]
    enc_type = blob[sec_at + SEC_ENC_TYPE]
    encrypted = any(salt)
    if encrypted and enc_type == 0:
        fail("security block has a salt but encryption_type is 0; a reader "
             "must key off the salt, so these disagree", errs)

    # §4: singularity layout.
    base = sing_at
    n_streams = struct.unpack_from("<I", blob, base)[0]
    if encrypted:
        # The singularity region is ciphertext, so the TOC cannot be read
        # without the key. Record it and verify everything else.
        notes.append("encrypted (type %d): TOC not inspectable without the key"
                     % enc_type)
        return errs, notes
    if n_streams > MAX_STREAMS:
        fail("n_streams %d exceeds the %d limit" % (n_streams, MAX_STREAMS), errs)
        return errs, notes
    toc_at = base + 4
    data_at = toc_at + n_streams * TOC_ENTRY
    if data_at > sing_at + sizes["singularity"]:
        fail("TOC overruns the singularity region", errs)
        return errs, notes

    entries = []
    for i in range(n_streams):
        o = toc_at + i * TOC_ENTRY
        dt, st, fl, cid, doff, dsz, osz = struct.unpack_from("<BBHIQQQ", blob, o)
        entries.append((dt, st, fl, cid, doff, dsz, osz))

    for i, (dt, st, fl, cid, doff, dsz, osz) in enumerate(entries):
        # §4: data_offset is the start of the NAME, not of the payload.
        # Layout is [name, padded to 4][payload], so the padded name
        # length is recoverable as next_doff - doff - data_size -- which
        # is also the only way to read a name that has been
        # XOR-obfuscated, since the NUL terminator is gone.
        if i + 1 < n_streams:
            name_pad = entries[i + 1][4] - doff - dsz
            if name_pad <= 0 or name_pad % 4:
                fail("stream %d: name padding %d is not a positive multiple "
                     "of 4" % (i, name_pad), errs)
                continue
            name = blob[data_at + doff:data_at + doff + name_pad]
        else:
            name = _name_at(blob, data_at, doff, base, sizes)
            name_pad = align4(len(name))
        if i + 1 < n_streams and name[-1:] != b"\x00":
            fail("stream %d name is not NUL-padded" % i, errs)
        body_at = data_at + doff + name_pad
        if body_at + dsz > base + sizes["singularity"]:
            fail("stream %d payload runs past the data block" % i, errs)
            continue
        if dsz == 0:
            fail("stream %d has a zero-length payload" % i, errs)
            continue
        if fl & FLAG_ENCRYPTED:
            body_at += 1152  # sizeof(qtsq_stream_encryption_t)
            notes.append("stream %d is per-stream encrypted" % i)
        head = blob[body_at:body_at + 4]

        # codec_id consistency (§5): a non-zero id must match the magic.
        family = (cid >> 24) & 0xFF
        if family == 0x10 and head[:4] != BLOB_MAGIC:
            fail("stream %d claims BLB1 (codec %#010x) but payload is %r"
                 % (i, cid, head), errs)
        elif family == 0x04 and head[:4] != TNS2_MAGIC:
            fail("stream %d claims TNS2 (codec %#010x) but payload is %r"
                 % (i, cid, head), errs)
        elif family == 0x03 and head[:4] != QTC_MAGIC:
            fail("stream %d claims a qtc stream but payload is %r" % (i, head), errs)
        elif family == 0x10 and head[:4] == b"SQTR":
            # SQTR is the legacy Horizon wrapper, not a BLB1 blob.
            fail("stream %d claims BLB1 but payload is the legacy SQTR "
                 "framing" % i, errs)

        # BLB1 self-consistency (§7.2).
        if head[:4] == BLOB_MAGIC:
            h = struct.unpack_from("<4sBBHHBBIIIQ", blob, body_at)
            _, kind, bcodec, bflags, ctype, depth, policy, p0, p1, chans, osz2 = h
            if kind not in KIND:
                fail("stream %d has unknown kind %d" % (i, kind), errs)
            if ctype and ctype > 60:
                fail("stream %d has implausible content_type %d" % (i, ctype), errs)
            if osz2 != osz:
                # BLB1.original_size must agree with the TOC entry.
                fail("stream %d: BLB1 original_size %d != TOC %d"
                     % (i, osz2, osz), errs)

    # §4.2: the manifest is a reserved name; a reader rejects it as a
    # user-supplied name, and there must be at most one.
    man = 0
    for i in range(n_streams):
        if i + 1 < n_streams:
            raw = blob[data_at + entries[i][4]:data_at + entries[i + 1][4]]
        else:
            raw = _name_at(blob, data_at, entries[i][4], base, sizes)
        if raw.split(b"\x00", 1)[0] == b"__manifest__":
            man += 1
    if man > 1:
        fail("%d streams named __manifest__ (reserved)" % man, errs)

    return errs, notes


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    bad = 0
    for path in argv[1:]:
        errs, notes = verify(path)
        if errs:
            bad += 1
            print("FAIL %s" % path)
            for e in errs:
                print("  - %s" % e)
        else:
            print("ok   %s" % path)
        for n in notes:
            print("     note: %s" % n)
    print("\n%d/%d files conform to docs/FORMAT.md"
          % (len(argv) - 1 - bad, len(argv) - 1))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
