#pragma once

#include "../core/types.hpp"
#include "../market_data/order_book.hpp"
#include <deque>
#include <cmath>

namespace trading {

// Order Book Imbalance (OBI) - Predicts short-term price movements
class OrderBookImbalanceStrategy {
public:
    struct Config {
        int num_levels;                 // Levels to analyze (typically 5-10)
        double imbalance_threshold;     // Min imbalance to trade (0.3-0.5)
        double min_volume_threshold;    // Min total volume (filters noise)
        double target_profit_bps;       // Take profit (5-15 bps)
        double stop_loss_bps;           // Stop loss (3-8 bps)
        int signal_decay_ms;            // Signal expires after this (100-500ms)
        
        Config()
            : num_levels(5)
            , imbalance_threshold(0.35)
            , min_volume_threshold(10.0)  // 10 BTC equivalent
            , target_profit_bps(10.0)
            , stop_loss_bps(5.0)
            , signal_decay_ms(200)
        {}
    };
    
    struct OBISignal {
        std::string symbol;
        Side predicted_direction;
        double imbalance_ratio;         // -1.0 to +1.0
        double confidence;              // 0.0 to 1.0
        double entry_price;
        double target_price;
        double stop_price;
        TimePoint generated_at;
        bool is_valid;
        
        OBISignal() : predicted_direction(Side::BUY), imbalance_ratio(0.0), 
                      confidence(0.0), entry_price(0.0), target_price(0.0),
                      stop_price(0.0), is_valid(false) {}
    };
    
    explicit OrderBookImbalanceStrategy(const Config& config)
        : config_(config)
    {}
    
    // Analyze order book and generate signal
    OBISignal analyze(const std::string& symbol, const OrderBook& book) {
        OBISignal signal;
        signal.symbol = symbol;
        signal.generated_at = Clock::now();
        
        // Calculate bid/ask volume in top N levels - EFFICIENT O(1)
        double bid_volume = 0.0;
        double ask_volume = 0.0;
        
        const auto& bids = book.get_bids();
        const auto& asks = book.get_asks();
        
        // Direct iteration - no std::advance needed
        int bid_count = 0;
        for (const auto& [price, qty] : bids) {
            if (bid_count >= config_.num_levels) break;
            bid_volume += qty;
            ++bid_count;
        }
        
        int ask_count = 0;
        for (const auto& [price, qty] : asks) {
            if (ask_count >= config_.num_levels) break;
            ask_volume += qty;
            ++ask_count;
        }
        
        // Check minimum volume threshold
        double total_volume = bid_volume + ask_volume;
        if (total_volume < config_.min_volume_threshold) {
            return signal;  // Not enough volume, skip
        }
        
        // Calculate imbalance ratio: -1 (all asks) to +1 (all bids)
        double imbalance = (bid_volume - ask_volume) / total_volume;
        signal.imbalance_ratio = imbalance;
        
        // Generate signal if imbalance exceeds threshold
        double abs_imbalance = std::abs(imbalance);
        
        if (abs_imbalance < config_.imbalance_threshold) {
            return signal;  // Imbalance too small
        }
        
        // Strong bid volume → predict price UP
        if (imbalance > config_.imbalance_threshold) {
            signal.predicted_direction = Side::BUY;
            signal.confidence = std::min(abs_imbalance / 0.7, 1.0);  // Scale to 0-1
            
            double mid = book.get_mid_price();
            signal.entry_price = mid;
            signal.target_price = mid * (1.0 + config_.target_profit_bps / 10000.0);
            signal.stop_price = mid * (1.0 - config_.stop_loss_bps / 10000.0);
            signal.is_valid = true;
        }
        // Strong ask volume → predict price DOWN
        else if (imbalance < -config_.imbalance_threshold) {
            signal.predicted_direction = Side::SELL;
            signal.confidence = std::min(abs_imbalance / 0.7, 1.0);
            
            double mid = book.get_mid_price();
            signal.entry_price = mid;
            signal.target_price = mid * (1.0 - config_.target_profit_bps / 10000.0);
            signal.stop_price = mid * (1.0 + config_.stop_loss_bps / 10000.0);
            signal.is_valid = true;
        }
        
        return signal;
    }
    
    // Check if signal has expired
    bool is_signal_expired(const OBISignal& signal) const {
        auto now = Clock::now();
        auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - signal.generated_at
        ).count();
        
        return age_ms > config_.signal_decay_ms;
    }
    
    // Convert to Order
    Order create_order_from_signal(const OBISignal& signal, double quantity) const {
        Order order;
        order.symbol = signal.symbol;
        order.side = signal.predicted_direction;
        order.type = OrderType::LIMIT;
        order.price = signal.entry_price;
        order.quantity = quantity;
        order.strategy_name = "OBI";
        order.created_time = Clock::now();
        
        return order;
    }
    
    // Historical tracking for analysis
    struct OBIStats {
        int total_signals;
        int winning_trades;
        int losing_trades;
        double total_pnl;
        double win_rate;
        double avg_hold_time_ms;
        
