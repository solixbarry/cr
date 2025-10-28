#pragma once

#include "../core/types.hpp"
#include "../core/circular_buffer.hpp"
#include <cmath>
#include <mutex>
#include <atomic>
#include <chrono>

namespace trading {

// Forward declare if needed
#ifndef LOG_ERROR
#define LOG_ERROR(msg) std::cerr << "[ERROR] " << msg << std::endl
#define LOG_WARN(msg) std::cerr << "[WARN] " << msg << std::endl
#define LOG_INFO(msg) std::cout << "[INFO] " << msg << std::endl
#endif

// Adverse Selection Filter - Detects toxic order flow
class AdverseSelectionFilter {
public:
    struct Config {
        int lookback_trades;                // How many trades to analyze
        double toxic_threshold;             // Toxicity score to widen spread
        double spread_multiplier_low;       // Multiplier for low toxicity
        double spread_multiplier_medium;    // Multiplier for medium toxicity
        double spread_multiplier_high;      // Multiplier for high toxicity
        int price_movement_window_ms;       // Window to check price impact
        double significant_price_move_bps;  // What's a "significant" move
        
        Config()
            : lookback_trades(20)
            , toxic_threshold(0.6)
            , spread_multiplier_low(1.0)
            , spread_multiplier_medium(1.5)
            , spread_multiplier_high(2.5)
            , price_movement_window_ms(500)
            , significant_price_move_bps(5.0)
        {}
    };
    
    struct ToxicityMetrics {
        double toxicity_score;              // 0.0-1.0
        std::string toxicity_level;         // "LOW", "MEDIUM", "HIGH"
        double recommended_spread_mult;     // Spread multiplier
        
        // Components
        double fill_adverse_ratio;          // % of fills that moved against us
        double avg_adverse_move_bps;        // Avg move after adverse fills
        double trade_size_percentile;       // Current trade size vs history
        double time_since_last_toxic_fill_ms;
        
        ToxicityMetrics()
            : toxicity_score(0.0)
            , toxicity_level("LOW")
            , recommended_spread_mult(1.0)
            , fill_adverse_ratio(0.0)
            , avg_adverse_move_bps(0.0)
            , trade_size_percentile(0.5)
            , time_since_last_toxic_fill_ms(10000.0)
        {}
    };
    
    struct FillEvent {
        Side our_side;                      // Which side we filled
        double fill_price;
        double fill_quantity;
        TimePoint fill_time;
        double price_after_500ms;           // Price 500ms later
        bool was_adverse;                   // Did price move against us?
        double adverse_move_bps;            // How much it moved
        
        FillEvent()
            : our_side(Side::BUY)
            , fill_price(0.0)
            , fill_quantity(0.0)
            , price_after_500ms(0.0)
            , was_adverse(false)
            , adverse_move_bps(0.0)
        {}
    };
    
    explicit AdverseSelectionFilter(const Config& config)
        : config_(config)
        , fill_history_(config.lookback_trades)
        , cached_toxicity_score_(0.0)
        , cached_spread_mult_(1.0)
        , needs_recalc_(true)
    {}
    
    // Record a fill (THREAD-SAFE) - circular buffer auto-manages size
    void record_fill(Side our_side, double price, double quantity) {
        std::lock_guard<std::mutex> lock(fills_mutex_);
        
        FillEvent fill;
        fill.our_side = our_side;
        fill.fill_price = price;
        fill.fill_quantity = quantity;
        fill.fill_time = Clock::now();
        
        fill_history_.push_back(fill);  // Auto-overwrites oldest
        
        needs_recalc_.store(true, std::memory_order_release);
    }
    
