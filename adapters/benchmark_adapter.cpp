// benchmark_adapter.cpp — this engine, behind the flash1 benchmark C ABI.
//
// Deliberately thin. The engine already emits the ABI's six report types
// natively, so this file converts a struct layout and nothing else: it holds no
// shadow book, makes no decisions about accept-vs-reject, and never rewrites a
// report. That is the point of the seam — the binary the harness measures is
// the binary the simulator backtests against, with no logic in between that
// could differ.
//
// Two adapter disciplines from the benchmark's own docs are followed here:
//
//   1. engine_query_* forward straight to the engine's live book
//      (docs/CONFORMANCE.md, "Faithful engine-state forwarding"). An adapter
//      that answered from a parallel shadow would keep the audit passing even
//      when the engine's book was wrong, degenerating the state audit into a
//      re-check of the already-hashed report stream.
//   2. engine_prebuild is NOT exported. Our native order representation is
//      built inside the timed call, where it belongs; there is nothing honest
//      to hoist, and the harness gates a prebuild that does more than
//      translate.
//
// Build: see adapters/CMakeLists.txt, or by hand —
//   c++ -std=c++20 -O3 -march=native -fPIC -shared \
//       -I <benchmark>/api -I libmatch/include \
//       adapters/benchmark_adapter.cpp -o ome_adapter.so
#include <cstdint>
#include <optional>

#include "matching_engine_api.h"

#include "match/map_book.hpp"
#include "match/report.hpp"
#include "match/types.hpp"

namespace {

const me_transport_t* g_transport = nullptr;
void* g_report_sink = nullptr;

// Translates one report into the ABI struct and pushes it. The spin on a full
// queue is what the reference adapters do: the drainer runs on its own core and
// the transport is momentarily full, not broken.
struct TransportSink {
    void operator()(const match::Report& report) const {
        me_report_t out{};
        out.type = static_cast<std::uint8_t>(report.type);
        out.side = static_cast<std::uint8_t>(report.side);
        out.sequence_number = match::value_of(report.seq);
        out.order_id = match::value_of(report.order_id);
        out.price_ticks = report.price.ticks;
        out.quantity = report.quantity.units;
        out.maker_order_id = match::value_of(report.maker_order_id);
        out.taker_order_id = match::value_of(report.taker_order_id);

        while (g_transport->push(g_report_sink, &out) == 0) {
            // spin: the consumer is draining
        }
    }
};

using Book = match::MapBook<TransportSink>;

// std::optional rather than a static instance: engine_init must give the
// harness a genuinely empty book, and the pre-flight check asserts exactly that
// before the clock starts.
std::optional<Book> g_book;

}  // namespace

extern "C" {

void engine_init(std::uint64_t /*seed*/, const me_transport_t* transport, void* report_sink) {
    g_transport = transport;
    g_report_sink = report_sink;
    g_book.emplace(TransportSink{});
}

void engine_shutdown(void) {
    g_book.reset();
    g_transport = nullptr;
    g_report_sink = nullptr;
}

void engine_on_new_order(const new_order_t* order) {
    g_book->on_new_order(match::Order{
        match::OrderId{order->order_id},
        match::SeqNum{order->sequence_number},
        match::Price{order->price_ticks},
        match::Quantity{order->quantity},
        static_cast<match::Side>(order->side),
        order->ioc ? match::TimeInForce::Ioc : match::TimeInForce::Gtc,
    });
}

void engine_on_cancel(const cancel_t* cancel) {
    g_book->on_cancel(match::SeqNum{cancel->sequence_number}, match::OrderId{cancel->order_id});
}

void engine_on_modify(const modify_t* modify) {
    g_book->on_modify(match::SeqNum{modify->sequence_number}, match::OrderId{modify->order_id},
                      static_cast<match::Side>(modify->side),
                      match::Price{modify->new_price_ticks}, match::Quantity{modify->new_quantity});
}

// The matching core is single-threaded and synchronous: every report is already
// in the transport by the time the on_* call returns, so there is nothing to
// drain. This is a no-op honestly, not a deferral.
void engine_flush(void) {}

std::int64_t engine_query_best_bid(void) {
    return g_book->best_bid().ticks;
}

std::int64_t engine_query_best_ask(void) {
    return g_book->best_ask().ticks;
}

std::uint64_t engine_query_depth_at(std::int64_t price_ticks, std::uint8_t side) {
    return g_book->depth_at(match::Price{price_ticks}, static_cast<match::Side>(side));
}

}  // extern "C"