        OBIStats() : total_signals(0), winning_trades(0), losing_trades(0),
                     total_pnl(0.0), win_rate(0.0), avg_hold_time_ms(0.0) {}
    };
    
    void record_trade_result(const OBISignal& signal, double pnl, int hold_time_ms) {
        stats_.total_signals++;
        stats_.total_pnl += pnl;
        
        if (pnl > 0) {
            stats_.winning_trades++;
        } else {
            stats_.losing_trades++;
        }
        
        // Update rolling average
        stats_.avg_hold_time_ms = (stats_.avg_hold_time_ms * (stats_.total_signals - 1) + 
                                   hold_time_ms) / stats_.total_signals;
        
        if (stats_.total_signals > 0) {
            stats_.win_rate = static_cast<double>(stats_.winning_trades) / stats_.total_signals;
        }
    }
    
    const OBIStats& get_stats() const {
        return stats_;
    }
    
    void reset_stats() {
        stats_ = OBIStats();
    }
    
private:
    Config config_;
    OBIStats stats_;
};

// Multi-level imbalance (weighted by distance from mid)
class WeightedOBIStrategy {
public:
    struct Config {
        int num_levels;
        double imbalance_threshold;
        std::vector<double> level_weights;  // Weight per level (closer = higher)
        
        Config() 
            : num_levels(5)
            , imbalance_threshold(0.35)
            , level_weights({1.0, 0.8, 0.6, 0.4, 0.2})  // Exponential decay
        {}
    };
    
    explicit WeightedOBIStrategy(const Config& config) : config_(config) {}
    
    // Calculate weighted imbalance
    double calculate_weighted_imbalance(const OrderBook& book) const {
        const auto& bids = book.get_bids();
        const auto& asks = book.get_asks();
        
        double weighted_bid_volume = 0.0;
        double weighted_ask_volume = 0.0;
        
        int level = 0;
        for (const auto& [price, qty] : bids) {
            if (level >= config_.num_levels) break;
            double weight = (level < config_.level_weights.size()) ? 
                           config_.level_weights[level] : 0.1;
            weighted_bid_volume += qty * weight;
            ++level;
        }
        
        level = 0;
        for (const auto& [price, qty] : asks) {
            if (level >= config_.num_levels) break;
            double weight = (level < config_.level_weights.size()) ? 
                           config_.level_weights[level] : 0.1;
            weighted_ask_volume += qty * weight;
            ++level;
        }
        
        double total = weighted_bid_volume + weighted_ask_volume;
        if (total < 0.0001) return 0.0;
        
        return (weighted_bid_volume - weighted_ask_volume) / total;
    }
    
private:
    Config config_;
};

// Real-time imbalance tracker with history
class OBITracker {
public:
    struct Snapshot {
        TimePoint timestamp;
        double imbalance;
        double bid_volume;
        double ask_volume;
    };
    
    explicit OBITracker(int history_size = 100) 
        : max_history_(history_size) {}
    
    void add_snapshot(const std::string& symbol, const OrderBook& book, double imbalance) {
        const auto& bids = book.get_bids();
        const auto& asks = book.get_asks();
        
        double bid_vol = 0.0;
        double ask_vol = 0.0;
        
        int count = 0;
        for (const auto& [price, qty] : bids) {
            if (count >= 5) break;
            bid_vol += qty;
            ++count;
        }
        
        count = 0;
        for (const auto& [price, qty] : asks) {
            if (count >= 5) break;
            ask_vol += qty;
            ++count;
        }
        
        Snapshot snap;
        snap.timestamp = Clock::now();
        snap.imbalance = imbalance;
        snap.bid_volume = bid_vol;
        snap.ask_volume = ask_vol;
        
        auto& history = history_[symbol];
        history.push_back(snap);
        
        // Bounded history - prevent memory leak
        if (history.size() > max_history_) {
            history.erase(history.begin());
        }
    }
    
    // Get recent trend (are we getting more bullish or bearish?)
    double get_trend(const std::string& symbol, int lookback = 10) const {
        auto it = history_.find(symbol);
        if (it == history_.end() || it->second.size() < 2) {
            return 0.0;
        }
        
        const auto& hist = it->second;
        int n = std::min(lookback, static_cast<int>(hist.size()));
        
        if (n < 2) return 0.0;
        
        double first_imbalance = hist[hist.size() - n].imbalance;
        double last_imbalance = hist.back().imbalance;
        
        return last_imbalance - first_imbalance;  // Positive = getting more bullish
    }
    
    const std::vector<Snapshot>& get_history(const std::string& symbol) const {
        static std::vector<Snapshot> empty;
        auto it = history_.find(symbol);
        return it != history_.end() ? it->second : empty;
    }
    
private:
    int max_history_;
    std::unordered_map<std::string, std::vector<Snapshot>> history_;
};

} // namespace trading
