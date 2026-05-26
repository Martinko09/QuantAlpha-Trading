#pragma once
// =============================================================================
// indicators.h — QuantAlpha Trading System
// All technical indicators implemented from scratch in pure C++17
//
// Mathematical Foundations:
//   EMA(t) = Price(t) * k  +  EMA(t-1) * (1-k),   k = 2/(N+1)
//   SMA(t) = (1/N) * Σ Price(t-i),  i=0..N-1
//   BB Upper = SMA + mult * σ,  σ = sqrt( (1/N)*Σ(P - SMA)² )
//   RSI     = 100 - 100/(1 + RS),  RS = AvgGain/AvgLoss  (Wilder EMA)
//   MACD    = EMA(fast) - EMA(slow),  Signal = EMA(MACD, 9)
//   ATR     = EMA( max(H-L, |H-Cp|, |L-Cp|) )  — Wilder smoothing
//   VWAP    = Σ(TP * Vol) / ΣVol,  TP = (H+L+C)/3
//   OBV     = OBV_prev ± Volume  (sign from Close direction)
//   Stoch   = 100*(C - Lowest_Low)/(Highest_High - Lowest_Low)
//   ADX     = EMA(|+DI - -DI| / (+DI + -DI) * 100)  Wilder
//   MFI     = 100 - 100/(1 + MF_ratio),  MF = TP * Volume
// =============================================================================

#include <vector>
#include <deque>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <algorithm>

// Candle — fundamental data unit
struct Candle {
    double open, high, low, close;
    double volume;
    long long timestamp;

    double typical_price() const { return (high + low + close) / 3.0; }
    double median_price()  const { return (high + low) / 2.0; }
    double weighted_close()const { return (high + low + close * 2.0) / 4.0; }
};

// Math utilities
namespace math_utils {

struct WelfordVariance {
    int    n   = 0;
    double mean = 0.0, M2 = 0.0;

    void update(double x) {
        ++n;
        double delta  = x - mean;
        mean += delta / n;
        double delta2 = x - mean;
        M2   += delta * delta2;
    }
    double variance()  const { return n < 2 ? 0.0 : M2 / (n - 1); }
    double stddev()    const { return std::sqrt(variance()); }
    void   reset()           { n=0; mean=0.0; M2=0.0; }
};

inline double rolling_stddev(const std::deque<double>& window) {
    if (window.empty()) return 0.0;
    double m = std::accumulate(window.begin(), window.end(), 0.0) / window.size();
    double sq = 0.0;
    for (double v : window) sq += (v - m) * (v - m);
    return std::sqrt(sq / window.size());
}

inline double rolling_mean(const std::deque<double>& window) {
    if (window.empty()) return 0.0;
    return std::accumulate(window.begin(), window.end(), 0.0) / window.size();
}

} // namespace math_utils

// =============================================================================
// 1. Simple Moving Average (SMA)
// =============================================================================
class SMA {
    int    period_;
    std::deque<double> buf_;
    double sum_ = 0.0;
    bool   ready_ = false;
public:
    double value = 0.0;

    explicit SMA(int period) : period_(period) {}

    bool update(double price) {
        buf_.push_back(price);
        sum_ += price;
        if ((int)buf_.size() > period_) {
            sum_ -= buf_.front();
            buf_.pop_front();
        }
        ready_ = ((int)buf_.size() == period_);
        if (ready_) value = sum_ / period_;
        return ready_;
    }
    bool is_ready()  const { return ready_; }
    int  period()    const { return period_; }
};

// =============================================================================
// 2. Exponential Moving Average (EMA)
// =============================================================================
class EMA {
    double k_;
    bool   first_ = true;
    bool   ready_ = false;
    int    period_;
    int    warmup_ = 0;
public:
    double value = 0.0;

    explicit EMA(int period, bool wilder = false)
        : k_(wilder ? 1.0/period : 2.0/(period+1)),
          period_(period) {}

