#pragma once

#include "types.hpp"
#include "order_tracker.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <vector>

namespace trading {

// Position information
struct Position {
    std::string symbol;
    double quantity;                // Signed: positive = long, negative = short
    double avg_price;               // Volume-weighted average entry price
    double realized_pnl;            // Realized P&L (closed trades)
    double unrealized_pnl;          // Mark-to-market unrealized P&L
    double total_fees_paid;         // Total fees paid on this position
    
    // Risk metrics
    double notional_value;          // Absolute notional value
    double var_contribution;        // Contribution to portfolio VaR
    
    // Timing
    TimePoint opened_time;
    TimePoint last_update_time;
    
    Position()
        : quantity(0.0)
        , avg_price(0.0)
        , realized_pnl(0.0)
        , unrealized_pnl(0.0)
        , total_fees_paid(0.0)
        , notional_value(0.0)
        , var_contribution(0.0)
        , opened_time(Clock::now())
        , last_update_time(Clock::now())
    {}
    
    bool is_flat() const {
        return std::abs(quantity) < 0.0000001;
    }
    
    bool is_long() const {
        return quantity > 0.0000001;
    }
    
    bool is_short() const {
        return quantity < -0.0000001;
    }
    
    // Calculate unrealized P&L given current market price
    double calculate_unrealized(double current_price) const {
        if (is_flat()) return 0.0;
        return quantity * (current_price - avg_price);
    }
    
    // Update unrealized P&L with current price
    void update_unrealized(double current_price) {
        unrealized_pnl = calculate_unrealized(current_price);
        notional_value = std::abs(quantity * current_price);
    }
};

// Risk limits
struct RiskLimits {
    // Position limits
    double max_position_per_symbol;     // Max notional per symbol
    double max_total_gross_exposure;    // Max sum of all abs(positions)
    double max_total_net_exposure;      // Max net exposure (long - short)
    
    // P&L limits
    double max_daily_loss;              // Max loss per day
    double max_daily_profit;            // Take-profit limit (optional)
    double trailing_stop_pct;           // Trailing stop from peak P&L
    
    // Order limits
    double max_order_size;              // Max single order notional
    int max_orders_per_second;          // Rate limit
    
    // Concentration limits
    double max_single_symbol_pct;       // Max % of portfolio in one symbol
    
    // Time-based limits
    int max_position_hold_seconds;      // Max time to hold position
    
    RiskLimits()
        : max_position_per_symbol(50000.0)
        , max_total_gross_exposure(150000.0)
        , max_total_net_exposure(100000.0)
        , max_daily_loss(5000.0)
        , max_daily_profit(20000.0)
        , trailing_stop_pct(0.5)  // 50% drawdown from peak
        , max_order_size(10000.0)
        , max_orders_per_second(50)
        , max_single_symbol_pct(0.4)  // 40%
        , max_position_hold_seconds(300)
    {}
};

// Risk manager - institutional grade
class RiskManager {
public:
    explicit RiskManager(const RiskLimits& limits, OrderTracker& order_tracker)
        : limits_(limits)
        , order_tracker_(order_tracker)
        , peak_daily_pnl_(0.0)
    {}
    
    // Pre-trade checks (MUST PASS before sending order)
    struct RiskCheckResult {
        bool passed;
        std::string reason;  // If failed
        
        RiskCheckResult(bool p = true, const std::string& r = "")
            : passed(p), reason(r) {}
    };
    
    RiskCheckResult check_order(const Order& order, double current_price) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        // Check 1: Daily loss limit
        double current_pnl = get_total_pnl_internal(current_price);
        if (current_pnl < -limits_.max_daily_loss) {
            return RiskCheckResult(false, "Daily loss limit exceeded");
        }
        
        // Check 2: Trailing stop from peak
        double drawdown_from_peak = peak_daily_pnl_.load() - current_pnl;
        double max_drawdown = limits_.max_daily_loss * limits_.trailing_stop_pct;
        if (drawdown_from_peak > max_drawdown) {
            return RiskCheckResult(false, "Trailing stop hit");
        }
        
