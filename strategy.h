#pragma once
// =============================================================================
// strategy.h — QuantAlpha Multi-Indicator Trading Strategy
//
// 3-LAYER CONFIRMATION SYSTEM:
//  Layer 1: TREND (EMA, ADX, Ichimoku)
//  Layer 2: MOMENTUM (MACD, RSI, Stochastic)
//  Layer 3: VOLUME (OBV, VWAP, MFI)
//
// ENTRY: When score >= 4/7 composite signals
// EXIT:  TP, SL, Trailing Stop, RSI extreme, MACD cross
// =============================================================================

#include "indicators.h"
#include <string>
#include <vector>
#include <sstream>

enum class Signal {
    NONE,
    BUY,
    SELL,
    EXIT_LONG,
    EXIT_SHORT
};

struct SignalInfo {
    Signal      signal      = Signal::NONE;
    double      entry_price = 0.0;
    double      stop_loss   = 0.0;
    double      take_profit = 0.0;
    std::string reason;
    double      confidence  = 0.0;
};

struct StrategyConfig {
    // EMA / SMA
    int ema_fast    = 20;
    int ema_slow    = 50;
    int sma_long    = 200;

    // Bollinger Bands
    int    bb_period = 20;
    double bb_mult   = 2.0;

    // MACD
    int macd_fast   = 12;
    int macd_slow   = 26;
    int macd_signal = 9;

    // RSI
    int    rsi_period      = 14;
    double rsi_buy_max     = 65.0;
    double rsi_sell_min    = 35.0;
    double rsi_exit_high   = 80.0;
    double rsi_exit_low    = 20.0;

    // ATR (for SL/TP)
    int    atr_period   = 14;
    double atr_sl_mult  = 1.5;
    double atr_tp_mult  = 2.5;
    double atr_trail    = 1.0;

    // Stochastic
    int    stoch_k = 14, stoch_d = 3;
    double stoch_oversold   = 25.0;
    double stoch_overbought = 75.0;

    // ADX
    int    adx_period  = 14;
    double adx_min     = 22.0;

    // MFI
    int    mfi_period  = 14;
    double mfi_bull    = 50.0;
    double mfi_bear    = 50.0;

    // Score thresholds
    int    min_bull_score = 4;
    int    min_bear_score = 4;
};

// =============================================================================
// QuantAlphaStrategy
// =============================================================================
class QuantAlphaStrategy {
public:
    EMA           ema_fast, ema_slow;
    SMA           sma_long;
    BollingerBands bb;
    MACD          macd;
    RSI           rsi;
    ATR           atr;
    VWAP          vwap;
    OBV           obv;
    Stochastic    stoch;
    ADX           adx;
    MFI           mfi;
    Ichimoku      ichi;

    StrategyConfig cfg;

    double prev_obv         = 0.0;
    double prev_macd_hist   = 0.0;
    double prev_stoch_k     = 50.0;
    double prev_ema_fast    = 0.0;

    bool   in_long          = false;
    bool   in_short         = false;
    double position_entry   = 0.0;
    double position_sl      = 0.0;
    double position_tp      = 0.0;
    double trailing_sl      = 0.0;
    bool   trailing_active  = false;

    explicit QuantAlphaStrategy(const StrategyConfig& c = StrategyConfig{})
        : cfg(c),
          ema_fast(c.ema_fast),  ema_slow(c.ema_slow),
          sma_long(c.sma_long),
          bb(c.bb_period, c.bb_mult),
          macd(c.macd_fast, c.macd_slow, c.macd_signal),
          rsi(c.rsi_period),
          atr(c.atr_period),
          stoch(c.stoch_k, c.stoch_d),
          adx(c.adx_period),
          mfi(c.mfi_period)
    {}

    SignalInfo update(const Candle& c) {
        ema_fast.update(c.close);
        ema_slow.update(c.close);
        sma_long.update(c.close);
        bb.update(c.close);
        macd.update(c.close);
        rsi.update(c.close);
        atr.update(c);
        vwap.update(c);
        obv.update(c);
        stoch.update(c);
        adx.update(c);
        mfi.update(c);
        ichi.update(c);

        if (!all_ready()) return {};

        SignalInfo info;

        if (in_long || in_short) {
            info = manage_position(c);
            if (info.signal == Signal::EXIT_LONG || info.signal == Signal::EXIT_SHORT)
                return info;
        }

        if (!in_long && !in_short) {
            info = check_entry(c);
        }

        prev_obv       = obv.value;
        prev_macd_hist = macd.result.histogram;
        prev_stoch_k   = stoch.result.k;
        prev_ema_fast  = ema_fast.value;

        return info;
    }

private:
    bool all_ready() const {
        return ema_fast.is_ready() && ema_slow.is_ready() &&
               sma_long.is_ready() && bb.is_ready() &&
               macd.is_ready() && rsi.is_ready() && atr.is_ready() &&
               stoch.is_ready() && adx.is_ready() && mfi.ready;
    }

