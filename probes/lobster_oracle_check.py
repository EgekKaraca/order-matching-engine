#!/usr/bin/env python3
"""
Feasibility probe for the phase-5 'reconstruction oracle' idea.

Replay a LOBSTER message file into a from-scratch L3 book and diff the result
against LOBSTER's own published orderbook file, row by row.

If the top-K levels agree, the message/orderbook pairing is a usable external
correctness oracle for a matching engine's replay path.
"""
import sys
import heapq
from collections import defaultdict

import os
MSG  = os.environ.get("MSG","msg10.csv")
BOOK = os.environ.get("BOOK","book10.csv")
LEVELS = int(os.environ.get("LEVELS","10"))
LIMIT = int(sys.argv[1]) if len(sys.argv) > 1 else 0        # 0 = whole file
DENSE = int(sys.argv[2]) if len(sys.argv) > 2 else 50_000   # compare every row up to here
STRIDE = 50                                                  # then every Nth row

ASK_DUMMY, BID_DUMMY = 9999999999, -9999999999

# price -> aggregate visible size, per side
asks, bids = defaultdict(int), defaultdict(int)
# order id -> (price, size, direction)
orders = {}

stats = {
    "rows": 0, "compared": 0,
    "ok1": 0, "ok5": 0, "ok10": 0,
    "unknown_id": 0, "hidden": 0, "halt": 0,
}
first_bad = {1: None, 5: None, 10: None}


def side_of(direction):
    return bids if direction == 1 else asks


def topk(side, k, is_ask):
    prices = heapq.nsmallest(k, side) if is_ask else heapq.nlargest(k, side)
    out = []
    for p in prices:
        out.append((p, side[p]))
    while len(out) < k:
        out.append((ASK_DUMMY if is_ask else BID_DUMMY, 0))
    return out


with open(MSG) as fm, open(BOOK) as fb:
    # Seed the book from published row 0 (state AFTER message 0), then replay
    # message 1..N against book rows 1..N. Seeded levels have no per-order
    # identity, so later cancels referencing them fall back to level deduction.
    seed = [int(x) for x in fb.readline().split(",")]
    fm.readline()
    for i in range(LEVELS):
        ap, asz, bp, bsz = seed[4 * i:4 * i + 4]
        if ap != ASK_DUMMY and asz:
            asks[ap] += asz
        if bp != BID_DUMMY and bsz:
            bids[bp] += bsz

    for row, (ml, bl) in enumerate(zip(fm, fb)):
        f = ml.split(",")
        etype = int(f[1]); oid = int(f[2]); size = int(f[3])
        price = int(f[4]); direction = int(f[5])
        stats["rows"] += 1

        if etype == 7:
            stats["halt"] += 1
        elif etype == 5:
            stats["hidden"] += 1          # hidden execution: no visible book change
        elif etype == 1:
            book = side_of(direction)
            book[price] += size
            if oid in orders:
                p0, s0, d0 = orders[oid]
                orders[oid] = (p0, s0 + size, d0)
            else:
                orders[oid] = (price, size, direction)
        elif etype in (2, 3, 4):
            book = side_of(direction)
            if oid in orders:
                p0, s0, d0 = orders[oid]
                take = size if etype == 2 else min(size, s0) if etype == 4 else s0
                if etype == 3:
                    take = s0
                book[p0] -= take
                if book[p0] <= 0:
                    book.pop(p0, None)
                rem = s0 - take
                if rem <= 0:
                    orders.pop(oid, None)
                else:
                    orders[oid] = (p0, rem, d0)
            else:
                # order was submitted outside the captured price range
                stats["unknown_id"] += 1
                book[price] -= size
                if book[price] <= 0:
                    book.pop(price, None)

        # ---- compare against the published book ----
        dense = row < DENSE
        if dense or row % STRIDE == 0:
            ref = [int(x) for x in bl.split(",")]
            mine = []
            a = topk(asks, LEVELS, True)
            b = topk(bids, LEVELS, False)
            for i in range(LEVELS):
                mine += [a[i][0], a[i][1], b[i][0], b[i][1]]
            stats["compared"] += 1
            for k in (1, 5, 10):
                n = 4 * k
                if mine[:n] == ref[:n]:
                    stats[f"ok{k}"] += 1
                elif first_bad[k] is None:
                    first_bad[k] = (row, mine[:8], ref[:8])

        if LIMIT and row + 1 >= LIMIT:
            break

c = stats["compared"]
print(f"rows replayed      : {stats['rows']:,}")
print(f"rows compared      : {c:,}")
for k in (1, 5, 10):
    print(f"  top-{k:<2} exact match : {stats[f'ok{k}']:,} / {c:,}  ({100*stats[f'ok{k}']/c:.4f}%)")
print(f"hidden executions  : {stats['hidden']:,}  (skipped, invisible to the book)")
print(f"trading halts      : {stats['halt']:,}")
print(f"unknown order ids  : {stats['unknown_id']:,}  (submitted outside the level-{LEVELS} window)")
for k in (1, 5, 10):
    if first_bad[k]:
        r, m, rf = first_bad[k]
        print(f"\nfirst top-{k} mismatch at row {r:,}")
        print(f"  mine: {m}")
        print(f"  ref : {rf}")
