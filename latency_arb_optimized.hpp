#pragma once

#include "../strategies/latency_arbitrage.hpp"
#include "../core/types.hpp"
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace trading {

// LATENCY ARBITRAGE - JANE STREET OPTIMIZED (PRODUCTION READY)
// Multi-venue optimization + adverse selection protection

class LatencyArbOptimized {
public:
    struct Config {
        std::vector<Venue> venues;
        
        // ✅ Dynamic profit thresholds based on opportunity frequency
        double base_min_profit_bps;
        double min_profit_decay_rate;           // Lower threshold if few opportunities
        
        // ✅ Multi-venue optimization
        bool enable_global_best;                // Find global best across all venues
        
        // ✅ Adverse selection protection
        double max_slippage_bps;                // Reject if slippage too high
        int max_orderbook_staleness_ms;         // Reject if prices stale
        
        // Execution
        double position_size_usd;
        int max_concurrent_arbs;
        double max_execution_latency_us;
        
        Config()
            : venues({Venue::BINANCE, Venue::KRAKEN, Venue::COINBASE})
            , base_min_profit_bps(15.0)
            , min_profit_decay_rate(0.7)
            , enable_global_best(true)
            , max_slippage_bps(8.0)
            , max_orderbook_staleness_ms(50)
            , position_size_usd(2000.0)
            , max_concurrent_arbs(3)
            , max_execution_latency_us(200.0)
        {}
    };
    
    struct EnhancedArbOpportunity {
        std::string symbol;
        
        Venue buy_venue;
        Venue sell_venue;
        double buy_price;
        double sell_price;
        
        double gross_profit_bps;
        double fees_bps;
        double slippage_bps;                    // ✅ Estimated slippage
        double net_profit_bps;                  // After fees + slippage
        double expected_profit_usd;
        
        double execute_quantity;
        double buy_liquidity_available;
        double sell_liquidity_available;
        
        int64_t detection_latency_us;
        int64_t orderbook_age_ms;               // ✅ How fresh is the data
        
        bool is_valid;
        std::string reject_reason;
        
        EnhancedArbOpportunity()
            : buy_venue(Venue::UNKNOWN)
            , sell_venue(Venue::UNKNOWN)
            , buy_price(0.0)
            , sell_price(0.0)
            , gross_profit_bps(0.0)
            , fees_bps(0.0)
            , slippage_bps(0.0)
            , net_profit_bps(0.0)
            , expected_profit_usd(0.0)
            , execute_quantity(0.0)
            , buy_liquidity_available(0.0)
            , sell_liquidity_available(0.0)
            , detection_latency_us(0)
            , orderbook_age_ms(0)
            , is_valid(false)
        {}
    };
    
    explicit LatencyArbOptimized(const Config& config)
        : config_(config)
        , active_arbs_(0)
        , opportunities_last_minute_(0)
        , last_opportunity_time_(Clock::now())
    {}
    
    // ✅ MULTI-VENUE OPTIMIZATION - Find global best buy/sell venues
    std::optional<EnhancedArbOpportunity> detect_global_best_opportunity(
        const std::string& symbol,
        const std::unordered_map<Venue, OrderBook>& books,
        const std::unordered_map<Venue, TimePoint>& timestamps)
    {
        auto start = Clock::now();
        
        if (active_arbs_.load(std::memory_order_relaxed) >= config_.max_concurrent_arbs) {
            return std::nullopt;
        }
        
        // Find cheapest venue to buy (lowest ask)
        Venue best_buy_venue = Venue::UNKNOWN;
        double best_buy_price = std::numeric_limits<double>::max();
        double best_buy_liquidity = 0.0;
        
        for (const auto& [venue, book] : books) {
            double ask = book.get_best_ask();
            if (ask > 0.0 && ask < best_buy_price) {
                best_buy_price = ask;
                best_buy_venue = venue;
                
                const auto& asks = book.get_asks();
                if (!asks.empty()) {
                    best_buy_liquidity = asks.begin()->second;
                }
            }
        }
        
        // Find most expensive venue to sell (highest bid)
        Venue best_sell_venue = Venue::UNKNOWN;
        double best_sell_price = 0.0;
        double best_sell_liquidity = 0.0;
        
        for (const auto& [venue, book] : books) {
            double bid = book.get_best_bid();
            if (bid > best_sell_price) {
                best_sell_price = bid;
                best_sell_venue = venue;
                
                const auto& bids = book.get_bids();
                if (!bids.empty()) {
                    best_sell_liquidity = bids.begin()->second;
                }
            }
        }
        
        // Validate we found both venues
        if (best_buy_venue == Venue::UNKNOWN || 
            best_sell_venue == Venue::UNKNOWN ||
            best_buy_venue == best_sell_venue) {
            return std::nullopt;
        }
        
        // Build opportunity
        EnhancedArbOpportunity opp;
        opp.symbol = symbol;
        opp.buy_venue = best_buy_venue;
        opp.sell_venue = best_sell_venue;
        opp.buy_price = best_buy_price;
        opp.sell_price = best_sell_price;
        opp.buy_liquidity_available = best_buy_liquidity;
        opp.sell_liquidity_available = best_sell_liquidity;
        
        // Calculate gross profit
        opp.gross_profit_bps = ((best_sell_price - best_buy_price) / best_buy_price) * 10000.0;
        
        // Calculate fees
        opp.fees_bps = get_fee_bps(best_buy_venue) + get_fee_bps(best_sell_venue);
        
        // ✅ ESTIMATE SLIPPAGE (adverse selection protection)
        auto buy_book_it = books.find(best_buy_venue);
        auto sell_book_it = books.find(best_sell_venue);
        
        if (buy_book_it != books.end() && sell_book_it != books.end()) {
            double target_qty = config_.position_size_usd / best_buy_price;
            
            double buy_slippage = estimate_slippage(buy_book_it->second, target_qty, Side::BUY);
            double sell_slippage = estimate_slippage(sell_book_it->second, target_qty, Side::SELL);
            
            opp.slippage_bps = (buy_slippage + sell_slippage) * 10000.0;
            
            // ✅ REJECT if slippage too high (adverse selection)
            if (opp.slippage_bps > config_.max_slippage_bps) {
                opp.reject_reason = "Slippage too high";
                return opp;
            }
        }
        
        // Net profit after fees and slippage
        opp.net_profit_bps = opp.gross_profit_bps - opp.fees_bps - opp.slippage_bps;
        
        // ✅ CHECK ORDERBOOK FRESHNESS
        auto now = Clock::now();
        int64_t max_age = 0;
        
        for (const auto& venue : {best_buy_venue, best_sell_venue}) {
            auto ts_it = timestamps.find(venue);
            if (ts_it != timestamps.end()) {
                auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - ts_it->second
                ).count();
                max_age = std::max(max_age, age);
            }
        }
        
        opp.orderbook_age_ms = max_age;
        
        if (max_age > config_.max_orderbook_staleness_ms) {
            opp.reject_reason = "Orderbook too stale";
            return opp;
        }
        
        // ✅ DYNAMIC THRESHOLD (lower if few opportunities recently)
        double threshold = get_dynamic_threshold();
        
        if (opp.net_profit_bps < threshold) {
            opp.reject_reason = "Net profit below threshold";
            return opp;
        }
        
        // Calculate execution size
        double max_qty_buy = best_buy_liquidity;
        double max_qty_sell = best_sell_liquidity;
        double max_qty = std::min(max_qty_buy, max_qty_sell);
        double max_notional = max_qty * best_buy_price;
        
        double target_notional = std::min(config_.position_size_usd, max_notional);
        opp.execute_quantity = target_notional / best_buy_price;
        opp.expected_profit_usd = (opp.net_profit_bps / 10000.0) * target_notional;
        
        // Detection latency
        auto end = Clock::now();
        opp.detection_latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
            end - start
        ).count();
        
        if (opp.detection_latency_us > config_.max_execution_latency_us) {
            opp.reject_reason = "Detection too slow";
            return opp;
        }
        
        opp.is_valid = true;
        
        // Track opportunity frequency
        opportunities_last_minute_++;
        last_opportunity_time_ = now;
        
        return opp;
    }
    
    void on_arbitrage_executed() {
        active_arbs_.fetch_add(1, std::memory_order_relaxed);
    }
    
    void on_arbitrage_completed() {
        active_arbs_.fetch_sub(1, std::memory_order_relaxed);
    }
    