    struct Score {
        int bull = 0, bear = 0;
        std::string bull_reasons, bear_reasons;

        void add_bull(int pts, const std::string& r) {
            bull += pts;
            if (!bull_reasons.empty()) bull_reasons += " | ";
            bull_reasons += r;
        }
        void add_bear(int pts, const std::string& r) {
            bear += pts;
            if (!bear_reasons.empty()) bear_reasons += " | ";
            bear_reasons += r;
        }
    };

    Score calculate_score(const Candle& c) const {
        Score s;

        // TREND LAYER
        if (ema_fast.value > ema_slow.value && ema_slow.value > sma_long.value)
            s.add_bull(1, "EMA20>EMA50>SMA200");
        else if (ema_fast.value < ema_slow.value && ema_slow.value < sma_long.value)
            s.add_bear(1, "EMA20<EMA50<SMA200");

        if (adx.result.adx > cfg.adx_min) {
            if (adx.result.plus_di > adx.result.minus_di)
                s.add_bull(1, "ADX+" + std::to_string((int)adx.result.adx));
            else
                s.add_bear(1, "ADX-" + std::to_string((int)adx.result.adx));
        }

        if (ichi.result.ready) {
            double cloud_top = std::max(ichi.result.senkou_a, ichi.result.senkou_b);
            double cloud_bot = std::min(ichi.result.senkou_a, ichi.result.senkou_b);
            if (c.close > cloud_top && ichi.result.tenkan > ichi.result.kijun)
                s.add_bull(1, "Ichi_above_cloud");
            else if (c.close < cloud_bot && ichi.result.tenkan < ichi.result.kijun)
                s.add_bear(1, "Ichi_below_cloud");
        }

        // MOMENTUM LAYER
        if (macd.result.histogram > 0 && macd.result.histogram > prev_macd_hist)
            s.add_bull(1, "MACD_hist_rising");
        else if (macd.result.histogram < 0 && macd.result.histogram < prev_macd_hist)
            s.add_bear(1, "MACD_hist_falling");

        if (rsi.value > 50.0 && rsi.value < cfg.rsi_buy_max)
            s.add_bull(1, "RSI=" + std::to_string((int)rsi.value));
        else if (rsi.value < 50.0 && rsi.value > cfg.rsi_sell_min)
            s.add_bear(1, "RSI=" + std::to_string((int)rsi.value));

        if (stoch.result.k > stoch.result.d && prev_stoch_k <= stoch.result.d
                && stoch.result.k < cfg.stoch_overbought)
            s.add_bull(1, "Stoch_cross_up");
        else if (stoch.result.k < stoch.result.d && prev_stoch_k >= stoch.result.d
                && stoch.result.k > cfg.stoch_oversold)
            s.add_bear(1, "Stoch_cross_down");

        // VOLUME LAYER
        bool obv_rising  = obv.value > prev_obv;
        bool obv_falling = obv.value < prev_obv;
        bool above_vwap  = c.close > vwap.value;
        bool below_vwap  = c.close < vwap.value;
        bool mfi_bull_   = mfi.value > cfg.mfi_bull;
        bool mfi_bear_   = mfi.value < cfg.mfi_bear;

        int vol_bull = (obv_rising ? 1 : 0) + (above_vwap ? 1 : 0) + (mfi_bull_ ? 1 : 0);
        int vol_bear = (obv_falling? 1 : 0) + (below_vwap ? 1 : 0) + (mfi_bear_ ? 1 : 0);

        if (vol_bull >= 2) s.add_bull(1, "Volume_confirm");
        if (vol_bear >= 2) s.add_bear(1, "Volume_confirm");

        // BOLLINGER BANDS
        bool squeeze = bb.result.bandwidth < 0.015;
        if (squeeze && c.close > bb.result.middle && prev_ema_fast <= bb.result.middle)
            s.add_bull(1, "BB_squeeze_breakout_up");
        else if (squeeze && c.close < bb.result.middle && prev_ema_fast >= bb.result.middle)
            s.add_bear(1, "BB_squeeze_breakout_down");
        else if (!squeeze && c.close > bb.result.middle)
            s.add_bull(1, "BB_above_mid");
        else if (!squeeze && c.close < bb.result.middle)
            s.add_bear(1, "BB_below_mid");

        return s;
    }