    bool update(double price) {
        if (first_) {
            value  = price;
            first_ = false;
        } else {
            value = price * k_ + value * (1.0 - k_);
        }
        ++warmup_;
        ready_ = (warmup_ >= period_);
        return ready_;
    }
    bool is_ready()  const { return ready_; }
    int  period()    const { return period_; }
};

// =============================================================================
// 3. Bollinger Bands (BB)
// =============================================================================
struct BBResult {
    double upper, middle, lower;
    double bandwidth;
    double pct_b;
    bool   ready;
};

class BollingerBands {
    int    period_;
    double mult_;
    SMA    sma_;
    std::deque<double> buf_;
public:
    BBResult result{};

    BollingerBands(int period = 20, double mult = 2.0)
        : period_(period), mult_(mult), sma_(period) {}

    bool update(double price) {
        sma_.update(price);
        buf_.push_back(price);
        if ((int)buf_.size() > period_) buf_.pop_front();

        result.ready = ((int)buf_.size() == period_) && sma_.is_ready();
        if (!result.ready) return false;

        double sd       = math_utils::rolling_stddev(buf_);
        result.middle   = sma_.value;
        result.upper    = result.middle + mult_ * sd;
        result.lower    = result.middle - mult_ * sd;
        result.bandwidth= (result.upper - result.lower) / result.middle;
        double denom    = result.upper - result.lower;
        result.pct_b    = denom > 1e-12 ? (price - result.lower) / denom : 0.5;
        return true;
    }
    bool is_ready() const { return result.ready; }
};

// =============================================================================
// 4. MACD (Moving Average Convergence Divergence)
// =============================================================================
struct MACDResult {
    double macd, signal, histogram;
    bool   ready;
};

class MACD {
    EMA    fast_, slow_, signal_;
public:
    MACDResult result{};

    MACD(int fast=12, int slow=26, int signal=9)
        : fast_(fast), slow_(slow), signal_(signal) {}

    bool update(double price) {
        fast_.update(price);
        slow_.update(price);

        result.ready = false;
        if (!fast_.is_ready() || !slow_.is_ready()) return false;

        double macd_line = fast_.value - slow_.value;
        signal_.update(macd_line);

        if (!signal_.is_ready()) return false;

        result.macd      = macd_line;
        result.signal    = signal_.value;
        result.histogram = macd_line - signal_.value;
        result.ready     = true;
        return true;
    }
    bool is_ready() const { return result.ready; }
};

// =============================================================================
// 5. RSI (Relative Strength Index) — Wilder, 14-period
// =============================================================================
class RSI {
    int    period_;
    double avg_gain_ = 0.0, avg_loss_ = 0.0;
    double prev_     = -1.0;
    int    count_    = 0;
    bool   ready_    = false;
    double k_;
public:
    double value = 50.0;

    explicit RSI(int period = 14) : period_(period), k_(1.0 / period) {}

    bool update(double price) {
        if (prev_ < 0.0) { prev_ = price; return false; }

        double change = price - prev_;
        double gain   = change > 0 ? change : 0.0;
        double loss   = change < 0 ? -change : 0.0;
        prev_         = price;
        ++count_;

        if (count_ <= period_) {
            avg_gain_ += gain;
            avg_loss_ += loss;
            if (count_ == period_) {
                avg_gain_ /= period_;
                avg_loss_ /= period_;
                ready_    =  true;
            }
        } else {
            avg_gain_ = avg_gain_ * (1.0 - k_) + gain * k_;
            avg_loss_ = avg_loss_ * (1.0 - k_) + loss * k_;
        }

        if (ready_) {
            double rs = avg_loss_ < 1e-12 ? 1e9 : avg_gain_ / avg_loss_;
            value = 100.0 - 100.0 / (1.0 + rs);
        }
        return ready_;
    }
    bool is_ready() const { return ready_; }
};

// =============================================================================
// 6. ATR (Average True Range) — Wilder, 14-period
// =============================================================================
class ATR {
    EMA   ema_;
    double prev_close_ = -1.0;
    bool  ready_       = false;
public:
    double value = 0.0;

