// deflated_sharpe.hpp — the haircut a short backtest's Sharpe has earned.
//
// A Sharpe ratio estimated from a short sample is biased upward, and the bias
// grows with the number of variants tried before one was chosen to report.
// This project reports a Sharpe annualised from a single intraday hour — a
// ~1700x extrapolation — so the correction is not optional decoration.
//
// Construction follows Bailey & Lopez de Prado: the standard error of a Sharpe
// estimate is
//
//     SE = sqrt( (1 - g1*SR + (g2-1)/4 * SR^2) / (n-1) )
//
// where g1 is the skewness and g2 the kurtosis of the return series. The
// observed ratio is then shrunk by that error, scaled up by how many
// independent trials went into picking it.
//
// One deliberate asymmetry, tested: the haircut always moves the ratio AWAY
// from zero when it is negative. Shrinking a losing strategy's Sharpe toward
// zero would make it look better, which inverts the entire point of correcting
// for optimism.
#ifndef SIM_DEFLATED_SHARPE_HPP
#define SIM_DEFLATED_SHARPE_HPP

#include <cmath>
#include <cstddef>

namespace sim {

// `trials` is the number of strategy variants or parameter settings evaluated
// before this result was reported. Passing 1 claims the first thing tried was
// the thing reported; if a sweep chose it, pass the size of the sweep.
inline double deflate_sharpe(double observed_sharpe, std::size_t samples, std::size_t trials,
                             double skew, double excess_kurtosis) {
    if (samples < 2 || observed_sharpe == 0.0 || trials == 0) return 0.0;

    const double n = static_cast<double>(samples);
    const double sr = observed_sharpe;
    const double kurtosis = excess_kurtosis + 3.0;  // g2 as the formula expects it

    double variance = 1.0 - skew * sr + (kurtosis - 1.0) / 4.0 * sr * sr;
    // A pathological skew/kurtosis combination can drive this negative, which
    // has no square root. Fall back to the normal-returns case rather than
    // producing a NaN that would propagate into a printed result.
    if (variance <= 0.0) variance = 1.0;

    const double standard_error = std::sqrt(variance / (n - 1.0));

    // More trials, more chances for noise to have produced the best-looking
    // one. sqrt(log(trials)) is the leading term of the expected maximum of
    // `trials` draws.
    const double trial_penalty =
        trials <= 1 ? 1.0 : std::sqrt(std::log(static_cast<double>(trials))) + 1.0;

    // Subtract in both directions: a negative Sharpe gets more negative.
    return sr - standard_error * trial_penalty;
}

}  // namespace sim

#endif  // SIM_DEFLATED_SHARPE_HPP
