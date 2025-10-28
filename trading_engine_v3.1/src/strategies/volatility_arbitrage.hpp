#pragma once

#include "../core/types.hpp"
#include "../core/circular_buffer.hpp"
#include <cmath>
#include <algorithm>

namespace trading {

// Volatility Arbitrage - Exploits volatility mispricing
class VolatilityArbitrageStrategy {
public:
    struct Config {
        int atr_period;                     // ATR calculation period
        double high_vol_multiplier;         // ATR multiplier for "high vol"
        double low_vol_multiplier;          // ATR multiplier for "low vol"
        
        double high_vol_entry_threshold;    // Enter when ATR > avg * this
        double low_vol_entry_threshold;     // Enter when ATR < avg * this
        
        double target_profit_bps;           // Take profit
        double stop_loss_bps;               // Stop loss
        
        double position_size_usd;
        int max_hold_minutes;               // Max position hold time
        
        Config()
            : atr_period(14)
            , high_vol_multiplier(1.5)
            , low_vol_multiplier(0.7)
            , high_vol_entry_threshold(1.3)
            , low_vol_entry_threshold(0.8)
            , target_profit_bps(20.0)
            , stop_loss_bps(10.0)
            , position_size_usd(5000.0)
            , max_hold_minutes(15)
        {}
    };
    
    enum class VolatilityRegime {
        HIGH,       // Current volatility elevated
        NORMAL,     // Average volatility
        LOW         // Volatility compressed
    };
    
    struct VolSignal {
        std::string symbol;
        VolatilityRegime regime;
        
        std::string strategy_type;          // "STRADDLE", "DIRECTIONAL", "MEAN_REVERT"
        Side primary_side;                  // Main direction (if directional)
        
        double current_atr;
        double avg_atr;
        double atr_ratio;                   // current_atr / avg_atr
        
        double entry_price;
        double target_price;
        double stop_price;
        
        double expected_profit_bps;
        TimePoint generated_at;
        bool is_valid;
        
        VolSignal()
            : regime(VolatilityRegime::NORMAL)
            , strategy_type("NONE")
            , primary_side(Side::BUY)
            , current_atr(0.0)
            , avg_atr(0.0)
            , atr_ratio(1.0)
            , entry_price(0.0)
            , target_price(0.0)
            , stop_price(0.0)
            , expected_profit_bps(0.0)
            , is_valid(false)
        {}
    };
    
    explicit VolatilityArbitrageStrategy(const Config& config)
        : config_(config)
        , price_history_(config.atr_period * 2)
        , atr_history_(50)
    {}
    
    // Update with new price - circular buffer auto-manages size
    void update_price(double price) {
        price_history_.push_back(price);
        
        // Calculate ATR when we have enough data
        if (price_history_.size() >= config_.atr_period + 1) {
            current_atr_ = calculate_atr();
            
            atr_history_.push_back(current_atr_);
            
            // Calculate average ATR
            if (atr_history_.size() >= 10) {
                double sum = 0.0;
                for (size_t i = 0; i < atr_history_.size(); ++i) {
                    sum += atr_history_[i];
                }
                avg_atr_ = sum / atr_history_.size();
            }
        }
    }
    
    // Detect volatility regime
    VolatilityRegime detect_regime() const {
        if (avg_atr_ < 0.000001) {
            return VolatilityRegime::NORMAL;
        }
        
        double ratio = current_atr_ / avg_atr_;
        
        if (ratio > config_.high_vol_entry_threshold) {
            return VolatilityRegime::HIGH;
        } else if (ratio < config_.low_vol_entry_threshold) {
            return VolatilityRegime::LOW;
        } else {
            return VolatilityRegime::NORMAL;
        }
    }
    
