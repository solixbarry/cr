#pragma once

#include "../core/types.hpp"
#include "../core/circular_buffer.hpp"
#include <cmath>
#include <vector>

namespace trading {

// Incremental statistics calculation (Welford's algorithm)
class RunningStats {
public:
    RunningStats() : count_(0), mean_(0.0), m2_(0.0) {}
    
    void push(double x) {
        count_++;
        double delta = x - mean_;
        mean_ += delta / count_;
        double delta2 = x - mean_;
        m2_ += delta * delta2;
    }
    
    void pop_front(double x) {
        if (count_ == 0) return;
        count_--;
        if (count_ == 0) {
            mean_ = 0.0;
            m2_ = 0.0;
            return;
        }
        double delta = x - mean_;
        mean_ -= delta / count_;
        double delta2 = x - mean_;
        m2_ -= delta * delta2;
    }
    
    double mean() const { return mean_; }
    double variance() const { return count_ > 1 ? m2_ / (count_ - 1) : 0.0; }
    double stddev() const { return std::sqrt(variance()); }
    int count() const { return count_; }
    
private:
    int count_;
    double mean_;
    double m2_;  // Sum of squared differences from mean
};

// Pairs Trading - Mean reversion on correlated pairs
class PairsTradingStrategy {
public:
    struct Config {
        std::string symbol1;                // e.g., "ETHUSDT"
        std::string symbol2;                // e.g., "BTCUSDT"
        
        int lookback_period;                // Historical window (100-500)
        double entry_z_score;               // Entry threshold (1.5-2.5)
        double exit_z_score;                // Exit threshold (0.0-0.5)
        double stop_loss_z_score;           // Stop loss (3.0-4.0)
        
        double position_size_usd;           // Size per leg
        double min_correlation;             // Min correlation (0.7-0.9)
        
        Config()
            : symbol1("ETHUSDT")
            , symbol2("BTCUSDT")
            , lookback_period(200)
            , entry_z_score(2.0)
            , exit_z_score(0.3)
            , stop_loss_z_score(3.5)
            , position_size_usd(5000.0)
            , min_correlation(0.75)
        {}
    };
    
    struct PairSignal {
        std::string symbol1;
        std::string symbol2;
        
        Side symbol1_side;                  // Long or short symbol1
        Side symbol2_side;                  // Opposite of symbol1
        
        double ratio;                       // Current price ratio
        double mean_ratio;                  // Historical mean
        double std_ratio;                   // Historical std dev
        double z_score;                     // How many std devs from mean
        
        double entry_price1;
        double entry_price2;
        double target_price1;
        double target_price2;
        double stop_price1;
        double stop_price2;
        
        double expected_profit_bps;
        TimePoint generated_at;
        bool is_valid;
        
        PairSignal()
            : symbol1_side(Side::BUY)
            , symbol2_side(Side::SELL)
            , ratio(0.0)
            , mean_ratio(0.0)
            , std_ratio(0.0)
            , z_score(0.0)
            , entry_price1(0.0)
            , entry_price2(0.0)
            , target_price1(0.0)
            , target_price2(0.0)
            , stop_price1(0.0)
            , stop_price2(0.0)
            , expected_profit_bps(0.0)
            , is_valid(false)
        {}
    };
    
    explicit PairsTradingStrategy(const Config& config)
        : config_(config)
        , stats_calculator_()
        , ratio_history_(config.lookback_period)
    {}
    
    // Update with new prices - EFFICIENT O(1) with circular buffer
    void update_prices(double price1, double price2) {
        double ratio = price1 / price2;
        
        // Circular buffer auto-overwrites oldest
        if (ratio_history_.full()) {
            double old_ratio = ratio_history_.front();
            stats_calculator_.pop_front(old_ratio);
        }
        
        ratio_history_.push_back(ratio);
        stats_calculator_.push(ratio);
        
        // Update cached statistics
        if (stats_calculator_.count() >= 20) {
            mean_ratio_ = stats_calculator_.mean();
            std_ratio_ = stats_calculator_.stddev();
        }
    }
    
