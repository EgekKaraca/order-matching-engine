// abi_replay.cpp — replay a workload through the adapter's C ABI, not the C++
// engine directly.
//
// tools/replay proves the ENGINE reproduces the published hash. It says nothing
// about the adapter, which is the part that actually runs under the harness and
// the part most likely to be wrong: a mistranslated side byte, a report field
// left unset, a symbol not exported with C linkage. Those all fail on the Linux
// box, days later, with the harness in the loop confusing the diagnosis.
//
// So this dlopen()s the built adapter exactly as the harness does, supplies a
// minimal me_transport_t, drives the same workload, and hashes the same
// canonical text. If this matches and tools/replay matches, the only untested
// things left on the ABI path are the harness's own timing, threading, and
// audit machinery.
//
// It is NOT the harness: single-threaded, untimed, no anti-cheat, no
// random-point state audit against a baseline engine.
#include "matching_engine_api.h"

#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "match/canonical.hpp"
#include "match/report.hpp"
#include "match/types.hpp"

namespace {

constexpr std::size_t kRecordSize = 40;
constexpr std::uint64_t kMagic = 0x4D4542575F303031ULL;

enum : std::uint8_t { kMsgNew = 0, kMsgCancel = 1, kMsgModify = 2 };

struct Message {
    std::uint8_t type;
    std::uint8_t side;
    std::uint8_t ioc;
    std::uint32_t quantity;
    std::uint64_t seq;
    std::uint64_t order_id;
    std::int64_t price_ticks;
};

template <typename T>
T read_at(const char* base, std::size_t offset) {
    T value;
    std::memcpy(&value, base + offset, sizeof(T));
    return value;
}

bool load_workload(const std::string& path, std::vector<Message>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << '\n';
        return false;
    }
    char header[16];
    in.read(header, sizeof(header));
    if (!in || read_at<std::uint64_t>(header, 0) != kMagic) {
        std::cerr << "bad or truncated workload header\n";
        return false;
    }
    const auto count = read_at<std::uint32_t>(header, 12);

    std::string blob(static_cast<std::size_t>(count) * kRecordSize, '\0');
    in.read(blob.data(), static_cast<std::streamsize>(blob.size()));
    if (in.gcount() != static_cast<std::streamsize>(blob.size())) {
        std::cerr << "truncated workload body\n";
        return false;
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const char* rec = blob.data() + static_cast<std::size_t>(i) * kRecordSize;
        out.push_back(Message{read_at<std::uint8_t>(rec, 0), read_at<std::uint8_t>(rec, 1),
                              read_at<std::uint8_t>(rec, 2), read_at<std::uint32_t>(rec, 4),
                              read_at<std::uint64_t>(rec, 8), read_at<std::uint64_t>(rec, 16),
                              read_at<std::int64_t>(rec, 24)});
    }
    return true;
}

// The simplest transport that satisfies the contract: unbounded, so push never
// fails and the adapter's spin loop never spins. The harness's real transport
// is a bounded SPSC queue drained on another core; the difference is timing
// behaviour, not report content.
std::vector<me_report_t> g_drained;

void* transport_create(std::uint32_t capacity) {
    g_drained.clear();
    g_drained.reserve(capacity);
    return &g_drained;
}

int transport_push(void* handle, const me_report_t* report) {
    static_cast<std::vector<me_report_t>*>(handle)->push_back(*report);
    return 1;
}

std::uint32_t transport_drain(void*, me_report_t*, std::uint32_t) {
    return 0;  // this tool reads the vector directly after the run
}

void transport_flush(void*) {}
void transport_destroy(void*) {}

const me_transport_t kTransport{transport_create, transport_push, transport_drain, transport_flush,
                                transport_destroy};