    explicit ATR(int period = 14) : ema_(period, true) {}

    bool update(const Candle& c) {
        double tr;
        if (prev_close_ < 0.0) {
            tr          = c.high - c.low;
            prev_close_ = c.close;
        } else {
            double hl  = c.high  - c.low;
            double hcp = std::fabs(c.high  - prev_close_);
            double lcp = std::fabs(c.low   - prev_close_);
            tr         = std::max({hl, hcp, lcp});
            prev_close_= c.close;
        }
        ready_ = ema_.update(tr);
        value  = ema_.value;
        return ready_;
    }
    bool is_ready() const { return ready_; }
};

// =============================================================================
// 7. VWAP (Volume Weighted Average Price)
// =============================================================================
class VWAP {
    double cum_tp_vol_ = 0.0;
    double cum_vol_    = 0.0;
    double cum_tp2_vol_= 0.0;
public:
    double value       = 0.0;
    double upper_band  = 0.0;
    double lower_band  = 0.0;

    void reset() {
        cum_tp_vol_ = cum_vol_ = cum_tp2_vol_ = 0.0;
        value = upper_band = lower_band = 0.0;
    }

    bool update(const Candle& c) {
        double tp       = c.typical_price();
        cum_tp_vol_    += tp * c.volume;
        cum_vol_       += c.volume;
        cum_tp2_vol_   += tp * tp * c.volume;

        if (cum_vol_ < 1e-12) return false;

        value           = cum_tp_vol_ / cum_vol_;
        double variance = cum_tp2_vol_ / cum_vol_ - value * value;
        double sd       = variance > 0 ? std::sqrt(variance) : 0.0;
        upper_band      = value + sd;
        lower_band      = value - sd;
        return true;
    }
};

// =============================================================================
// 8. OBV (On-Balance Volume)
// =============================================================================
class OBV {
    double prev_close_ = -1.0;
public:
    double value = 0.0;

    bool update(const Candle& c) {
        if (prev_close_ < 0.0) { prev_close_ = c.close; return false; }

        if      (c.close > prev_close_) value += c.volume;
        else if (c.close < prev_close_) value -= c.volume;

        prev_close_ = c.close;
        return true;
    }
};

// =============================================================================
// 9. Stochastic Oscillator (%K and %D)
// =============================================================================
struct StochResult {
    double k, d;
    bool   ready;
};

class Stochastic {
    int    k_period_, d_period_;
    std::deque<double> highs_, lows_;
    SMA    d_sma_;
public:
    StochResult result{};

    Stochastic(int k_period = 14, int d_period = 3)
        : k_period_(k_period), d_period_(d_period), d_sma_(d_period) {}

    bool update(const Candle& c) {
        highs_.push_back(c.high);
        lows_ .push_back(c.low);
        if ((int)highs_.size() > k_period_) { highs_.pop_front(); lows_.pop_front(); }

        result.ready = false;
        if ((int)highs_.size() < k_period_) return false;

        double hh = *std::max_element(highs_.begin(), highs_.end());
        double ll = *std::min_element(lows_ .begin(), lows_ .end());
        double range = hh - ll;

        result.k = range > 1e-12 ? 100.0 * (c.close - ll) / range : 50.0;
        d_sma_.update(result.k);

        if (!d_sma_.is_ready()) return false;

        result.d     = d_sma_.value;
        result.ready = true;
        return true;
    }
    bool is_ready() const { return result.ready; }
};

// =============================================================================
// 10. ADX (Average Directional Index) + DI+/DI-
// =============================================================================
struct ADXResult {
    double adx, plus_di, minus_di;
    bool   ready;
    bool   is_trending() const { return adx > 25.0; }
};

