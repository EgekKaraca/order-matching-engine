#!/usr/bin/env python3
"""workload_parity_check.py — is a locally generated workload bit-identical to
the reference the published hash was computed against?

WHY THIS EXISTS. The benchmark's generator hand-rolls every distribution on
std::mt19937 (bit-portable) but still calls libm cos/exp/log/pow, which are NOT
standardised to the last bit. The shipped reference was generated on
glibc/aarch64. If Apple's libm differs anywhere, a locally generated workload
diverges from the reference and the published hash is unreachable on this
machine — which would mean Phase 1 conformance cannot be chased locally at all
and must wait for a Linux box.

HOW IT TESTS WITHOUT AN ENGINE. Every workload field is echoed back in
reference/canonical_output.txt.gz, because each message produces exactly one
ack-or-reject carrying its own identity:

    new     seq -> `0,seq,side,order_id,price_ticks,quantity`   (all 5 fields)
    cancel  seq -> `2,seq,side,order_id,price` | `4,seq,order_id`
    modify  seq -> `3,seq,side,order_id,new_price,new_qty` | `5,seq,order_id`

Note that a CancelAck does NOT imply a cancel message: an IOC new order whose
remainder goes unfilled emits one for its dropped residual, carrying its own
side and price. So type 2 maps to {cancel, new-with-ioc}, and which one it is
tells us the local record's ioc flag matches the reference too.

So the canonical text pins, for every sequence number, the message's class and
order_id; for new orders and successful modifies it pins every field the record
carries. (A CancelAck's side/price come from the *resting* order, so they are
engine-derived and not checked here; cancel_t itself carries only id + seq,
both of which are checked.) Trades (type 1) share the aggressor's seq and are
skipped — they are engine output, not workload input.

Exit 0 = the local workload is the reference workload, so the published hash is
reachable on this machine.

Usage: workload_parity_check.py <workload.bin> <canonical_output.txt.gz>
"""
import gzip
import struct
import sys

# On-disk record, 40 bytes little-endian (workload/generator.cpp:write_binary):
#   [u8 type][u8 side][u8 ioc][u8 pad][u32 qty][u64 seq][u64 order_id]
#   [i64 price_ticks][i64 reserved]
REC = struct.Struct("<BBBBIQQqq")
MAGIC = 0x4D4542575F303031  # "MEBW_001"
MSG_NEW, MSG_CANCEL, MSG_MODIFY = 0, 1, 2

# Which message classes may produce each report type. Type 2 is the interesting
# one: a CancelAck is emitted both by a cancel message and by an IOC new order
# dropping its residual.
CLASSES_OF_REPORT = {
    0: {MSG_NEW},
    2: {MSG_CANCEL, MSG_NEW},
    3: {MSG_MODIFY},
    4: {MSG_CANCEL},
    5: {MSG_MODIFY},
}
CLASS_NAME = {MSG_NEW: "new", MSG_CANCEL: "cancel", MSG_MODIFY: "modify"}


def read_workload(path):
    """seq -> (class, order_id, side, price_ticks, qty). Indexed by seq, which
    generator.cpp assigns as the record's own array index."""
    with open(path, "rb") as f:
        magic, _ver, count = struct.unpack("<QII", f.read(16))
        if magic != MAGIC:
            sys.exit(f"bad magic {magic:#x} — not a workload file")
        blob = f.read()
    if len(blob) != count * REC.size:
        sys.exit(f"truncated: {len(blob)} bytes != {count} * {REC.size}")
    out = [None] * count
    for i in range(count):
        typ, side, ioc, _pad, qty, seq, oid, price, _rsv = REC.unpack_from(blob, i * REC.size)
        if seq != i:
            sys.exit(f"record {i} carries seq {seq} — sequence is not the array index")
        out[i] = (typ, oid, side, price, qty, ioc)
    return out


def main(workload_path, canonical_path):
    wl = read_workload(workload_path)
    print(f"local workload: {len(wl):,} messages")

    seen = [False] * len(wl)
    checked_fields = 0
    errors = []

    with gzip.open(canonical_path, "rt") as f:
        for lineno, line in enumerate(f, 1):
            parts = line.rstrip("\n").split(",")
            rtype = int(parts[0])
            if rtype == 1:  # Trade — engine output, carries the aggressor's seq
                continue
            seq = int(parts[1])
            if seq >= len(wl):
                errors.append(f"line {lineno}: seq {seq} beyond local workload ({len(wl)})")
                break
            cls, oid, side, price, qty, ioc = wl[seq]
            seen[seq] = True

            allowed = CLASSES_OF_REPORT[rtype]
            if cls not in allowed:
                names = "/".join(CLASS_NAME[c] for c in sorted(allowed))
                errors.append(
                    f"line {lineno}: seq {seq} is a {CLASS_NAME[cls]} locally, "
                    f"but the reference reports type {rtype} (a {names} message)")
                break

            if rtype in (4, 5):  # reject: `type,seq,order_id`
                fields = [("order_id", int(parts[2]), oid)]
            elif rtype == 2 and cls == MSG_NEW:
                # IOC residual — side and price are the aggressor's own, so both
                # are workload-derived. Reaching here also proves ioc is set.
                if not ioc:
                    errors.append(f"line {lineno}: seq {seq} drops an IOC residual in the "
                                  f"reference but is not IOC locally")
                    break
                fields = [("side", int(parts[2]), side), ("order_id", int(parts[3]), oid),
                          ("price_ticks", int(parts[4]), price)]
            elif rtype == 2:  # CancelAck for a cancel: side/price are the RESTING order's
                fields = [("order_id", int(parts[3]), oid)]
            elif rtype == 0:  # OrderAck: `0,seq,side,order_id,price,qty`
                fields = [("side", int(parts[2]), side), ("order_id", int(parts[3]), oid),
                          ("price_ticks", int(parts[4]), price), ("quantity", int(parts[5]), qty)]
            else:  # ModifyAck: `3,seq,side,order_id,new_price,new_qty`
                fields = [("side", int(parts[2]), side), ("order_id", int(parts[3]), oid),
                          ("price_ticks", int(parts[4]), price), ("quantity", int(parts[5]), qty)]

            for name, want, got in fields:
                checked_fields += 1
                if want != got:
                    errors.append(f"line {lineno}: seq {seq} {name}: reference={want} local={got}")
            if errors:
                break

    if errors:
        print("DIVERGED — the local workload is NOT the reference workload")
        for e in errors:
            print(f"  {e}")
        return 1

    unseen = seen.count(False)
    print(f"checked {checked_fields:,} field values across {len(wl) - unseen:,} messages")
    if unseen:
        # Every message must produce an ack or a reject, so this should be zero.
        print(f"WARNING: {unseen:,} local messages had no corresponding report")
        return 1
    print("IDENTICAL — every workload message matches the reference in every field.")
    print("The published correctness hash is reachable on this machine.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