    // Generate trading signal
    PairSignal generate_signal(double current_price1, double current_price2) {
        PairSignal signal;
        signal.symbol1 = config_.symbol1;
        signal.symbol2 = config_.symbol2;
        signal.generated_at = Clock::now();
        
        if (ratio_history_.size() < config_.lookback_period / 2) {
            return signal;  // Not enough data
        }
        
        double current_ratio = current_price1 / current_price2;
        signal.ratio = current_ratio;
        signal.mean_ratio = mean_ratio_;
        signal.std_ratio = std_ratio_;
        
        // Calculate z-score
        if (std_ratio_ < 0.000001) {
            return signal;  // No volatility
        }
        
        double z_score = (current_ratio - mean_ratio_) / std_ratio_;
        signal.z_score = z_score;
        
        // Check entry conditions
        if (std::abs(z_score) < config_.entry_z_score) {
            return signal;  // Not far enough from mean
        }
        
        // Ratio too high → short symbol1, long symbol2
        if (z_score > config_.entry_z_score) {
            signal.symbol1_side = Side::SELL;  // Short expensive
            signal.symbol2_side = Side::BUY;   // Long cheap
            
            signal.entry_price1 = current_price1;
            signal.entry_price2 = current_price2;
            
            // Target: ratio returns to mean
            signal.target_price1 = mean_ratio_ * current_price2;
            signal.target_price2 = current_price2;  // Hold this
            
            // Stop: ratio goes even further
            double stop_ratio = mean_ratio_ + (config_.stop_loss_z_score * std_ratio_);
            signal.stop_price1 = stop_ratio * current_price2;
            signal.stop_price2 = current_price2;
            
            signal.is_valid = true;
        }
        // Ratio too low → long symbol1, short symbol2
        else if (z_score < -config_.entry_z_score) {
            signal.symbol1_side = Side::BUY;   // Long cheap
            signal.symbol2_side = Side::SELL;  // Short expensive
            
            signal.entry_price1 = current_price1;
            signal.entry_price2 = current_price2;
            
            signal.target_price1 = mean_ratio_ * current_price2;
            signal.target_price2 = current_price2;
            
            double stop_ratio = mean_ratio_ - (config_.stop_loss_z_score * std_ratio_);
            signal.stop_price1 = stop_ratio * current_price2;
            signal.stop_price2 = current_price2;
            
            signal.is_valid = true;
        }
        
        // Calculate expected profit
        if (signal.is_valid) {
            double entry_ratio = signal.entry_price1 / signal.entry_price2;
            double target_ratio = mean_ratio_;
            
            signal.expected_profit_bps = std::abs((target_ratio - entry_ratio) / entry_ratio) * 10000.0;
        }
        
        return signal;
    }
    
    // Check if should exit position
    bool should_exit(double current_price1, double current_price2, const PairSignal& entry_signal) {
        double current_ratio = current_price1 / current_price2;
        double current_z = (current_ratio - mean_ratio_) / std_ratio_;
        
        // Exit if returned close to mean
        if (std::abs(current_z) < config_.exit_z_score) {
            return true;
        }
        
        // Stop loss if moved against us
        if (entry_signal.symbol1_side == Side::SELL) {
            // We shorted symbol1, stop if ratio keeps increasing
            if (current_z > config_.stop_loss_z_score) {
                return true;
            }
        } else {
            // We longed symbol1, stop if ratio keeps decreasing
            if (current_z < -config_.stop_loss_z_score) {
                return true;
            }
        }
        
        return false;
    }
    
    // Create orders for pair trade
    std::pair<Order, Order> create_pair_orders(const PairSignal& signal) {
        // Calculate quantities to maintain dollar-neutral
        double qty1 = config_.position_size_usd / signal.entry_price1;
        double qty2 = config_.position_size_usd / signal.entry_price2;
        
        // Order for symbol1
        Order order1;
        order1.symbol = signal.symbol1;
        order1.side = signal.symbol1_side;
        order1.type = OrderType::LIMIT;
        order1.price = signal.entry_price1;
        order1.quantity = qty1;
        order1.strategy_name = "PAIRS_TRADING";
        order1.created_time = Clock::now();
        
        // Order for symbol2
        Order order2;
        order2.symbol = signal.symbol2;
        order2.side = signal.symbol2_side;
        order2.type = OrderType::LIMIT;
        order2.price = signal.entry_price2;
        order2.quantity = qty2;
        order2.strategy_name = "PAIRS_TRADING";
        order2.created_time = Clock::now();
        
        return {order1, order2};
    }
    