        // Check 3: Order size limit
        double order_notional = order.quantity * order.price;
        if (order_notional > limits_.max_order_size) {
            return RiskCheckResult(false, "Order size exceeds limit");
        }
        
        // Check 4: Position limit for this symbol
        auto pos_it = positions_.find(order.symbol);
        double current_notional = 0.0;
        if (pos_it != positions_.end()) {
            current_notional = std::abs(pos_it->second.quantity * current_price);
        }
        
        // Calculate new position after order
        double new_quantity = (pos_it != positions_.end() ? pos_it->second.quantity : 0.0);
        if (order.side == Side::BUY) {
            new_quantity += order.quantity;
        } else {
            new_quantity -= order.quantity;
        }
        double new_notional = std::abs(new_quantity * current_price);
        
        if (new_notional > limits_.max_position_per_symbol) {
            return RiskCheckResult(false, "Symbol position limit exceeded");
        }
        
        // Check 5: Total gross exposure
        double total_gross = calculate_total_gross_exposure(current_price);
        double order_impact = order_notional;
        
        if (pos_it != positions_.end()) {
            // If reducing position, don't add to gross
            if ((pos_it->second.is_long() && order.side == Side::SELL) ||
                (pos_it->second.is_short() && order.side == Side::BUY)) {
                order_impact = std::max(0.0, new_notional - current_notional);
            }
        }
        
        if (total_gross + order_impact > limits_.max_total_gross_exposure) {
            return RiskCheckResult(false, "Total gross exposure limit exceeded");
        }
        
        // Check 6: Concentration limit
        double portfolio_value = total_gross + order_impact;
        if (portfolio_value > 0 && new_notional / portfolio_value > limits_.max_single_symbol_pct) {
            return RiskCheckResult(false, "Concentration limit exceeded");
        }
        
        return RiskCheckResult(true);
    }
    
    // Process fill and update positions
    void on_fill(const Fill& fill) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        auto& pos = positions_[fill.symbol];
        
        double signed_quantity = (fill.side == Side::BUY) ? fill.quantity : -fill.quantity;
        
        if (pos.is_flat()) {
            // Opening new position
            pos.symbol = fill.symbol;
            pos.quantity = signed_quantity;
            pos.avg_price = fill.price;
            pos.opened_time = fill.received_time;
            pos.total_fees_paid = fill.fee;
            
        } else if ((pos.is_long() && fill.side == Side::BUY) ||
                   (pos.is_short() && fill.side == Side::SELL)) {
            // Adding to position
            double total_cost = (pos.quantity * pos.avg_price) + (signed_quantity * fill.price);
            pos.quantity += signed_quantity;
            pos.avg_price = total_cost / pos.quantity;
            pos.total_fees_paid += fill.fee;
            
        } else {
            // Closing or reducing position
            double closed_quantity = std::min(std::abs(signed_quantity), std::abs(pos.quantity));
            double pnl = closed_quantity * (fill.price - pos.avg_price) * 
                        (pos.is_long() ? 1.0 : -1.0);
            
            pos.realized_pnl += (pnl - fill.fee);
            pos.quantity += signed_quantity;
            pos.total_fees_paid += fill.fee;
            
            // Update daily P&L atomically
            daily_realized_pnl_.fetch_add(pnl - fill.fee, std::memory_order_relaxed);
        }
        
        pos.last_update_time = Clock::now();
        
