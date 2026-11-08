// replay.cpp — drive a benchmark workload through the engine and write the
// canonical report text.
//
// This is the local half of the correctness oracle. The harness itself is
// Linux-only, but its workload generator is not, and probes/workload_parity_
// check.py established that a workload generated here is bit-identical to the
// one the published hashes were computed against. So:
//
//     generator normal normal.bin 1000000 23
//     replay normal.bin out.txt
//     shasum -a 256 out.txt        # compare to reference/correctness_hash.txt
//
// gets all the way to the benchmark's `Status: PASS` — the report-stream half
// of the verdict — without leaving the development machine. What still needs
// the Linux box: the timed run, the random-point state audit, and the
// anti-cheat checks that together make it `Verdict: VALID`.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "match/arena_book.hpp"
#include "match/canonical.hpp"
#include "match/map_book.hpp"
#include "match/report.hpp"
#include "match/types.hpp"

namespace {

// The on-disk workload record, from the generator's write_binary():
//   [u8 type][u8 side][u8 ioc][u8 pad][u32 qty][u64 seq][u64 order_id]
//   [i64 price_ticks][i64 reserved]
constexpr std::size_t kRecordSize = 40;
constexpr std::uint64_t kMagic = 0x4D4542575F303031ULL;  // "MEBW_001"

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
    if (!in) {
        std::cerr << "truncated header\n";
        return false;
    }
    if (read_at<std::uint64_t>(header, 0) != kMagic) {
        std::cerr << "bad magic — not a workload file\n";
        return false;
    }
    const auto count = read_at<std::uint32_t>(header, 12);

    std::string blob(static_cast<std::size_t>(count) * kRecordSize, '\0');
    in.read(blob.data(), static_cast<std::streamsize>(blob.size()));
    if (in.gcount() != static_cast<std::streamsize>(blob.size())) {
        std::cerr << "truncated: wanted " << blob.size() << " bytes, got " << in.gcount() << '\n';
        return false;
    }

    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const char* rec = blob.data() + static_cast<std::size_t>(i) * kRecordSize;
        out.push_back(Message{
            read_at<std::uint8_t>(rec, 0),
            read_at<std::uint8_t>(rec, 1),
            read_at<std::uint8_t>(rec, 2),
            read_at<std::uint32_t>(rec, 4),
            read_at<std::uint64_t>(rec, 8),
            read_at<std::uint64_t>(rec, 16),
            read_at<std::int64_t>(rec, 24),
        });
    }
    return true;
}

struct CollectingSink {
    std::vector<match::Report>* out;
    void operator()(const match::Report& r) const { out->push_back(r); }
};

// Both engines are driven through the identical loop, so a throughput
// comparison between them is a comparison of the engines and not of two
// different harnesses.
template <typename Book>
void drive(Book& book, const std::vector<Message>& messages) {
    for (const Message& m : messages) {
        const auto side = static_cast<match::Side>(m.side);
        switch (m.type) {
            case kMsgNew:
                book.on_new_order(
                    match::Order{match::OrderId{m.order_id}, match::SeqNum{m.seq},
                                 match::Price{m.price_ticks}, match::Quantity{m.quantity}, side,
                                 m.ioc ? match::TimeInForce::Ioc : match::TimeInForce::Gtc});
                break;
            case kMsgCancel:
                book.on_cancel(match::SeqNum{m.seq}, match::OrderId{m.order_id});
                break;
            case kMsgModify:
                book.on_modify(match::SeqNum{m.seq}, match::OrderId{m.order_id}, side,
                               match::Price{m.price_ticks}, match::Quantity{m.quantity});
                break;
            default:
                break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: replay <workload.bin> <out.txt> [map|arena]\n";
        return 2;
    }

    std::vector<Message> messages;
    if (!load_workload(argv[1], messages)) return 1;
    std::cout << "loaded " << messages.size() << " messages\n";

    std::vector<match::Report> reports;
    reports.reserve(messages.size() + messages.size() / 4);

    const std::string engine = argc > 3 ? argv[3] : "map";
    if (engine != "map" && engine != "arena") {
        std::cerr << "unknown engine '" << engine << "' — expected map or arena\n";
        return 2;
    }

    const auto started = std::chrono::steady_clock::now();
    std::int64_t best_bid = 0;
    std::int64_t best_ask = 0;
    std::size_t resting = 0;
    bool valid = false;

    if (engine == "arena") {
        match::ArenaBook<CollectingSink> book{CollectingSink{&reports}, 1u << 20};
        drive(book, messages);
        best_bid = book.best_bid().ticks;
        best_ask = book.best_ask().ticks;
        resting = book.resting_count();
        valid = book.validate();
    } else {
        match::MapBook<CollectingSink> book{CollectingSink{&reports}};
        drive(book, messages);
        best_bid = book.best_bid().ticks;
        best_ask = book.best_ask().ticks;
        resting = book.resting_count();
        valid = book.validate();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();

    // Per-type counts. Cheap, and they localise a whole class of divergence
    // before hashing anything: the reference's own counts for normal/seed 23
    // are 1000000 acks, 62474 trades, 931363 cancel-acks, 164857 modify-acks,
    // 38131 cancel-rejects, 7818 modify-rejects.
    std::size_t by_type[6] = {};
    for (const auto& r : reports) ++by_type[static_cast<std::size_t>(r.type)];
    static const char* kNames[] = {"OrderAck",  "Trade",        "CancelAck",
                                   "ModifyAck", "CancelReject", "ModifyReject"};
    std::cout << "reports: " << reports.size() << '\n';
    for (std::size_t i = 0; i < 6; ++i) {
        std::cout << "  " << kNames[i] << ": " << by_type[i] << '\n';
    }

    std::ofstream out(argv[2], std::ios::binary);
    if (!out) {
        std::cerr << "cannot write " << argv[2] << '\n';
        return 1;
    }
    match::canonical::write(out, reports);
    out.close();
    if (!out) {
        std::cerr << "write failed\n";
        return 1;
    }

    std::cout << "wrote " << argv[2] << '\n';
    std::cout << "book after replay: best_bid=" << best_bid << " best_ask=" << best_ask
              << " resting=" << resting << " valid=" << (valid ? "yes" : "NO") << '\n';
    // A local, same-machine number only. It is NOT comparable to the published
    // figures: those are measured by the harness on Graviton4, open-loop and
    // coordinated-omission-free, and include report transport. This is a
    // stopwatch around a loop with an in-process vector sink.
    std::cout << "engine=" << engine << " matched " << messages.size() << " messages in " << seconds
              << "s = "
              << (seconds > 0 ? static_cast<double>(messages.size()) / seconds / 1e6 : 0.0)
              << " M msg/s (LOCAL, not the harness number)\n";
    return 0;
}