    SignalInfo check_entry(const Candle& c) {
        Score score = calculate_score(c);
        SignalInfo info;
        double atr_val = atr.value;
        if (atr_val < 1e-12) return info;

        if (score.bull >= cfg.min_bull_score) {
            info.signal      = Signal::BUY;
            info.entry_price = c.close;
            info.stop_loss   = c.close - cfg.atr_sl_mult * atr_val;
            info.take_profit = c.close + cfg.atr_tp_mult * atr_val;
            info.reason      = score.bull_reasons;
            info.confidence  = std::min(1.0, score.bull / 7.0);

            in_long         = true;
            position_entry  = c.close;
            position_sl     = info.stop_loss;
            position_tp     = info.take_profit;
            trailing_active = false;
            trailing_sl     = info.stop_loss;
        }
        else if (score.bear >= cfg.min_bear_score) {
            info.signal      = Signal::SELL;
            info.entry_price = c.close;
            info.stop_loss   = c.close + cfg.atr_sl_mult * atr_val;
            info.take_profit = c.close - cfg.atr_tp_mult * atr_val;
            info.reason      = score.bear_reasons;
            info.confidence  = std::min(1.0, score.bear / 7.0);

            in_short        = true;
            position_entry  = c.close;
            position_sl     = info.stop_loss;
            position_tp     = info.take_profit;
            trailing_active = false;
            trailing_sl     = info.stop_loss;
        }
        return info;
    }

    SignalInfo manage_position(const Candle& c) {
        SignalInfo info;
        double atr_val = atr.value;

        if (in_long) {
            if (!trailing_active && c.close >= position_entry + cfg.atr_sl_mult * atr_val)
                trailing_active = true;

            if (trailing_active) {
                double new_trail = c.close - cfg.atr_trail * atr_val;
                if (new_trail > trailing_sl) trailing_sl = new_trail;
                position_sl = trailing_sl;
            }

            bool hit_sl      = c.low  <= position_sl;
            bool hit_tp      = c.high >= position_tp;
            bool rsi_extreme = rsi.value > cfg.rsi_exit_high;
            bool macd_cross  = macd.result.macd < macd.result.signal
                               && prev_macd_hist > 0;

            if (hit_sl || hit_tp || rsi_extreme || macd_cross) {
                info.signal = Signal::EXIT_LONG;
                info.entry_price = hit_sl ? position_sl : (hit_tp ? position_tp : c.close);
                if (hit_sl)        info.reason = "SL_hit";
                else if (hit_tp)   info.reason = "TP_hit";
                else if (rsi_extreme) info.reason = "RSI_overbought";
                else               info.reason = "MACD_exit";
                in_long = false;
            }
        }
        else if (in_short) {
            if (!trailing_active && c.close <= position_entry - cfg.atr_sl_mult * atr_val)
                trailing_active = true;

            if (trailing_active) {
                double new_trail = c.close + cfg.atr_trail * atr_val;
                if (new_trail < trailing_sl) trailing_sl = new_trail;
                position_sl = trailing_sl;
            }

            bool hit_sl      = c.high >= position_sl;
            bool hit_tp      = c.low  <= position_tp;
            bool rsi_extreme = rsi.value < cfg.rsi_exit_low;
            bool macd_cross  = macd.result.macd > macd.result.signal
                               && prev_macd_hist < 0;

            if (hit_sl || hit_tp || rsi_extreme || macd_cross) {
                info.signal = Signal::EXIT_SHORT;
                info.entry_price = hit_sl ? position_sl : (hit_tp ? position_tp : c.close);
                if (hit_sl)        info.reason = "SL_hit";
                else if (hit_tp)   info.reason = "TP_hit";
                else if (rsi_extreme) info.reason = "RSI_oversold";
                else               info.reason = "MACD_exit";
                in_short = false;
            }
        }
        return info;
    }
};