    // Generate volatility arbitrage signal
    VolSignal generate_signal(double current_price) {
        VolSignal signal;
        signal.symbol = "";  // Set by caller
        signal.generated_at = Clock::now();
        signal.current_atr = current_atr_;
        signal.avg_atr = avg_atr_;
        
        if (avg_atr_ < 0.000001 || price_history_.size() < config_.atr_period + 1) {
            return signal;  // Not enough data
        }
        
        signal.atr_ratio = current_atr_ / avg_atr_;
        signal.regime = detect_regime();
        
        // HIGH VOLATILITY: Expect mean reversion
        if (signal.regime == VolatilityRegime::HIGH) {
            // Volatility too high → will compress
            // Strategy: Sell straddle or fade the move
            
            signal.strategy_type = "MEAN_REVERT";
            signal.entry_price = current_price;
            
            // If price spiked up, short
            if (is_recent_price_spike_up()) {
                signal.primary_side = Side::SELL;
                signal.target_price = current_price * (1.0 - config_.target_profit_bps / 10000.0);
                signal.stop_price = current_price * (1.0 + config_.stop_loss_bps / 10000.0);
            }
            // If price dumped down, long
            else if (is_recent_price_spike_down()) {
                signal.primary_side = Side::BUY;
                signal.target_price = current_price * (1.0 + config_.target_profit_bps / 10000.0);
                signal.stop_price = current_price * (1.0 - config_.stop_loss_bps / 10000.0);
            }
            else {
                return signal;  // No clear direction
            }
            
            signal.expected_profit_bps = config_.target_profit_bps;
            signal.is_valid = true;
        }
        // LOW VOLATILITY: Expect expansion
        else if (signal.regime == VolatilityRegime::LOW) {
            // Volatility compressed → will expand
            // Strategy: Buy straddle or breakout trade
            
            signal.strategy_type = "STRADDLE";
            signal.entry_price = current_price;
            
            // In low vol, we expect a breakout
            // Place trades on both sides (straddle-like)
            signal.primary_side = Side::BUY;  // Default to buy
            signal.target_price = current_price * (1.0 + config_.target_profit_bps / 10000.0);
            signal.stop_price = current_price * (1.0 - config_.stop_loss_bps / 10000.0);
            
            signal.expected_profit_bps = config_.target_profit_bps;
            signal.is_valid = true;
        }
        
        return signal;
    }
    
    // Create order from signal
    Order create_order_from_signal(const VolSignal& signal, double quantity) {
        Order order;
        order.symbol = signal.symbol;
        order.side = signal.primary_side;
        order.type = OrderType::LIMIT;
        order.price = signal.entry_price;
        order.quantity = quantity;
        order.strategy_name = "VOL_ARB";
        order.created_time = Clock::now();
        
        return order;
    }
    
    // Should exit based on time or volatility regime change?
    bool should_exit(const VolSignal& entry_signal) {
        // Check hold time
        auto now = Clock::now();
        auto hold_minutes = std::chrono::duration_cast<std::chrono::minutes>(
            now - entry_signal.generated_at
        ).count();
        
        if (hold_minutes > config_.max_hold_minutes) {
            return true;
        }
        
        // Check if regime changed
        auto current_regime = detect_regime();
        if (current_regime != entry_signal.regime) {
            return true;  // Volatility regime changed, exit
        }
        
        return false;
    }
    
    // Statistics
    struct VolArbStats {
        int total_trades;
        int high_vol_trades;
        int low_vol_trades;
        int winning_trades;
        double total_pnl;
        double win_rate;
        double avg_hold_minutes;
        
        VolArbStats()
            : total_trades(0)
            , high_vol_trades(0)
            , low_vol_trades(0)
            , winning_trades(0)
            , total_pnl(0.0)
            , win_rate(0.0)
            , avg_hold_minutes(0.0)
        {}
    };
    
