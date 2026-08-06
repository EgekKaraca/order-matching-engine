#include "adapters/lobster.hpp"

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace lobster {
namespace {

constexpr int kNanosecondDigits = 9;

std::string_view next_field(std::string_view& rest, bool& ok) {
    if (!ok) return {};
    const auto comma = rest.find(',');
    if (comma == std::string_view::npos) {
        const auto field = rest;
        rest = {};
        return field;
    }
    const auto field = rest.substr(0, comma);
    rest.remove_prefix(comma + 1);
    return field;
}

void trim(std::string_view& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' ||
                          s.back() == '\n')) {
        s.remove_suffix(1);
    }
}

template <typename T>
bool parse_int(std::string_view field, T& out) {
    trim(field);
    if (field.empty()) return false;
    const char* begin = field.data();
    const char* end = field.data() + field.size();
    if constexpr (std::is_unsigned_v<T>) {
        // from_chars refuses a leading '+' but accepts '-' into an unsigned
        // type; these columns are never negative, so reject the sign outright.
        if (field.front() == '-') return false;
    }
    const auto [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc{} && ptr == end;
}

// "34200.000000001" -> 34200000000001. Parsed as two integers rather than a
// double: a double carries 52 bits of mantissa, and a full trading day in
// nanoseconds needs 46 — it fits today, but the moment anyone widens the
// window or the precision it silently stops fitting. Integers do not have that
// failure mode.
bool parse_time_ns(std::string_view field, std::int64_t& out) {
    trim(field);
    const auto dot = field.find('.');
    const auto seconds_text = field.substr(0, dot);

    std::int64_t seconds = 0;
    if (!parse_int(seconds_text, seconds)) return false;

    std::int64_t fraction = 0;
    if (dot != std::string_view::npos) {
        auto digits = field.substr(dot + 1);
        if (digits.size() > kNanosecondDigits) digits = digits.substr(0, kNanosecondDigits);
        if (!digits.empty() && !parse_int(digits, fraction)) return false;
        for (std::size_t i = digits.size(); i < kNanosecondDigits; ++i) fraction *= 10;
    }

    out = seconds * 1'000'000'000 + fraction;
    return true;
}

}  // namespace

bool parse_message(std::string_view line, Message& out) {
    trim(line);
    if (line.empty()) return false;

    bool ok = true;
    std::string_view rest = line;

    const auto time_field = next_field(rest, ok);
    const auto type_field = next_field(rest, ok);
    const auto id_field = next_field(rest, ok);
    const auto size_field = next_field(rest, ok);
    const auto price_field = next_field(rest, ok);
    const auto direction_field = next_field(rest, ok);

    Message parsed;
    std::int64_t type = 0;
    std::int64_t direction = 0;

    if (!parse_time_ns(time_field, parsed.time_ns)) return false;
    if (!parse_int(type_field, type)) return false;
    if (!parse_int(id_field, parsed.order_id)) return false;
    if (!parse_int(size_field, parsed.size)) return false;
    if (!parse_int(price_field, parsed.price)) return false;
    if (!parse_int(direction_field, direction)) return false;

    if (type < 1 || type > 7) return false;
    if (direction != 1 && direction != -1) return false;

    parsed.type = static_cast<EventType>(type);
    parsed.side = direction == 1 ? match::Side::Buy : match::Side::Sell;

    out = parsed;
    return true;
}

bool parse_orderbook_row(std::string_view line, std::int64_t* out, std::size_t levels) {
    trim(line);
    bool ok = true;
    std::string_view rest = line;
    for (std::size_t i = 0; i < 4 * levels; ++i) {
        const auto field = next_field(rest, ok);
        if (field.empty() || !parse_int(field, out[i])) return false;
    }
    // A row with more columns than the caller asked for is the wrong file —
    // most likely a level-10 orderbook paired with a level-50 message file.
    return rest.empty();
}

}  // namespace lobster
