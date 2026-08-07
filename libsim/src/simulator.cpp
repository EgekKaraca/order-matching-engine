#include "sim/simulator.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace sim {

// The historical book, seen read-only, for models that estimate rather than
// participate.
struct Simulator::Probe final : BookProbe {
    explicit Probe(const Simulator* sim) : owner(sim) {}
    const Simulator* owner;
    match::AggregateQuantity depth_at(match::Price price, match::Side side) const override {
        return owner->book_.depth_at(price, side);
    }
    match::Price best_bid() const override { return owner->book_.best_bid(); }
    match::Price best_ask() const override { return owner->book_.best_ask(); }
};

Simulator::Simulator(Strategy& strategy, DelayModel& feed_delay, DelayModel& order_delay,
                     FillModel& fill_model, SimulatorConfig config, ReactionModel* reaction)
    : strategy_(strategy),
      fill_model_(fill_model),
      reaction_(reaction),
      feed_delay_(feed_delay),
      order_delay_(order_delay),
      config_(config),
      book_(TradeSink{this}),
      portfolio_(config.fees) {
    // Ids come from the gate, which owns the counter.
    gate_.set_next_id(config.strategy_id_base);
}

void Simulator::seed_level(match::Side side, match::Price price, match::Quantity quantity) {
    book_.on_new_order(match::Order{match::OrderId{next_seed_id_++}, match::SeqNum{0}, price,
                                    quantity, side, match::TimeInForce::Gtc});
}

bool Simulator::suppressed_by_reaction(const MarketEvent& event) {
    if (reaction_ == nullptr || event.type != MarketEventType::Add) return false;

    // Level-local only. Suppressing adds at other prices would be rewriting
    // the market rather than modelling a reaction to our presence.
    match::AggregateQuantity strategy_size = 0;
    for (const auto& [id, ahead] : live_orders_) {
        (void)ahead;
        const auto side = order_sides_.find(id);
        if (side == order_sides_.end() || side->second != event.side) continue;
        const auto posted = posted_index_.find(id);
        if (posted == posted_index_.end()) continue;
        if (posted_orders_[posted->second].price != event.price) continue;
        const auto remaining = remaining_.find(id);
        if (remaining == remaining_.end()) continue;
        strategy_size += remaining->second.units;
    }
    if (strategy_size == 0) return false;

    const auto depth = book_.depth_at(event.price, event.side);
    if (!reaction_->crowds_out(event.time, event.side, event.price, strategy_size, depth)) {
        return false;
    }
    ++stats_.crowded_out;
    return true;
}

void Simulator::on_market_event(const MarketEvent& event) {
    now_ = event.time;
    ++stats_.events;

    // Order of operations matters and is not arbitrary. Strategy orders whose
    // arrival time has come must reach the book BEFORE this event is applied:
    // an order that arrived at 10:00:00.5 was in the queue when the 10:00:01
    // execution swept it, and applying it afterwards would silently save it
    // from every fill it should have taken.
    release_orders(now_);

    // A participant who would not have joined our level, had we not been in
    // it, simply does not appear.
    if (suppressed_by_reaction(event)) {
        publish_snapshot(now_);
        deliver_due(now_);
        return;
    }

    switch (event.type) {
        case MarketEventType::Add:
            book_.on_new_order(match::Order{event.id, match::SeqNum{0}, event.price,
                                            event.quantity, event.side,
                                            match::TimeInForce::Gtc});
            break;

        case MarketEventType::Reduce:
        case MarketEventType::Delete: {
            const match::Quantity got = book_.reduce(event.id, event.quantity);
            if (got < event.quantity) {
                // Liquidity whose arrival was never captured. Drain the level.
                book_.consume_at(event.side, event.price, event.quantity - got);
            }
            break;
        }

        case MarketEventType::Execute: {
            // The aggressor that caused this execution, replayed as a real
            // incoming order so the engine's FIFO decides who fills. This is
            // the whole mechanism: if a strategy order sits ahead of the
            // historical maker in the queue, the strategy fills instead — which
            // is exactly what would have happened.
            //
            // IOC, because an execution consumes what is there and leaves
            // nothing resting; a GTC residual would invent liquidity.
            book_.on_new_order(match::Order{match::OrderId{config_.aggressor_id},
                                            match::SeqNum{0}, event.price, event.quantity,
                                            match::opposite(event.side),
                                            match::TimeInForce::Ioc});
            break;
        }
    }

    if (!fill_model_.uses_engine()) apply_model_fills(event);

    publish_snapshot(now_);
    deliver_due(now_);
}

