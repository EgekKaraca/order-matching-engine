// lobster_events.hpp — LOBSTER messages to the simulator's normalised events.
//
// This is where a venue's encoding stops and the simulator's vocabulary
// starts. Everything venue-specific — the dollars-x-10000 price scale, the
// direction convention, which event types are invisible — is handled on this
// side of the line so that libsim never learns what LOBSTER is.
#ifndef ADAPTERS_LOBSTER_EVENTS_HPP
#define ADAPTERS_LOBSTER_EVENTS_HPP

#include <optional>

#include "adapters/lobster.hpp"
#include "sim/types.hpp"

namespace lobster {

// Returns nullopt for events that leave the displayed book untouched — hidden
// executions, crosses, halts. Dropping them here means the simulator never has
// to know they exist.
inline std::optional<sim::MarketEvent> to_market_event(const Message& m) {
    if (!m.touches_visible_book()) return std::nullopt;

    sim::MarketEvent event;
    event.time = sim::Time{m.time_ns};
    event.id = match::OrderId{m.order_id};
    event.price = match::Price{m.price};
    event.quantity = match::Quantity{m.size};
    // For every LOBSTER event type this is the side of the order named by the
    // message. On an execution that is the MAKER — the order being executed —
    // which is exactly what sim::MarketEvent documents it to mean.
    event.side = m.side;

    switch (m.type) {
        case EventType::NewLimitOrder:
            event.type = sim::MarketEventType::Add;
            break;
        case EventType::PartialCancel:
            event.type = sim::MarketEventType::Reduce;
            break;
        case EventType::Delete:
            event.type = sim::MarketEventType::Delete;
            break;
        case EventType::VisibleExecution:
            event.type = sim::MarketEventType::Execute;
            break;
        default:
            return std::nullopt;
    }
    return event;
}

}  // namespace lobster

#endif  // ADAPTERS_LOBSTER_EVENTS_HPP