    void record_trade_result(const VolSignal& signal, double pnl, double hold_minutes) {
        stats_.total_trades++;
        stats_.total_pnl += pnl;
        
        if (signal.regime == VolatilityRegime::HIGH) {
            stats_.high_vol_trades++;
        } else if (signal.regime == VolatilityRegime::LOW) {
            stats_.low_vol_trades++;
        }
        
        if (pnl > 0) {
            stats_.winning_trades++;
        }
        
        stats_.avg_hold_minutes = (stats_.avg_hold_minutes * (stats_.total_trades - 1) +
                                   hold_minutes) / stats_.total_trades;
        
        if (stats_.total_trades > 0) {
            stats_.win_rate = static_cast<double>(stats_.winning_trades) / stats_.total_trades;
        }
    }
    
    const VolArbStats& get_stats() const {
        return stats_;
    }
    
    double get_current_atr() const {
        return current_atr_;
    }
    
    double get_avg_atr() const {
        return avg_atr_;
    }
    
    double get_atr_ratio() const {
        return avg_atr_ > 0 ? current_atr_ / avg_atr_ : 1.0;
    }
    
private:
    Config config_;
    
    CircularBuffer<double> price_history_;
    CircularBuffer<double> atr_history_;
    
    double current_atr_ = 0.0;
    double avg_atr_ = 0.0;
    
    VolArbStats stats_;
    
    // Calculate ATR (Average True Range) - uses circular buffer
    double calculate_atr() {
        size_t history_size = price_history_.size();
        if (history_size < config_.atr_period + 1) {
            return 0.0;
        }
        
        double sum_tr = 0.0;
        size_t start_idx = history_size - config_.atr_period;
        
        for (size_t i = start_idx; i < history_size; ++i) {
            double high = price_history_[i];
            double low = price_history_[i];
            double prev_close = price_history_[i-1];
            
            // True Range = max of:
            // 1. high - low
            // 2. abs(high - prev_close)
            // 3. abs(low - prev_close)
            
            double tr = std::max({
                high - low,
                std::abs(high - prev_close),
                std::abs(low - prev_close)
            });
            
            sum_tr += tr;
        }
        
        return sum_tr / config_.atr_period;
    }
    
    // Check if recent price spiked up
    bool is_recent_price_spike_up() const {
        if (price_history_.size() < 10) {
            return false;
        }
        
        double current = price_history_.back();
        double prev_5 = price_history_[price_history_.size() - 6];
        
        double move_pct = (current - prev_5) / prev_5;
        
        return move_pct > 0.01;  // 1%+ move up
    }
    
    // Check if recent price dumped down
    bool is_recent_price_spike_down() const {
        if (price_history_.size() < 10) {
            return false;
        }
        
        double current = price_history_.back();
        double prev_5 = price_history_[price_history_.size() - 6];
        
        double move_pct = (current - prev_5) / prev_5;
        
        return move_pct < -0.01;  // 1%+ move down
    }
};

// Volatility surface tracker (for options-like strategies)
class VolatilitySurfaceTracker {
public:
    struct VolatilitySnapshot {
        TimePoint timestamp;
        double realized_vol;        // Actual volatility
        double implied_vol;         // Expected volatility
        double vol_premium;         // implied - realized
        
        VolatilitySnapshot()
            : realized_vol(0.0)
            , implied_vol(0.0)
            , vol_premium(0.0)
        {}
    };
    
    void add_snapshot(double realized, double implied) {
        VolatilitySnapshot snap;
        snap.timestamp = Clock::now();
        snap.realized_vol = realized;
        snap.implied_vol = implied;
        snap.vol_premium = implied - realized;
        
        history_.push_back(snap);
        
        if (history_.size() > 100) {
            history_.erase(history_.begin());
        }
    }
    
    // Is volatility currently underpriced or overpriced?
    std::string get_volatility_bias() const {
        if (history_.empty()) {
            return "NEUTRAL";
        }
        
        double recent_premium = history_.back().vol_premium;
        
        if (recent_premium > 0.02) {
            return "OVERPRICED";  // Implied > realized, sell vol
        } else if (recent_premium < -0.02) {
            return "UNDERPRICED";  // Implied < realized, buy vol
        } else {
            return "NEUTRAL";
        }
    }
    
private:
    std::vector<VolatilitySnapshot> history_;
};

} // namespace trading