    // Calculate correlation between the pair
    double calculate_correlation() const {
        if (price1_history_.size() < 20 || price2_history_.size() < 20) {
            return 0.0;
        }
        
        size_t n = std::min(price1_history_.size(), price2_history_.size());
        
        // Calculate means
        double mean1 = 0.0, mean2 = 0.0;
        for (size_t i = 0; i < n; ++i) {
            mean1 += price1_history_[i];
            mean2 += price2_history_[i];
        }
        mean1 /= n;
        mean2 /= n;
        
        // Calculate correlation
        double numerator = 0.0;
        double sum_sq1 = 0.0, sum_sq2 = 0.0;
        
        for (size_t i = 0; i < n; ++i) {
            double diff1 = price1_history_[i] - mean1;
            double diff2 = price2_history_[i] - mean2;
            
            numerator += diff1 * diff2;
            sum_sq1 += diff1 * diff1;
            sum_sq2 += diff2 * diff2;
        }
        
        double denominator = std::sqrt(sum_sq1 * sum_sq2);
        if (denominator < 0.000001) return 0.0;
        
        return numerator / denominator;
    }
    
    // Statistics
    struct PairsStats {
        int total_trades;
        int winning_trades;
        int losing_trades;
        double total_pnl;
        double win_rate;
        double avg_z_score_at_entry;
        double avg_hold_time_minutes;
        
        PairsStats()
            : total_trades(0)
            , winning_trades(0)
            , losing_trades(0)
            , total_pnl(0.0)
            , win_rate(0.0)
            , avg_z_score_at_entry(0.0)
            , avg_hold_time_minutes(0.0)
        {}
    };
    
    void record_trade_result(const PairSignal& signal, double pnl, double hold_minutes) {
        stats_.total_trades++;
        stats_.total_pnl += pnl;
        
        if (pnl > 0) {
            stats_.winning_trades++;
        } else {
            stats_.losing_trades++;
        }
        
        // Update averages
        stats_.avg_z_score_at_entry = (stats_.avg_z_score_at_entry * (stats_.total_trades - 1) +
                                       std::abs(signal.z_score)) / stats_.total_trades;
        
        stats_.avg_hold_time_minutes = (stats_.avg_hold_time_minutes * (stats_.total_trades - 1) +
                                        hold_minutes) / stats_.total_trades;
        
        if (stats_.total_trades > 0) {
            stats_.win_rate = static_cast<double>(stats_.winning_trades) / stats_.total_trades;
        }
    }
    
    const PairsStats& get_stats() const {
        return stats_;
    }
    
    double get_current_z_score() const {
        if (ratio_history_.empty() || std_ratio_ < 0.000001) {
            return 0.0;
        }
        return (ratio_history_.back() - mean_ratio_) / std_ratio_;
    }
    
private:
    Config config_;
    
    CircularBuffer<double> ratio_history_;
    std::deque<double> price1_history_;  // Keep for correlation calc
    std::deque<double> price2_history_;  // Keep for correlation calc
    
    // Incremental statistics - O(1) updates
    RunningStats stats_calculator_;
    
    double mean_ratio_ = 0.0;
    double std_ratio_ = 0.0;
    
    PairsStats stats_;
};

// Multi-pair manager
class MultiPairManager {
public:
    void add_pair(const std::string& symbol1, const std::string& symbol2,
                  const PairsTradingStrategy::Config& config) {
        std::string pair_key = symbol1 + "_" + symbol2;
        pairs_[pair_key] = std::make_unique<PairsTradingStrategy>(config);
    }
    
    void update_all_prices(const std::unordered_map<std::string, double>& prices) {
        for (auto& [pair_key, strategy] : pairs_) {
            // Parse pair_key to get symbols
            // Update strategy with prices
        }
    }
    
    std::vector<PairsTradingStrategy::PairSignal> generate_all_signals(
        const std::unordered_map<std::string, double>& prices)
    {
        std::vector<PairsTradingStrategy::PairSignal> signals;
        
        for (auto& [pair_key, strategy] : pairs_) {
            // Generate signal for each pair
            // Add to signals vector if valid
        }
        
        return signals;
    }
    
private:
    std::unordered_map<std::string, std::unique_ptr<PairsTradingStrategy>> pairs_;
};

} // namespace trading