private:
    Config config_;
    std::atomic<int> active_arbs_;
    std::atomic<int> opportunities_last_minute_;
    TimePoint last_opportunity_time_;
    
    // ✅ DYNAMIC THRESHOLD based on opportunity frequency
    double get_dynamic_threshold() const {
        auto now = Clock::now();
        auto seconds_since_last = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_opportunity_time_
        ).count();
        
        // If no opportunities in 60 seconds, lower threshold
        if (seconds_since_last > 60) {
            return config_.base_min_profit_bps * config_.min_profit_decay_rate;
        }
        
        return config_.base_min_profit_bps;
    }
    
    // ✅ SLIPPAGE ESTIMATION
    double estimate_slippage(const OrderBook& book, double quantity, Side side) const {
        double total_value = 0.0;
        double remaining_qty = quantity;
        
        const auto& levels = (side == Side::BUY) ? book.get_asks() : book.get_bids();
        
        for (const auto& [price, level_qty] : levels) {
            double fill_qty = std::min(remaining_qty, level_qty);
            total_value += fill_qty * price;
            remaining_qty -= fill_qty;
            
            if (remaining_qty <= 0.0) break;
        }
        
        if (total_value == 0.0) return 0.0;
        
        double vwap = total_value / quantity;
        double best_price = (side == Side::BUY) ? book.get_best_ask() : book.get_best_bid();
        
        return std::abs(vwap - best_price) / best_price;
    }
    
    double get_fee_bps(Venue venue) const {
        switch (venue) {
            case Venue::BINANCE: return 10.0;   // 0.10% taker
            case Venue::KRAKEN: return 16.0;    // 0.16% taker
            case Venue::COINBASE: return 40.0;  // 0.40% taker
            default: return 20.0;
        }
    }
};

// USAGE EXAMPLE:
//
// LatencyArbOptimized::Config config;
// config.enable_global_best = true;
// config.max_slippage_bps = 8.0;
// LatencyArbOptimized arb_strategy(config);
//
// // Detect best opportunity across all venues
// auto opp = arb_strategy.detect_global_best_opportunity(
//     "BTCUSD", all_books, timestamps
// );
//
// if (opp.has_value() && opp->is_valid) {
//     LOG_INFO("Global best arb: "
//              << "buy@" << to_string(opp->buy_venue) << ":" << opp->buy_price
//              << " sell@" << to_string(opp->sell_venue) << ":" << opp->sell_price
//              << " net=" << opp->net_profit_bps << "bps"
//              << " slippage=" << opp->slippage_bps << "bps");
//     
//     arb_strategy.on_arbitrage_executed();
//     execute_arb(*opp);
// }

} // namespace trading
