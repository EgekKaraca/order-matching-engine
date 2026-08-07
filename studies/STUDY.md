# Does the fill assumption matter? A measurement

Almost every backtester fakes the fill. *"I get the close." "I cross the spread and I'm done."
"My limit order at the touch filled because the price traded there."* That assumption is where
backtest returns come from, and it is where they die in production: a passive order at the touch
fills only when the queue ahead of it drains, and the fills you *do* get are disproportionately
the ones you didn't want.

This is a measurement of how much that assumption is worth, in dollars, on real order-by-order
data.

## Method

The identical strategy, on identical data, with identical latency, under different fill models.
Everything varies except the one thing being measured.

| | |
|---|---|
| Data | LOBSTER AAPL 2012-06-21, 09:30–10:30, level-50 (91,996 events) |
| Strategy | fixed-size passive market maker, joining the touch on both sides |
| Latency | feed 100 µs, order 500 µs (both swept) |
| Fees | maker rebate 2 units/share, taker fee 3 units/share |
| Engine | this repository's `std::map` matching engine, byte-identical to the published benchmark consensus on all five scenarios |

**Model A — naive.** Filled whenever the market trades at my price. No queue. This is what most
portfolio backtesters do, explicitly or by omission. One deliberate restraint: the fill is capped
by the volume that actually traded rather than filling the order outright, which is the *less*
generous variant — so the gap measured here is a lower bound.

**Model C — L3 exact.** The order enters the same book as everyone else's through the matching
engine, takes a real place in a real FIFO queue, and fills only when the size ahead of it
actually drains via the historical cancels and executions being replayed.

**Model B — L2 probabilistic** was implemented and is excluded. See *What went wrong* below.

## The result

At 100-share orders and 500 µs order latency:

| Model | Fills | Spread capture | Mark-out @1s | @10s | @60s | Net PnL |
|---|--:|--:|--:|--:|--:|--:|
| A — naive | 1,118 | $467 | −$2,163 | −$3,171 | −$6,147 | **+$783** |
| C — L3 exact | 908 | $1,075 | −$937 | −$3,886 | −$3,730 | **−$2,114** |

**The strategy is profitable under the naive fill model and loss-making under the real one.**

The mark-outs are the mechanism. Both models show negative mark-outs — a passive maker is
adversely selected under either — but the naive model books fills it would never have received,
at prices it would never have got, and the accumulated difference is the entire result.

### But 100 shares is too big to claim that from

Median displayed size at the touch is **109 shares**, so a 100-share order is 92% of a typical
level. At that size the simulator's core assumption — that these orders do not change anyone
else's behaviour — is carrying more weight than it can bear.

Sweeping order size, with the position limit scaled alongside it so that the sweep measures size
rather than the limit:

| Order size | % of touch depth | PnL_A | PnL_C | Gap | Assumption |
|--:|--:|--:|--:|--:|---|
| 1 | 0.9% | $26 | $4 | 85% | defensible |
| **10** | **9.2%** | **+$217** | **−$46** | **121%** | **defensible** |
| 50 | 45.9% | $873 | −$936 | 207% | strained |
| 100 | 91.7% | $783 | −$2,114 | 370% | strained |
| 300 | 275% | −$2,233 | −$6,318 | 183% | strained |

**The sign flip survives at a defensible size.** At 9.2% of displayed depth the naive model still
reports a profit and the L3 model still reports a loss. That is the result this work earns; the
370% is not.

The gap grows monotonically with size across the defensible range, which is mechanically what
should happen — a larger order waits longer in the queue, so the naive model's error compounds.

### Robustness

`PnL_A > PnL_C` at every order latency from 0 to 100 ms, and in all four quarter-hour windows of
the sample. The *direction* is robust. The magnitude is not stable, and should not be quoted as
though it were.

## What went wrong, and what that cost

Three mistakes are recorded here because each of them produced a plausible, publishable, wrong
answer, and only deliberate suspicion caught them.

**1. The first run measured nothing.** With the strategy quoting a fixed $0.30 from the mid, the
gap came out near zero with unstable sign. AAPL in 2012 routinely ran a $0.61 spread, so a $0.30
offset quotes *inside* the spread — where there is no queue to join, and where every fill model
therefore agrees by construction. The tell was queue depth at entry: p90 of 18 shares. The
strategy has to **join the touch** for queue position to exist as a thing that can be modelled. A
study that skipped that check would have concluded "no gap" and been wrong.

**2. Position limits silently never bound.** The strategy tracked only its most recent quote per
side. On a requote the cancel took an order-latency to land, and in that window the forgotten
order was still live and could still fill — and nothing told the strategy its orders were gone.
Closing position reached 18,650 against a limit of 1,000, which makes the PnL a measurement of
inventory mark-to-market rather than of execution. Fixed by notifying the strategy when its
orders retire, and by limiting total *exposure* rather than filled position.

**3. The headline was quoted at an indefensible size** before the size sweep existed — see above.

## Limits of this result

- **One hour, one stock, one day.** The numbers are real; their error bars are not estimated. A
  single point estimate is not a finding.