class ADX {
    int    period_;
    double k_;
    double smooth_tr_   = 0.0;
    double smooth_plus_ = 0.0;
    double smooth_minus_= 0.0;
    double smooth_dx_   = 0.0;
    double prev_high_   = -1.0, prev_low_ = -1.0, prev_close_ = -1.0;
    int    count_       = 0;
    std::vector<double> tr_buf_, plus_buf_, minus_buf_;
    bool   ready_       = false;
    bool   adx_ready_   = false;
    int    dx_count_    = 0;
public:
    ADXResult result{};

    explicit ADX(int period = 14) : period_(period), k_(1.0 / period) {}

    bool update(const Candle& c) {
        result.ready = false;
        if (prev_close_ < 0.0) {
            prev_high_ = c.high; prev_low_ = c.low; prev_close_ = c.close;
            return false;
        }

        double tr    = std::max({c.high - c.low,
                                 std::fabs(c.high - prev_close_),
                                 std::fabs(c.low  - prev_close_)});
        double up    = c.high - prev_high_;
        double down  = prev_low_ - c.low;
        double plus_dm  = (up > down   && up   > 0.0) ? up   : 0.0;
        double minus_dm = (down > up   && down > 0.0) ? down : 0.0;

        prev_high_ = c.high; prev_low_ = c.low; prev_close_ = c.close;
        ++count_;

        if (count_ <= period_) {
            tr_buf_   .push_back(tr);
            plus_buf_ .push_back(plus_dm);
            minus_buf_.push_back(minus_dm);
            if (count_ == period_) {
                smooth_tr_    = std::accumulate(tr_buf_   .begin(), tr_buf_   .end(), 0.0);
                smooth_plus_  = std::accumulate(plus_buf_ .begin(), plus_buf_ .end(), 0.0);
                smooth_minus_ = std::accumulate(minus_buf_.begin(), minus_buf_.end(), 0.0);
                ready_ = true;
            }
        } else {
            smooth_tr_    = smooth_tr_    - smooth_tr_   /period_ + tr;
            smooth_plus_  = smooth_plus_  - smooth_plus_ /period_ + plus_dm;
            smooth_minus_ = smooth_minus_ - smooth_minus_/period_ + minus_dm;
        }

        if (!ready_) return false;

        double pdi = smooth_tr_ > 1e-12 ? 100.0 * smooth_plus_  / smooth_tr_ : 0.0;
        double mdi = smooth_tr_ > 1e-12 ? 100.0 * smooth_minus_ / smooth_tr_ : 0.0;
        double sum_di = pdi + mdi;
        double dx   = sum_di > 1e-12 ? 100.0 * std::fabs(pdi - mdi) / sum_di : 0.0;

        ++dx_count_;
        if (dx_count_ <= period_) {
            smooth_dx_ += dx;
            if (dx_count_ == period_) {
                smooth_dx_ /= period_;
                adx_ready_  = true;
            }
        } else {
            smooth_dx_ = smooth_dx_ * (1.0 - k_) + dx * k_;
        }

        result.plus_di  = pdi;
        result.minus_di = mdi;
        result.adx      = adx_ready_ ? smooth_dx_ : 0.0;
        result.ready    = adx_ready_;
        return result.ready;
    }
    bool is_ready() const { return result.ready; }
};

// =============================================================================
// 11. MFI (Money Flow Index)
// =============================================================================
class MFI {
    int    period_;
    double prev_tp_  = -1.0;
    std::deque<double> pos_mf_, neg_mf_;
public:
    double value = 50.0;
    bool   ready = false;

    explicit MFI(int period = 14) : period_(period) {}

    bool update(const Candle& c) {
        double tp  = c.typical_price();
        double mf  = tp * c.volume;

        if (prev_tp_ >= 0.0) {
            pos_mf_.push_back(tp > prev_tp_ ? mf : 0.0);
            neg_mf_.push_back(tp < prev_tp_ ? mf : 0.0);
            if ((int)pos_mf_.size() > period_) {
                pos_mf_.pop_front(); neg_mf_.pop_front();
            }
        }
        prev_tp_ = tp;

        ready = ((int)pos_mf_.size() == period_);
        if (!ready) return false;

        double pos = std::accumulate(pos_mf_.begin(), pos_mf_.end(), 0.0);
        double neg = std::accumulate(neg_mf_.begin(), neg_mf_.end(), 0.0);
        double ratio = neg < 1e-12 ? 1e9 : pos / neg;
        value = 100.0 - 100.0 / (1.0 + ratio);
        return true;
    }
};