// Displayed size at the touch on the side this order will trade against. The
// fill-probability surface needs it: a thin opposite side is what pulls the
// price through a resting order.
match::AggregateQuantity Simulator::opposite_touch_depth(match::Side side) const {
    if (side == match::Side::Buy) {
        const auto best = book_.best_ask();
        return best == match::Price::no_ask() ? 0 : book_.depth_at(best, match::Side::Sell);
    }
    const auto best = book_.best_bid();
    return best == match::Price::no_bid() ? 0 : book_.depth_at(best, match::Side::Buy);
}

void Simulator::record_fill(match::OrderId id, match::Side side, match::Price price,
                            match::Quantity quantity, match::AggregateQuantity ahead_at_entry,
                            bool maker) {
    Fill fill;
    fill.time = now_;
    fill.id = id;
    fill.side = side;
    fill.price = price;
    fill.quantity = quantity;
    fill.maker = maker;
    fill.queue_ahead_at_entry = ahead_at_entry;
    const auto behind = behind_at_entry_.find(id);
    fill.queue_behind_at_entry = behind == behind_at_entry_.end() ? 0 : behind->second;

    portfolio_.apply(fill);
    fills_.push_back(fill);
    ++stats_.strategy_fills;
    if (maker) {
        ++stats_.maker_fills;
    } else {
        ++stats_.taker_fills;
    }

    // The strategy learns about it only after feed latency — a fill report is
    // a message from the venue like any other.
    fill_queue_.push_back(PendingFill{now_ + feed_delay_.sample(now_), fill});

    const auto posted = posted_index_.find(id);
    if (posted != posted_index_.end()) {
        posted_orders_[posted->second].filled_quantity += quantity;
        posted_orders_[posted->second].retired_at = now_;
    }
}

void Simulator::on_report(const match::Report& report) {
    if (report.type != match::ReportType::Trade) return;

    // The strategy as the AGGRESSOR. A marketable order trades inside
    // on_new_order, before it could be registered as live, so it is recognised
    // through in_flight_ instead. Queue ahead is zero by definition: a taker
    // does not queue, which is exactly what it pays the spread for.
    if (in_flight_active_ && report.taker_order_id == in_flight_) {
        record_fill(in_flight_, order_sides_[in_flight_], report.price, report.quantity, 0,
                    /*maker=*/false);
        return;
    }

    // Otherwise only the strategy's own resting orders are of interest; the
    // historical crowd's trades are scenery.
    const auto maker = live_orders_.find(report.maker_order_id);
    if (maker == live_orders_.end()) return;

    record_fill(report.maker_order_id, order_sides_[report.maker_order_id], report.price,
                report.quantity, maker->second, /*maker=*/true);

    // If the order is now gone, stop tracking it. The engine has already
    // removed it; this only keeps the mirror tidy.
    if (!book_.quantity_ahead(report.maker_order_id).has_value()) {
        live_orders_.erase(maker);
        order_sides_.erase(report.maker_order_id);
        behind_at_entry_.erase(report.maker_order_id);
        posted_index_.erase(report.maker_order_id);
        gone_queue_.push_back(
            PendingGone{now_ + feed_delay_.sample(now_), report.maker_order_id, true});
        ++stats_.orders_retired;
    }
}

void Simulator::apply_model_fills(const MarketEvent& event) {
    const Probe probe{this};
    model_fills_.clear();
    fill_model_.on_event(event, probe, model_fills_);

    for (const ModelFill& filled : model_fills_) {
        const auto tracked = live_orders_.find(filled.id);
        if (tracked == live_orders_.end()) continue;

        record_fill(filled.id, order_sides_[filled.id], filled.price, filled.quantity,
                    tracked->second, /*maker=*/true);

        // The model owns the remaining quantity; when it stops reporting the
        // order it is done with it.
        if (fill_model_.queue_ahead(filled.id) == 0 && remaining_[filled.id] <= filled.quantity) {
            live_orders_.erase(tracked);
            order_sides_.erase(filled.id);
            behind_at_entry_.erase(filled.id);
            posted_index_.erase(filled.id);
            gone_queue_.push_back(PendingGone{now_ + feed_delay_.sample(now_), filled.id, true});
            ++stats_.orders_retired;
            remaining_.erase(filled.id);
        } else {
            remaining_[filled.id] = remaining_[filled.id] - filled.quantity;
        }
    }
}