- **Queue-realistic, impact-free.** Replaying historical flow assumes these orders do not change
  anyone else's behaviour. That holds for small passive size and fails for large; the size sweep
  above is an attempt to say where. The paradigm that removes the assumption is agent-based
  simulation ([ABIDES](https://github.com/jpmorganchase/abides-jpmc-public)).
- **The ratio metric is fragile.** `(PnL_A − PnL_C)/|PnL_A|` blows up whenever `PnL_A` is near
  zero. Report the dollar gap with it, always.
- **The reconstruction is bounded to the touch.** Top-of-book exact for 54,605 consecutive events
  and 99.90% of the session; at full 50-level depth, 2.84%. The strategy quotes at the touch, so
  the bound covers what the study uses — but it is a bound.
- **Model B is excluded**, so this is a two-model comparison, not three.

## Reproducing

```bash
scripts/reproduce.sh                                  # everything that does not need Linux
STUDY_CSV_DIR=/tmp build/studies/fill_model_study \
    data/message_50.csv data/orderbook_50.csv         # the sweeps, as CSV
```

## Cross-validation against a live-trading experiment

[arXiv:2502.18625](https://arxiv.org/html/2502.18625v2) ran 232,897 minimum-size maker orders
on Binance BTC perps, submitted mechanically regardless of signal — structurally the same
experiment as the fixed-offset maker here. At 10-share orders:

| | Their live result | Ours (replay) |
|---|---|---|
| Naive market-making Sharpe | −109.0 | −18.1 raw / −19.8 deflated |
| Fill rate vs queue position | falls toward the back | **falls toward the back ✓** |
| Fill rate vs opposite depth | rises as it thins | **rises as it thickens ✗** |
| Fill/return relationship | negatively correlated | negative mark-outs ✓ |

**We reproduce one direction and contradict the other, and the contradiction is recorded
rather than tuned away.**

Fill probability by queue position and opposite depth, 1,791 posted orders:

| Queue position | Thin opp | Mid opp | Thick opp |
|---|--:|--:|--:|
| front (0–25%) | 24.5% (204) | 32.0% (122) | 47.4% (19) |
| back (75–100%) | 23.9% (920) | 21.3% (436) | 30.0% (90) |

Front beats back in every column — that direction agrees. The opposite-depth direction does
not, and three explanations are untested: a $0.61-spread equity at the touch is not a
tight-spread crypto perp and a thick two-sided book may simply mean more trading; their
opposite-depth measure may not be ours; and the thick-opposite cells hold only 19 and 90
orders, so that column may be noise.

**A structural limitation:** the two middle queue bins are empty and always will be. A
join-the-touch strategy is either alone at its level or behind all of it, never in between,
so only two of four rows can ever populate. Comparing a two-row surface to their full one is
a real weakness of this cross-check.

On the Sharpe: same sign, magnitude **not** claimed to match. Ours moves with order size
(−18.1 at 10 shares, −70.8 at 100), so quoting whichever is closest to −109 would be choosing
the number that flatters the comparison. The honest statement is that a mechanical,
signal-free market maker loses money on a risk-adjusted basis in both.

### Adverse versus benign fills

Using [arXiv:2409.12721](https://arxiv.org/html/2409.12721)'s definition — a fill is adverse
when the position is out of the money immediately after execution:

| | Fills | PnL |
|---|--:|--:|
| Adverse | 246 | −$212.60 |
| Benign | 246 | +$109.39 |

The adverse fills are **half the count but nearly twice the magnitude**. That asymmetry is the
mechanism, and it is exactly what a single blended PnL hides. It is also what the naive fill
model structurally cannot produce, because it does not know which fills it would not have got.

*(The exactly-even 246/246 count split is a coincidence in this sample rather than a
constraint of the method — nothing in `classify_fills` forces balance. Worth re-checking on
more data.)*

### How much rests on the impact-free assumption

Sweeping the crowd-out intensity (0 = the published impact-free baseline), 10-share orders:

| Intensity | PnL_A | PnL_C | Gap | Adds crowded out |
|--:|--:|--:|--:|--:|
| 0.00 | $217 | −$46 | 121.2% | 0 |
| 0.25 | $259 | −$81 | 131.2% | 1,305 |
| 0.50 | $223 | −$43 | 119.4% | 1,948 |
| 1.00 | $223 | −$53 | 123.9% | 2,891 |
| 2.00 | $179 | −$33 | 118.4% | 3,854 |

**The sign flip survives the entire sweep and the gap is stable at 118–131%.** The headline
does not rest on the impact-free assumption — which is what this study existed to test, and
was not the guaranteed answer.

Read as a **sensitivity, not a correction**. The model captures one mechanism — participants
declining to queue behind a visible order — and does not move the mid, model informed flow, or
model a race to the touch. Removing the assumption properly needs agent-based simulation
([ABIDES](https://github.com/jpmorganchase/abides-jpmc-public)) or a queue-reactive generative
model ([arXiv:2603.24137](https://arxiv.org/pdf/2603.24137)).

**The first version of that model had a real artifact**, caught only because the numbers looked
wrong: `PnL_C` went positive under impact and fills collapsed from 492 to 60. An `Add` could be
crowded out while that same order's later `Delete` was still replayed; the delete found no such
order, fell back to draining the level from the front, and the order at the front was often the
strategy's own — so crowding out was handing the strategy fills nothing paid for. An order that
never existed cannot later be cancelled. Fixed, and pinned by a test.
