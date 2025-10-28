#pragma once

#include "../core/types.hpp"
#include "../market_data/order_book.hpp"
#include <unordered_map>
#include <optional>

namespace trading {

// Latency Arbitrage - Exploits price differences across exchanges
class LatencyArbitrageStrategy {
public:
    struct Config {
        std::vector<Venue> venues;          // Exchanges to monitor
        double min_profit_bps;              // Min profit to execute (8-20 bps)
        double max_execution_latency_us;    // Max execution time (100-500μs)
        double position_size_usd;           // Size per arb
        int max_concurrent_arbs;            // Max simultaneous arbitrages
        double fee_bps;                     // Total fees (both sides)
        
        Config()
            : venues({Venue::BINANCE, Venue::BYBIT, Venue::COINBASE})
            , min_profit_bps(12.0)          // 12 bps minimum
            , max_execution_latency_us(200.0)
            , position_size_usd(5000.0)
            , max_concurrent_arbs(3)
            , fee_bps(4.0)                  // 2 bps each side (taker)
        {}
    };
    
    struct ArbitrageOpportunity {
        std::string symbol;
        
        // Buy side
        Venue buy_venue;
        double buy_price;
        double buy_quantity_available;
        
        // Sell side  
        Venue sell_venue;
        double sell_price;
        double sell_quantity_available;
        
        // Profitability
        double gross_profit_bps;
        double net_profit_bps;              // After fees
        double expected_profit_usd;
        
        // Timing
        TimePoint detected_at;
        int64_t detection_latency_us;       // How long to detect
        
        // Execution
        double execute_quantity;            // How much to trade
        bool is_valid;
        
        ArbitrageOpportunity()
            : buy_venue(Venue::UNKNOWN)
            , sell_venue(Venue::UNKNOWN)
            , buy_price(0.0)
            , sell_price(0.0)
            , buy_quantity_available(0.0)
            , sell_quantity_available(0.0)
            , gross_profit_bps(0.0)
            , net_profit_bps(0.0)
            , expected_profit_usd(0.0)
            , detection_latency_us(0)
            , execute_quantity(0.0)
            , is_valid(false)
        {}
    };
    
    explicit LatencyArbitrageStrategy(const Config& config)
        : config_(config)
        , active_arbs_(0)
    {
        // Precompute all venue pairs once
        for (size_t i = 0; i < config_.venues.size(); ++i) {
            for (size_t j = i + 1; j < config_.venues.size(); ++j) {
                venue_pairs_.emplace_back(config_.venues[i], config_.venues[j]);
            }
        }
    }
    
    // Detect arbitrage opportunities across venues
    std::optional<ArbitrageOpportunity> detect_opportunity(
        const std::string& symbol,
        const std::unordered_map<Venue, OrderBook>& books)
    {
        auto start = Clock::now();
        
        // Check if we can take more arbs
        if (active_arbs_.load(std::memory_order_relaxed) >= config_.max_concurrent_arbs) {
            return std::nullopt;
        }
        
        ArbitrageOpportunity best_opp;
        best_opp.symbol = symbol;
        
        // Use precomputed venue pairs - O(n) not O(n²)
        for (const auto& pair : venue_pairs_) {
            Venue venue1 = pair.first;
            Venue venue2 = pair.second;
            
            auto book1_it = books.find(venue1);
            auto book2_it = books.find(venue2);
            
            if (book1_it == books.end() || book2_it == books.end()) {
                continue;
            }
            
            const auto& book1 = book1_it->second;
            const auto& book2 = book2_it->second;
            
            // Check both directions
            check_arb_direction(symbol, venue1, book1, venue2, book2, best_opp);
            check_arb_direction(symbol, venue2, book2, venue1, book1, best_opp);
        }
        
        auto end = Clock::now();
        best_opp.detection_latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
            end - start
        ).count();
        
        // Validate opportunity
        if (best_opp.net_profit_bps >= config_.min_profit_bps &&
            best_opp.detection_latency_us < config_.max_execution_latency_us) {
            
            best_opp.detected_at = end;
            best_opp.is_valid = true;
            return best_opp;
        }
        
        return std::nullopt;
    }
    
    // Create orders for arbitrage execution
    std::pair<Order, Order> create_arb_orders(const ArbitrageOpportunity& opp) {
        // Buy order (cheap venue)
        Order buy_order;
        buy_order.symbol = opp.symbol;
        buy_order.venue = opp.buy_venue;
        buy_order.side = Side::BUY;
        buy_order.type = OrderType::LIMIT_IOC;  // Immediate or cancel
        buy_order.price = opp.buy_price;
        buy_order.quantity = opp.execute_quantity;
        buy_order.strategy_name = "LATENCY_ARB";
        buy_order.created_time = Clock::now();
        
        // Sell order (expensive venue)
        Order sell_order;
        sell_order.symbol = opp.symbol;
        sell_order.venue = opp.sell_venue;
        sell_order.side = Side::SELL;
        sell_order.type = OrderType::LIMIT_IOC;
        sell_order.price = opp.sell_price;
        sell_order.quantity = opp.execute_quantity;
        sell_order.strategy_name = "LATENCY_ARB";
        sell_order.created_time = Clock::now();
        
        // Track active arb
        active_arbs_.fetch_add(1, std::memory_order_relaxed);
        
        return {buy_order, sell_order};
    }
    
    // Mark arb as completed
    void complete_arbitrage() {
        active_arbs_.fetch_sub(1, std::memory_order_relaxed);
    }
    