void Simulator::release_orders(Time now) {
    // Cancels first: within the same instant, a cancel the strategy sent
    // earlier should not be defeated by a resubmission it sent later.
    while (!pending_cancels_.empty() && pending_cancels_.front().arrival <= now) {
        const auto cancel = pending_cancels_.front();
        pending_cancels_.pop_front();

        if (live_orders_.erase(cancel.id) == 0) {
            // Already filled or already gone — the classic "too late to
            // cancel", and a real cost of order latency rather than an error.
            ++stats_.rejected_cancels;
            continue;
        }
        order_sides_.erase(cancel.id);
        remaining_.erase(cancel.id);
        behind_at_entry_.erase(cancel.id);
        const auto retiring = posted_index_.find(cancel.id);
        if (retiring != posted_index_.end()) posted_orders_[retiring->second].retired_at = now;
        posted_index_.erase(cancel.id);
        if (fill_model_.uses_engine()) {
            book_.on_cancel(match::SeqNum{0}, cancel.id);
        } else {
            fill_model_.on_removed(cancel.id);
        }
        gone_queue_.push_back(PendingGone{now + feed_delay_.sample(now), cancel.id, false});
        ++stats_.orders_retired;
    }

    while (!pending_submits_.empty() && pending_submits_.front().arrival <= now) {
        const auto submit = pending_submits_.front();
        pending_submits_.pop_front();

        // Record the queue depth ahead of it AT ENTRY. This is the number a
        // naive fill model assumes is zero, and capturing it here — rather
        // than reconstructing it later — is what makes the comparison in the
        // study a measurement rather than an estimate.
        // A marketable order always goes through the engine, whatever the fill
        // model is. There is no queue to model when you take liquidity — the
        // three models agree by construction — so routing it through the real
        // book is both correct and the only way to know what it actually got.
        if (submit.tif == match::TimeInForce::Ioc) {
            order_sides_[submit.id] = submit.side;
            in_flight_ = submit.id;
            in_flight_active_ = true;
            book_.on_new_order(match::Order{submit.id, match::SeqNum{0}, submit.price,
                                            submit.quantity, submit.side,
                                            match::TimeInForce::Ioc});
            in_flight_active_ = false;
            order_sides_.erase(submit.id);
            // IOC never rests, so it is retired the moment it is done.
            gone_queue_.push_back(PendingGone{now + feed_delay_.sample(now), submit.id, true});
            ++stats_.orders_retired;
            continue;
        }

        if (fill_model_.uses_engine()) {
            book_.on_new_order(match::Order{submit.id, match::SeqNum{0}, submit.price,
                                            submit.quantity, submit.side,
                                            match::TimeInForce::Gtc});
            const auto ahead = book_.quantity_ahead(submit.id);
            if (ahead.has_value()) {
                live_orders_[submit.id] = *ahead;
                order_sides_[submit.id] = submit.side;
                remaining_[submit.id] = submit.quantity;
                // Everything at the level except this order and the part ahead
                // of it. depth_at includes our own size, so subtract it.
                const auto level = book_.depth_at(submit.price, submit.side);
                const auto own = static_cast<match::AggregateQuantity>(submit.quantity.units);
                behind_at_entry_[submit.id] = level > *ahead + own ? level - *ahead - own : 0;

                PostedOrder record;
                record.id = submit.id;
                record.posted_at = now;
                record.side = submit.side;
                record.price = submit.price;
                record.quantity = submit.quantity;
                record.queue_ahead = *ahead;
                record.queue_behind = behind_at_entry_[submit.id];
                record.opposite_depth = opposite_touch_depth(submit.side);
                posted_index_[submit.id] = posted_orders_.size();
                posted_orders_.push_back(record);
            }
            // If it has no queue position it filled instantly on arrival, and
            // on_report has already accounted for it.
        } else {
            // A counterfactual model: the order stays OUT of the book, so the
            // depth these models read is the historical depth, unperturbed.
            const Probe probe{this};
            fill_model_.on_placed(submit.id, submit.side, submit.price, submit.quantity, probe);
            live_orders_[submit.id] = probe.depth_at(submit.price, submit.side);
            order_sides_[submit.id] = submit.side;
            remaining_[submit.id] = submit.quantity;
            // The counterfactual models keep their orders out of the book, so
            // nothing can queue behind an order that is not there.
            behind_at_entry_[submit.id] = 0;

            PostedOrder record;
            record.id = submit.id;
            record.posted_at = now;
            record.side = submit.side;
            record.price = submit.price;
            record.quantity = submit.quantity;
            record.queue_ahead = live_orders_[submit.id];
            record.queue_behind = 0;
            record.opposite_depth = opposite_touch_depth(submit.side);
            posted_index_[submit.id] = posted_orders_.size();
            posted_orders_.push_back(record);
        }
    }
}