    // Update price information (call periodically) - THREAD-SAFE
    void update_current_price(double price) {
        std::lock_guard<std::mutex> lock(fills_mutex_);
        
        auto now = Clock::now();
        bool any_updated = false;
        
        // Check old fills - circular buffer supports range-based for
        for (auto& fill : fill_history_) {
            if (fill.price_after_500ms > 0.0) {
                continue;  // Already analyzed
            }
            
            auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - fill.fill_time
            ).count();
            
            if (age_ms >= config_.price_movement_window_ms) {
                fill.price_after_500ms = price;
                
                // Check if adverse
                if (fill.our_side == Side::BUY) {
                    // We bought, adverse if price went down
                    double move_bps = ((price - fill.fill_price) / fill.fill_price) * 10000.0;
                    fill.adverse_move_bps = move_bps;
                    fill.was_adverse = (move_bps < -config_.significant_price_move_bps);
                } else {
                    // We sold, adverse if price went up
                    double move_bps = ((price - fill.fill_price) / fill.fill_price) * 10000.0;
                    fill.adverse_move_bps = move_bps;
                    fill.was_adverse = (move_bps > config_.significant_price_move_bps);
                }
                
                if (fill.was_adverse) {
                    last_toxic_fill_time_ = now;
                }
                
                any_updated = true;
            }
        }
        
        if (any_updated) {
            needs_recalc_.store(true, std::memory_order_release);
        }
    }
    
    // Calculate toxicity metrics (CACHED)
    ToxicityMetrics calculate_toxicity() {
        // Check if we need to recalculate
        if (!needs_recalc_.load(std::memory_order_acquire)) {
            ToxicityMetrics cached;
            cached.toxicity_score = cached_toxicity_score_.load(std::memory_order_relaxed);
            cached.recommended_spread_mult = cached_spread_mult_.load(std::memory_order_relaxed);
            return cached;
        }
        
        std::lock_guard<std::mutex> lock(fills_mutex_);
        
        ToxicityMetrics metrics;
        
        // Count adverse fills
        int adverse_count = 0;
        double total_adverse_move = 0.0;
        int analyzed_fills = 0;
        
        for (const auto& fill : fill_history_) {
            if (fill.price_after_500ms > 0.0) {  // Has been analyzed
                analyzed_fills++;
                
                if (fill.was_adverse) {
                    adverse_count++;
                    total_adverse_move += std::abs(fill.adverse_move_bps);
                }
            }
        }
        
        if (analyzed_fills > 0) {
            metrics.fill_adverse_ratio = static_cast<double>(adverse_count) / analyzed_fills;
        }
        
        if (adverse_count > 0) {
            metrics.avg_adverse_move_bps = total_adverse_move / adverse_count;
        }
        
        // Time since last toxic fill
        auto now = Clock::now();
        if (last_toxic_fill_time_ != TimePoint{}) {
            metrics.time_since_last_toxic_fill_ms = 
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_toxic_fill_time_
                ).count();
        }
        
        // Calculate overall toxicity score (0-1)
        // Higher adverse ratio = more toxic
        // Larger adverse moves = more toxic
        // Recent toxic fills = more toxic
        
        double ratio_component = metrics.fill_adverse_ratio;
        double magnitude_component = std::min(metrics.avg_adverse_move_bps / 20.0, 1.0);
        double recency_component = std::max(0.0, 1.0 - (metrics.time_since_last_toxic_fill_ms / 10000.0));
        
        metrics.toxicity_score = (ratio_component * 0.5 + 
                                  magnitude_component * 0.3 +
                                  recency_component * 0.2);
        
        // Determine toxicity level and spread multiplier
        if (metrics.toxicity_score < 0.3) {
            metrics.toxicity_level = "LOW";
            metrics.recommended_spread_mult = config_.spread_multiplier_low;
        } else if (metrics.toxicity_score < 0.6) {
            metrics.toxicity_level = "MEDIUM";
            metrics.recommended_spread_mult = config_.spread_multiplier_medium;
        } else {
            metrics.toxicity_level = "HIGH";
            metrics.recommended_spread_mult = config_.spread_multiplier_high;
        }
        
        // Cache results
        cached_toxicity_score_.store(metrics.toxicity_score, std::memory_order_relaxed);
        cached_spread_mult_.store(metrics.recommended_spread_mult, std::memory_order_relaxed);
        needs_recalc_.store(false, std::memory_order_release);
        