// =============================================================================
// 12. Volume Profile
// =============================================================================
class VolumeProfile {
    int    bins_;
    double range_low_, range_high_, bin_size_;
    std::vector<double> vol_bins_;
    double total_vol_ = 0.0;
public:
    double poc_price   = 0.0;
    double va_high     = 0.0;
    double va_low      = 0.0;

    VolumeProfile(double range_low, double range_high, int bins = 50)
        : bins_(bins), range_low_(range_low), range_high_(range_high),
          bin_size_((range_high - range_low) / bins),
          vol_bins_(bins, 0.0) {}

    void add_candle(const Candle& c) {
        double tp  = c.typical_price();
        int    idx = static_cast<int>((tp - range_low_) / bin_size_);
        idx = std::clamp(idx, 0, bins_ - 1);
        vol_bins_[idx] += c.volume;
        total_vol_     += c.volume;
    }

    void calculate() {
        if (total_vol_ < 1e-12) return;

        int poc_idx = (int)(std::max_element(vol_bins_.begin(), vol_bins_.end()) - vol_bins_.begin());
        poc_price   = range_low_ + (poc_idx + 0.5) * bin_size_;

        double target = total_vol_ * 0.70;
        double accum  = vol_bins_[poc_idx];
        int lo = poc_idx, hi = poc_idx;
        while (accum < target && (lo > 0 || hi < bins_ - 1)) {
            double add_hi = (hi + 1 < bins_)  ? vol_bins_[hi + 1] : 0.0;
            double add_lo = (lo - 1 >= 0)      ? vol_bins_[lo - 1] : 0.0;
            if (add_hi >= add_lo && hi + 1 < bins_)  { ++hi; accum += add_hi; }
            else if (lo - 1 >= 0)                     { --lo; accum += add_lo; }
            else                                      { break; }
        }
        va_low  = range_low_ + lo       * bin_size_;
        va_high = range_low_ + (hi + 1) * bin_size_;
    }
};

// =============================================================================
// 13. Ichimoku Kinko Hyo
// =============================================================================
struct IchimokuResult {
    double tenkan, kijun, senkou_a, senkou_b, chikou;
    bool   ready;
    bool   is_bullish() const {
        return tenkan > kijun && senkou_a > senkou_b;
    }
};

class Ichimoku {
    std::deque<double> highs_, lows_, closes_;

    double donchian_mid(const std::deque<double>& h,
                        const std::deque<double>& l, int n) const {
        if ((int)h.size() < n) return 0.0;
        double hh = *std::max_element(h.end()-n, h.end());
        double ll = *std::min_element(l.end()-n, l.end());
        return (hh + ll) / 2.0;
    }
public:
    IchimokuResult result{};

    bool update(const Candle& c) {
        highs_ .push_back(c.high);
        lows_  .push_back(c.low);
        closes_.push_back(c.close);
        if ((int)highs_.size() > 52) {
            highs_ .pop_front(); lows_.pop_front();
        }
        if ((int)closes_.size() > 52) closes_.pop_front();

        result.ready = ((int)highs_.size() >= 52);
        if (!result.ready) return false;

        result.tenkan   = donchian_mid(highs_, lows_,  9);
        result.kijun    = donchian_mid(highs_, lows_, 26);
        result.senkou_a = (result.tenkan + result.kijun) / 2.0;
        result.senkou_b = donchian_mid(highs_, lows_, 52);
        result.chikou   = closes_.size() >= 26 ? *(closes_.end()-26) : closes_.front();
        return true;
    }
    bool is_ready() const { return result.ready; }
};