    // Statistics
    struct ArbStats {
        int total_opportunities;
        int executed_arbs;
        int successful_arbs;
        int failed_arbs;
        double total_profit;
        double avg_profit_bps;
        double win_rate;
        int64_t avg_execution_time_us;
        
        ArbStats()
            : total_opportunities(0)
            , executed_arbs(0)
            , successful_arbs(0)
            , failed_arbs(0)
            , total_profit(0.0)
            , avg_profit_bps(0.0)
            , win_rate(0.0)
            , avg_execution_time_us(0)
        {}
    };
    
    void record_arb_result(const ArbitrageOpportunity& opp, bool success, 
                          double actual_profit, int64_t execution_time_us) {
        stats_.total_opportunities++;
        stats_.executed_arbs++;
        
        if (success) {
            stats_.successful_arbs++;
            stats_.total_profit += actual_profit;
            
            // Update rolling average
            stats_.avg_profit_bps = (stats_.avg_profit_bps * (stats_.successful_arbs - 1) +
                                    opp.net_profit_bps) / stats_.successful_arbs;
            
            stats_.avg_execution_time_us = (stats_.avg_execution_time_us * (stats_.successful_arbs - 1) +
                                           execution_time_us) / stats_.successful_arbs;
        } else {
            stats_.failed_arbs++;
        }
        
        if (stats_.executed_arbs > 0) {
            stats_.win_rate = static_cast<double>(stats_.successful_arbs) / stats_.executed_arbs;
        }
    }
    
    const ArbStats& get_stats() const {
        return stats_;
    }
    
    int get_active_arbs() const {
        return active_arbs_.load(std::memory_order_relaxed);
    }
    
private:
    Config config_;
    std::atomic<int> active_arbs_;
    ArbStats stats_;
    
    // Precomputed venue pairs for efficient iteration
    std::vector<std::pair<Venue, Venue>> venue_pairs_;
    
    // Check arbitrage in one direction
    void check_arb_direction(
        const std::string& symbol,
        Venue buy_venue, const OrderBook& buy_book,
        Venue sell_venue, const OrderBook& sell_book,
        ArbitrageOpportunity& best_opp)
    {
        // Get best bid/ask from each venue
        double buy_ask = buy_book.get_best_ask();   // Buy here (cross the spread)
        double sell_bid = sell_book.get_best_bid(); // Sell here (cross the spread)
        
        if (buy_ask <= 0.0 || sell_bid <= 0.0) {
            return;
        }
        
        // Calculate profit
        double gross_profit_bps = ((sell_bid - buy_ask) / buy_ask) * 10000.0;
        double net_profit_bps = gross_profit_bps - config_.fee_bps;
        
        if (net_profit_bps <= best_opp.net_profit_bps) {
            return;  // Not better than current best
        }
        
        // Get available quantities
        const auto& buy_asks = buy_book.get_asks();
        const auto& sell_bids = sell_book.get_bids();
        
        if (buy_asks.empty() || sell_bids.empty()) {
            return;
        }
        
        double buy_qty = buy_asks.begin()->second;
        double sell_qty = sell_bids.begin()->second;
        
        // Execute quantity is minimum of both sides
        double max_qty = std::min(buy_qty, sell_qty);
        double max_notional = max_qty * buy_ask;
        
        // Size the trade
        double target_notional = std::min(config_.position_size_usd, max_notional);
        double execute_qty = target_notional / buy_ask;
        
        // Update best opportunity
        best_opp.buy_venue = buy_venue;
        best_opp.sell_venue = sell_venue;
        best_opp.buy_price = buy_ask;
        best_opp.sell_price = sell_bid;
        best_opp.buy_quantity_available = buy_qty;
        best_opp.sell_quantity_available = sell_qty;
        best_opp.gross_profit_bps = gross_profit_bps;
        best_opp.net_profit_bps = net_profit_bps;
        best_opp.execute_quantity = execute_qty;
        best_opp.expected_profit_usd = (net_profit_bps / 10000.0) * target_notional;
    }
};

// Triangular arbitrage (e.g., BTC → ETH → SOL → BTC)
class TriangularArbitrageStrategy {
public:
    struct Config {
        std::vector<std::string> triangle;  // e.g., ["BTC", "ETH", "SOL"]
        double min_profit_bps;
        double max_slippage_bps;
        
        Config()
            : triangle({"BTC", "ETH", "SOL"})
            , min_profit_bps(15.0)
            , max_slippage_bps(5.0)
        {}
    };
    
    struct TriangularOpportunity {
        std::vector<std::string> symbols;
        std::vector<Side> sides;
        std::vector<double> prices;
        double net_profit_bps;
        bool is_valid;
        
        TriangularOpportunity() 
            : net_profit_bps(0.0), is_valid(false) {}
    };
    
    explicit TriangularArbitrageStrategy(const Config& config) : config_(config) {}
    
    // Detect triangular arbitrage
    std::optional<TriangularOpportunity> detect_opportunity(
        const std::unordered_map<std::string, OrderBook>& books)
    {
        // Implementation for BTC → ETH → SOL → BTC cycle
        // Calculate if (BTC/USD) * (ETH/BTC) * (SOL/ETH) * (USD/SOL) > 1.0
        
        // This requires cross-rate calculations and proper symbol mapping
        // Left as exercise - would need more complex price chain logic
        
        return std::nullopt;
    }
    
private:
    Config config_;
};

} // namespace trading