        return metrics;
    }
    
    // Should we widen spreads?
    bool should_widen_spreads() {
        auto metrics = calculate_toxicity();
        return metrics.toxicity_score > config_.toxic_threshold;
    }
    
    // Get recommended spread adjustment
    double get_spread_multiplier() {
        return calculate_toxicity().recommended_spread_mult;
    }
    
    // Reset history (e.g., new trading session)
    void reset() {
        fill_history_.clear();
        last_toxic_fill_time_ = TimePoint{};
    }
    
    // Statistics
    struct AdverseSelectionStats {
        int total_fills;
        int adverse_fills;
        double adverse_fill_rate;
        double avg_adverse_move_bps;
        double total_adverse_cost;          // Total cost from adverse selection
        
        AdverseSelectionStats()
            : total_fills(0)
            , adverse_fills(0)
            , adverse_fill_rate(0.0)
            , avg_adverse_move_bps(0.0)
            , total_adverse_cost(0.0)
        {}
    };
    
    AdverseSelectionStats get_stats() const {
        AdverseSelectionStats stats;
        
        for (const auto& fill : fill_history_) {
            if (fill.price_after_500ms > 0.0) {
                stats.total_fills++;
                
                if (fill.was_adverse) {
                    stats.adverse_fills++;
                    double cost = std::abs(fill.adverse_move_bps) * 
                                 (fill.fill_quantity * fill.fill_price) / 10000.0;
                    stats.total_adverse_cost += cost;
                }
            }
        }
        
        if (stats.total_fills > 0) {
            stats.adverse_fill_rate = static_cast<double>(stats.adverse_fills) / stats.total_fills;
        }
        
        if (stats.adverse_fills > 0) {
            // Calculate weighted average
            double weighted_sum = 0.0;
            for (const auto& fill : fill_history_) {
                if (fill.was_adverse) {
                    weighted_sum += std::abs(fill.adverse_move_bps);
                }
            }
            stats.avg_adverse_move_bps = weighted_sum / stats.adverse_fills;
        }
        
        return stats;
    }
    
private:
    Config config_;
    CircularBuffer<FillEvent> fill_history_;
    TimePoint last_toxic_fill_time_;
    
    // Thread safety
    mutable std::mutex fills_mutex_;
    
    // Caching to avoid expensive recalculations
    std::atomic<double> cached_toxicity_score_;
    std::atomic<double> cached_spread_mult_;
    std::atomic<bool> needs_recalc_;
};

// Enhanced market making with adverse selection protection
class AdverseSelectionAwareMM {
public:
    struct Config {
        double base_spread_bps;
        AdverseSelectionFilter::Config filter_config;
        
        Config() : base_spread_bps(2.0) {}
    };
    
    AdverseSelectionAwareMM(const Config& config)
        : config_(config)
        , filter_(config.filter_config)
    {}
    
    // Calculate quote prices with adverse selection adjustment
    std::pair<double, double> calculate_quotes(double mid_price) {
        double spread_mult = filter_.get_spread_multiplier();
        double adjusted_spread_bps = config_.base_spread_bps * spread_mult;
        
        double half_spread = (adjusted_spread_bps / 10000.0) * mid_price / 2.0;
        
        double bid = mid_price - half_spread;
        double ask = mid_price + half_spread;
        
        return {bid, ask};
    }
    
    // Record fill and update filter
    void on_fill(Side our_side, double price, double quantity) {
        filter_.record_fill(our_side, price, quantity);
    }
    
    // Update with current price
    void on_price_update(double price) {
        filter_.update_current_price(price);
    }
    
    // Get current toxicity
    AdverseSelectionFilter::ToxicityMetrics get_toxicity() {
        return filter_.calculate_toxicity();
    }
    
private:
    Config config_;
    AdverseSelectionFilter filter_;
};

} // namespace trading