template <typename Fn>
Fn* required_symbol(void* handle, const char* name) {
    // The harness resolves the same names; a missing one here is a missing
    // `extern "C"` there.
    auto* sym = reinterpret_cast<Fn*>(dlsym(handle, name));
    if (sym == nullptr) std::cerr << "missing required symbol: " << name << '\n';
    return sym;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: abi_replay <adapter.so> <workload.bin> <out.txt>\n";
        return 2;
    }

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        std::cerr << "dlopen failed: " << dlerror() << '\n';
        return 1;
    }

    auto* init =
        required_symbol<void(std::uint64_t, const me_transport_t*, void*)>(lib, "engine_init");
    auto* shutdown = required_symbol<void()>(lib, "engine_shutdown");
    auto* on_new = required_symbol<void(const new_order_t*)>(lib, "engine_on_new_order");
    auto* on_cancel = required_symbol<void(const cancel_t*)>(lib, "engine_on_cancel");
    auto* on_modify = required_symbol<void(const modify_t*)>(lib, "engine_on_modify");
    auto* flush = required_symbol<void()>(lib, "engine_flush");
    auto* best_bid = required_symbol<std::int64_t()>(lib, "engine_query_best_bid");
    auto* best_ask = required_symbol<std::int64_t()>(lib, "engine_query_best_ask");
    auto* depth_at =
        required_symbol<std::uint64_t(std::int64_t, std::uint8_t)>(lib, "engine_query_depth_at");

    if (!init || !shutdown || !on_new || !on_cancel || !on_modify || !flush || !best_bid ||
        !best_ask || !depth_at) {
        return 1;
    }

    std::vector<Message> messages;
    if (!load_workload(argv[2], messages)) return 1;
    std::cout << "loaded " << messages.size() << " messages\n";

    init(0, &kTransport, transport_create(static_cast<std::uint32_t>(messages.size() * 2)));

    // The harness asserts an empty book here, before the clock starts, to catch
    // an engine that rested orders during the optional prebuild pass.
    if (best_bid() != INT64_MIN || best_ask() != INT64_MAX) {
        std::cerr << "pre-start book is not empty by the API sentinels\n";
        return 1;
    }

    for (const Message& m : messages) {
        switch (m.type) {
            case kMsgNew: {
                const new_order_t order{m.order_id, m.seq, m.price_ticks, m.quantity, m.side,
                                        m.ioc,      {}};
                on_new(&order);
                break;
            }
            case kMsgCancel: {
                const cancel_t cancel{m.order_id, m.seq};
                on_cancel(&cancel);
                break;
            }
            case kMsgModify: {
                const modify_t modify{m.order_id, m.seq, m.price_ticks, m.quantity, m.side, {}};
                on_modify(&modify);
                break;
            }
            default:
                std::cerr << "unknown message type\n";
                return 1;
        }
    }
    flush();

    // Translate back out of the ABI struct so the canonical text is produced by
    // the same code path tools/replay uses — the comparison is then about the
    // adapter alone.
    std::vector<match::Report> reports;
    reports.reserve(g_drained.size());
    for (const me_report_t& r : g_drained) {
        reports.push_back(
            match::Report{static_cast<match::ReportType>(r.type), static_cast<match::Side>(r.side),
                          match::SeqNum{r.sequence_number}, match::OrderId{r.order_id},
                          match::Price{r.price_ticks}, match::Quantity{r.quantity},
                          match::OrderId{r.maker_order_id}, match::OrderId{r.taker_order_id}});
    }
    std::cout << "reports: " << reports.size() << '\n';

    std::ofstream out(argv[3], std::ios::binary);
    if (!out) {
        std::cerr << "cannot write " << argv[3] << '\n';
        return 1;
    }
    match::canonical::write(out, reports);
    out.close();

    std::cout << "queries through the ABI: best_bid=" << best_bid() << " best_ask=" << best_ask()
              << " depth_at(best_bid,buy)=" << depth_at(best_bid(), 0) << '\n';

    shutdown();
    dlclose(lib);
    return 0;
}
