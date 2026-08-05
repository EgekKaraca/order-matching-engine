#include "match/canonical.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <ostream>
#include <string>
#include <system_error>
#include <vector>

namespace match::canonical {
namespace {

// std::to_chars rather than iostreams or std::format: this runs 2.2M times per
// workload replay, and Apple clang 15's libc++ has no <format> anyway.
void append_int(std::string& out, std::int64_t value) {
    std::array<char, 24> buf{};
    const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    out.append(buf.data(), end);
}

void append_uint(std::string& out, std::uint64_t value) {
    std::array<char, 24> buf{};
    const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    out.append(buf.data(), end);
}

void append_line(std::string& out, const Report& r) {
    const auto type = static_cast<std::uint64_t>(r.type);
    const auto side = static_cast<std::uint64_t>(r.side);

    append_uint(out, type);
    out += ',';
    append_uint(out, value_of(r.seq));

    switch (r.type) {
        case ReportType::OrderAck:
        case ReportType::ModifyAck:
            out += ',';
            append_uint(out, side);
            out += ',';
            append_uint(out, value_of(r.order_id));
            out += ',';
            append_int(out, r.price.ticks);
            out += ',';
            append_uint(out, r.quantity.units);
            break;

        case ReportType::Trade:
            out += ',';
            append_int(out, r.price.ticks);
            out += ',';
            append_uint(out, r.quantity.units);
            out += ',';
            append_uint(out, value_of(r.maker_order_id));
            out += ',';
            append_uint(out, value_of(r.taker_order_id));
            break;

        case ReportType::CancelAck:
            out += ',';
            append_uint(out, side);
            out += ',';
            append_uint(out, value_of(r.order_id));
            out += ',';
            append_int(out, r.price.ticks);
            break;

        case ReportType::CancelReject:
        case ReportType::ModifyReject:
            out += ',';
            append_uint(out, value_of(r.order_id));
            break;
    }
}

}  // namespace

std::string line(const Report& report) {
    std::string out;
    out.reserve(64);
    append_line(out, report);
    return out;
}

void sort(std::vector<Report>& reports) {
    std::stable_sort(reports.begin(), reports.end(), [](const Report& a, const Report& b) {
        if (a.seq != b.seq) return value_of(a.seq) < value_of(b.seq);
        return a.type < b.type;
    });
}

void write(std::ostream& out, std::vector<Report>& reports) {
    sort(reports);

    // Batch into a buffer rather than one << per field: the same text, a lot
    // fewer stream operations.
    std::string buf;
    buf.reserve(1u << 16);
    for (std::size_t i = 0; i < reports.size(); ++i) {
        if (i != 0) buf += '\n';
        append_line(buf, reports[i]);
        if (buf.size() >= (1u << 15)) {
            out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            buf.clear();
        }
    }
    out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
}

}  // namespace match::canonical