        // Track fills for analysis
        recent_fills_.push_back(fill);
        if (recent_fills_.size() > 1000) {
            recent_fills_.erase(recent_fills_.begin());
        }
    }
    
    // Update unrealized P&L with current market prices
    void update_market_prices(const std::unordered_map<std::string, double>& prices) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        double total_unrealized = 0.0;
        
        for (auto& [symbol, pos] : positions_) {
            auto it = prices.find(symbol);
            if (it != prices.end()) {
                pos.update_unrealized(it->second);
                total_unrealized += pos.unrealized_pnl;
            }
        }
        
        // Update peak for trailing stop
        double total_pnl = daily_realized_pnl_.load(std::memory_order_relaxed) + total_unrealized;
        
        double current_peak = peak_daily_pnl_.load(std::memory_order_relaxed);
        while (total_pnl > current_peak) {
            if (peak_daily_pnl_.compare_exchange_weak(current_peak, total_pnl,
                                                       std::memory_order_relaxed)) {
                break;
            }
        }
    }
    
    // Get position
    std::optional<Position> get_position(const std::string& symbol) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        auto it = positions_.find(symbol);
        if (it != positions_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    // Get all positions
    std::vector<Position> get_all_positions() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        std::vector<Position> result;
        result.reserve(positions_.size());
        
        for (const auto& [symbol, pos] : positions_) {
            if (!pos.is_flat()) {
                result.push_back(pos);
            }
        }
        
        return result;
    }
    
    // Get total P&L (realized + unrealized)
    double get_total_pnl(const std::unordered_map<std::string, double>& current_prices) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return get_total_pnl_internal(current_prices);
    }
    
    // Risk metrics
    double calculate_total_gross_exposure(double generic_price = 0.0) const {
        double total = 0.0;
        for (const auto& [symbol, pos] : positions_) {
            total += pos.notional_value;
        }
        return total;
    }
    
    double calculate_total_net_exposure() const {
        double total = 0.0;
        for (const auto& [symbol, pos] : positions_) {
            total += pos.quantity * pos.avg_price;  // Signed
        }
        return total;
    }
    
    // Reset daily P&L (call at start of trading day)
    void reset_daily() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        daily_realized_pnl_.store(0.0, std::memory_order_relaxed);
        peak_daily_pnl_.store(0.0, std::memory_order_relaxed);
        
        for (auto& [symbol, pos] : positions_) {
            pos.realized_pnl = 0.0;
            pos.unrealized_pnl = 0.0;
        }
        
        recent_fills_.clear();
    }
    
    // Statistics
    struct RiskStats {
        double total_realized_pnl;
        double total_unrealized_pnl;
        double total_pnl;
        double gross_exposure;
        double net_exposure;
        double peak_pnl_today;
        double drawdown_from_peak;
        size_t num_positions;
        size_t num_fills;
    };
    
    RiskStats get_stats(const std::unordered_map<std::string, double>& current_prices) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        RiskStats stats;
        stats.total_realized_pnl = daily_realized_pnl_.load(std::memory_order_relaxed);
        
        double unrealized = 0.0;
        for (const auto& [symbol, pos] : positions_) {
            unrealized += pos.unrealized_pnl;
        }
        stats.total_unrealized_pnl = unrealized;
        stats.total_pnl = stats.total_realized_pnl + stats.total_unrealized_pnl;
        
        stats.gross_exposure = calculate_total_gross_exposure();
        stats.net_exposure = calculate_total_net_exposure();
        stats.peak_pnl_today = peak_daily_pnl_.load(std::memory_order_relaxed);
        stats.drawdown_from_peak = stats.peak_pnl_today - stats.total_pnl;
        stats.num_positions = positions_.size();
        stats.num_fills = recent_fills_.size();
        
        return stats;
    }
    
private:
    RiskLimits limits_;
    OrderTracker& order_tracker_;
    
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Position> positions_;
    
    std::atomic<double> daily_realized_pnl_{0.0};
    std::atomic<double> peak_daily_pnl_{0.0};
    
    std::vector<Fill> recent_fills_;  // For analysis
    
    // Internal helper (assumes lock held)
    double get_total_pnl_internal(double generic_price) const {
        double realized = daily_realized_pnl_.load(std::memory_order_relaxed);
        double unrealized = 0.0;
        
        for (const auto& [symbol, pos] : positions_) {
            unrealized += pos.unrealized_pnl;
        }
        
        return realized + unrealized;
    }
    
    double get_total_pnl_internal(const std::unordered_map<std::string, double>& prices) const {
        double realized = daily_realized_pnl_.load(std::memory_order_relaxed);
        double unrealized = 0.0;
        
        for (const auto& [symbol, pos] : positions_) {
            auto it = prices.find(symbol);
            if (it != prices.end()) {
                unrealized += pos.calculate_unrealized(it->second);
            }
        }
        
        return realized + unrealized;
    }
};

} // namespace trading
