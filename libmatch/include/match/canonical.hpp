// canonical.hpp — the benchmark's canonical serialisation of a report stream.
//
// The correctness oracle is the SHA-256 of this exact text, so the format is a
// contract down to the byte. From docs/METHODOLOGY.md of the benchmark repo:
//
//     0  OrderAck      0,seq,side,order_id,price_ticks,quantity
//     1  Trade         1,seq,price_ticks,quantity,maker_order_id,taker_order_id
//     2  CancelAck     2,seq,side,order_id,price_ticks
//     3  ModifyAck     3,seq,side,order_id,price_ticks,quantity
//     4  CancelReject  4,seq,order_id
//     5  ModifyReject  5,seq,order_id
//
// joined by '\n' with NO trailing newline, after a STABLE sort by
// (sequence_number, type).
//
// Why the sort matters: it makes the canonical form independent of the order in
// which an engine emits one message's reports — emitting a ModifyAck before or
// after that modify's trades serialises identically. What it does NOT make
// independent is the order of trades within a single message: the sort is
// stable, so fills must still be emitted in match order. That is the whole
// point of price-time priority, and it is the part a wrong engine gets wrong.
//
// This lives in libmatch rather than in the adapter because it is also the
// project's own debugging instrument: replaying a workload locally and diffing
// this text against the published canonical output is how a divergence gets
// localised, with or without the harness.
#ifndef MATCH_CANONICAL_HPP
#define MATCH_CANONICAL_HPP

#include <iosfwd>
#include <string>
#include <vector>

#include "match/report.hpp"

namespace match::canonical {

// One report as its canonical line, without the newline.
std::string line(const Report& report);

// Stable sort by (seq, type) — the canonical ordering. In place, because a
// full workload is ~2.2M reports and copying them is pure waste.
void sort(std::vector<Report>& reports);

// Sorts `reports`, then writes the canonical text. No trailing newline: the
// hash is over exactly these bytes.
void write(std::ostream& out, std::vector<Report>& reports);

}  // namespace match::canonical

#endif  // MATCH_CANONICAL_HPP