void Simulator::publish_snapshot(Time as_of) {
    VisibleUpdate update;
    update.as_of = as_of;
    update.visible_at = as_of + feed_delay_.sample(as_of);
    update.best_bid = book_.best_bid();
    update.best_ask = book_.best_ask();
    update.bid_size = update.best_bid == match::Price::no_bid()
                          ? 0
                          : book_.depth_at(update.best_bid, match::Side::Buy);
    update.ask_size = update.best_ask == match::Price::no_ask()
                          ? 0
                          : book_.depth_at(update.best_ask, match::Side::Sell);
    feed_queue_.push_back(update);

    if (update.best_bid != match::Price::no_bid() && update.best_ask != match::Price::no_ask()) {
        mid_series_.emplace_back(as_of, update.best_bid.ticks + update.best_ask.ticks);
        touch_depths_.push_back((update.bid_size + update.ask_size) / 2);
    }
}

BookView Simulator::current_view(Time now) const {
    return BookView{last_view_.as_of(),  now,
                    last_view_.best_bid(), last_view_.bid_size(),
                    last_view_.best_ask(), last_view_.ask_size()};
}

void Simulator::deliver_due(Time now) {
    bool book_changed = false;
    while (!feed_queue_.empty() && feed_queue_.front().visible_at <= now) {
        const auto update = feed_queue_.front();
        feed_queue_.pop_front();
        last_view_ = BookView{update.as_of,   now,          update.best_bid,
                              update.bid_size, update.best_ask, update.ask_size};
        book_changed = true;
    }

    while (!fill_queue_.empty() && fill_queue_.front().visible_at <= now) {
        const auto pending = fill_queue_.front();
        fill_queue_.pop_front();
        gate_.clear();
        strategy_.on_fill(pending.fill, current_view(now), gate_);
        ++stats_.callbacks;
        schedule_from_gate(now);
    }

    while (!gone_queue_.empty() && gone_queue_.front().visible_at <= now) {
        const auto gone = gone_queue_.front();
        gone_queue_.pop_front();
        gate_.clear();
        strategy_.on_order_gone(gone.id, gone.filled, gate_);
        ++stats_.callbacks;
        schedule_from_gate(now);
    }

    if (book_changed) run_callbacks(now);
}

void Simulator::run_callbacks(Time now) {
    gate_.clear();
    strategy_.on_book(current_view(now), gate_);
    ++stats_.callbacks;
    schedule_from_gate(now);
}

void Simulator::schedule_from_gate(Time decided_at) {
    // One latency draw per action, at the moment of the decision. The strategy
    // has no say in this: it cannot specify an arrival time, so it cannot
    // back-date an order into a queue position it did not earn.
    for (const auto& submit : gate_.submits()) {
        pending_submits_.push_back(PendingSubmit{decided_at + order_delay_.sample(decided_at),
                                                 submit.side, submit.price, submit.quantity,
                                                 submit.id, submit.tif});
        ++stats_.strategy_submits;
    }
    for (const match::OrderId id : gate_.cancels()) {
        pending_cancels_.push_back(
            PendingCancel{decided_at + order_delay_.sample(decided_at), id});
        ++stats_.strategy_cancels;
    }
    gate_.clear();

    // A variable delay can invert arrival order, so both queues are kept
    // sorted by arrival rather than by submission. Stable, so two actions
    // drawing the same delay keep the order the strategy issued them in.
    std::stable_sort(pending_submits_.begin(), pending_submits_.end(),
                     [](const PendingSubmit& a, const PendingSubmit& b) {
                         return a.arrival < b.arrival;
                     });
    std::stable_sort(pending_cancels_.begin(), pending_cancels_.end(),
                     [](const PendingCancel& a, const PendingCancel& b) {
                         return a.arrival < b.arrival;
                     });
}

void Simulator::finish() {
    // Let anything already in flight land, so orders are not left in a limbo
    // that flatters the fill rate.
    release_orders(Time{std::numeric_limits<std::int64_t>::max()});
}

}  // namespace sim
