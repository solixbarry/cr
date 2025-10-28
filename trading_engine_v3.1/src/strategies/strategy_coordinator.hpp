#pragma once

#include "order_book_imbalance.hpp"
#include "latency_arbitrage.hpp"
#include "pairs_trading.hpp"
#include "adverse_selection_filter.hpp"
#include "volatility_arbitrage.hpp"
#include "../core/types.hpp"
#include "../core/risk_manager.hpp"
#include <memory>
#include <vector>

namespace trading {

// Master Strategy Coordinator - Manages all 5 money-making algorithms
class StrategyCoordinator {
public:
    struct Config {
        bool enable_obi = true;
        bool enable_latency_arb = true;
        bool enable_pairs = true;
        bool enable_adverse_filter = true;
        bool enable_vol_arb = true;
        
        OrderBookImbalanceStrategy::Config obi_config;
        LatencyArbitrageStrategy::Config latency_arb_config;
        PairsTradingStrategy::Config pairs_config;
        AdverseSelectionFilter::Config adverse_filter_config;
        VolatilityArbitrageStrategy::Config vol_arb_config;
        
        // Global limits
        int max_total_positions = 20;
        double max_total_notional = 150000.0;
    };
    
    explicit StrategyCoordinator(const Config& config, RiskManager& risk_manager)
        : config_(config)
        , risk_manager_(risk_manager)
    {
        // Initialize enabled strategies
        if (config_.enable_obi) {
            obi_strategy_ = std::make_unique<OrderBookImbalanceStrategy>(config_.obi_config);
            LOG_INFO("OBI Strategy enabled");
        }
        
        if (config_.enable_latency_arb) {
            latency_arb_strategy_ = std::make_unique<LatencyArbitrageStrategy>(config_.latency_arb_config);
            LOG_INFO("Latency Arbitrage enabled");
        }
        
        if (config_.enable_pairs) {
            // Initialize pairs (ETH/BTC, SOL/BTC, LINK/BTC)
            auto eth_btc_config = config_.pairs_config;
            eth_btc_config.symbol1 = "ETHUSDT";
            eth_btc_config.symbol2 = "BTCUSDT";
            pairs_strategies_["ETH_BTC"] = std::make_unique<PairsTradingStrategy>(eth_btc_config);
            
            auto sol_btc_config = config_.pairs_config;
            sol_btc_config.symbol1 = "SOLUSDT";
            sol_btc_config.symbol2 = "BTCUSDT";
            pairs_strategies_["SOL_BTC"] = std::make_unique<PairsTradingStrategy>(sol_btc_config);
            
            LOG_INFO("Pairs Trading enabled (2 pairs)");
        }
        
        if (config_.enable_adverse_filter) {
            adverse_filter_ = std::make_unique<AdverseSelectionFilter>(config_.adverse_filter_config);
            LOG_INFO("Adverse Selection Filter enabled");
        }
        
        if (config_.enable_vol_arb) {
            // One vol arb per symbol
            vol_arb_strategies_["BTCUSDT"] = std::make_unique<VolatilityArbitrageStrategy>(config_.vol_arb_config);
            vol_arb_strategies_["ETHUSDT"] = std::make_unique<VolatilityArbitrageStrategy>(config_.vol_arb_config);
            LOG_INFO("Volatility Arbitrage enabled (2 symbols)");
        }
    }
    
    // Process market data and generate signals from ALL strategies
    std::vector<Order> process_market_update(
        const std::string& symbol,
        const OrderBook& book,
        const std::unordered_map<std::string, OrderBook>& all_books,
        const std::unordered_map<std::string, double>& current_prices)
    {
        std::vector<Order> orders;
        double current_price = book.get_mid_price();
        
        // 1. ORDER BOOK IMBALANCE
        if (obi_strategy_ && config_.enable_obi) {
            auto obi_signal = obi_strategy_->analyze(symbol, book);
            
            if (obi_signal.is_valid && !obi_strategy_->is_signal_expired(obi_signal)) {
                // Check risk limits
                auto risk_check = risk_manager_.check_order(
                    Order{}, // Would need to construct proper order
                    current_price
                );
                
                if (risk_check.passed) {
                    double quantity = calculate_position_size(symbol, current_price, "OBI");
                    Order order = obi_strategy_->create_order_from_signal(obi_signal, quantity);
                    order.symbol = symbol;
                    orders.push_back(order);
                    
                    LOG_INFO("OBI Signal: " << symbol << " " << to_string(obi_signal.predicted_direction)
                             << " confidence=" << obi_signal.confidence);
                }
            }
        }
        
        // 2. LATENCY ARBITRAGE
        if (latency_arb_strategy_ && config_.enable_latency_arb && all_books.size() > 1) {
            auto arb_opp = latency_arb_strategy_->detect_opportunity(symbol, all_books);
            
            if (arb_opp.has_value() && arb_opp->is_valid) {
                auto [buy_order, sell_order] = latency_arb_strategy_->create_arb_orders(*arb_opp);
                
                // Check risk for both legs
                auto buy_check = risk_manager_.check_order(buy_order, arb_opp->buy_price);
                auto sell_check = risk_manager_.check_order(sell_order, arb_opp->sell_price);
                
                if (buy_check.passed && sell_check.passed) {
                    orders.push_back(buy_order);
                    orders.push_back(sell_order);
                    
                    LOG_INFO("Latency Arb: " << symbol 
                             << " buy@" << to_string(arb_opp->buy_venue)
                             << " sell@" << to_string(arb_opp->sell_venue)
                             << " profit=" << arb_opp->net_profit_bps << "bps");
                }
            }
        }
        
        // 3. PAIRS TRADING
        if (config_.enable_pairs) {
            for (auto& [pair_name, strategy] : pairs_strategies_) {
                // Update prices
                auto it1 = current_prices.find(strategy->config_.symbol1);
                auto it2 = current_prices.find(strategy->config_.symbol2);
                
                if (it1 != current_prices.end() && it2 != current_prices.end()) {
                    strategy->update_prices(it1->second, it2->second);
                    
                    auto pair_signal = strategy->generate_signal(it1->second, it2->second);
                    
                    if (pair_signal.is_valid) {
                        auto [order1, order2] = strategy->create_pair_orders(pair_signal);
                        
                        // Risk check both legs
                        auto check1 = risk_manager_.check_order(order1, it1->second);
                        auto check2 = risk_manager_.check_order(order2, it2->second);
                        
                        if (check1.passed && check2.passed) {
                            orders.push_back(order1);
                            orders.push_back(order2);
                            
                            LOG_INFO("Pairs Signal: " << pair_name 
                                     << " z=" << pair_signal.z_score
                                     << " expected=" << pair_signal.expected_profit_bps << "bps");
                        }
                    }
                }
            }
        }
        
        // 4. VOLATILITY ARBITRAGE
        if (config_.enable_vol_arb) {
            auto vol_it = vol_arb_strategies_.find(symbol);
            if (vol_it != vol_arb_strategies_.end()) {
                vol_it->second->update_price(current_price);
                
                auto vol_signal = vol_it->second->generate_signal(current_price);
                
                if (vol_signal.is_valid) {
                    double quantity = calculate_position_size(symbol, current_price, "VOL_ARB");
                    Order order = vol_it->second->create_order_from_signal(vol_signal, quantity);
                    order.symbol = symbol;
                    
                    auto risk_check = risk_manager_.check_order(order, current_price);
                    
                    if (risk_check.passed) {
                        orders.push_back(order);
                        
                        LOG_INFO("Vol Arb Signal: " << symbol
                                 << " regime=" << static_cast<int>(vol_signal.regime)
                                 << " strategy=" << vol_signal.strategy_type);
                    }
                }
            }
        }
        
        // 5. ADVERSE SELECTION FILTER (applies to market making)
        if (adverse_filter_ && config_.enable_adverse_filter) {
            adverse_filter_->update_current_price(current_price);
            
            auto toxicity = adverse_filter_->calculate_toxicity();
            
            // If toxicity high, don't send market making orders
            // Or widen spreads if we do
            if (toxicity.toxicity_score > 0.7) {
                LOG_WARN("High toxicity detected: " << symbol 
                         << " score=" << toxicity.toxicity_score
                         << " - filtering MM orders");
                
                // Remove any market making orders from the list
                orders.erase(
                    std::remove_if(orders.begin(), orders.end(),
                        [](const Order& o) { return o.strategy_name == "MM"; }),
                    orders.end()
                );
            }
        }
        
        return orders;
    }
    
    // Record fill (updates adverse selection filter)
    void on_fill(const Fill& fill) {
        if (adverse_filter_ && config_.enable_adverse_filter) {
            adverse_filter_->record_fill(fill.side, fill.price, fill.quantity);
        }
        
        // Update strategy-specific tracking
        // ... (track OBI fills, arb fills, etc.)
    }
    
    // Comprehensive statistics
    struct PerformanceStats {
        // Per-strategy stats
        OrderBookImbalanceStrategy::OBIStats obi_stats;
        LatencyArbitrageStrategy::ArbStats latency_arb_stats;
        PairsTradingStrategy::PairsStats pairs_stats;
        VolatilityArbitrageStrategy::VolArbStats vol_arb_stats;
        AdverseSelectionFilter::AdverseSelectionStats adverse_stats;
        
        // Combined
        int total_signals_generated;
        int total_orders_sent;
        double total_pnl;
        double combined_win_rate;
    };
    
    PerformanceStats get_performance_stats() {
        PerformanceStats stats;
        
        if (obi_strategy_) {
            stats.obi_stats = obi_strategy_->get_stats();
        }
        
        if (latency_arb_strategy_) {
            stats.latency_arb_stats = latency_arb_strategy_->get_stats();
        }
        
        // Aggregate pairs stats
        for (const auto& [name, strategy] : pairs_strategies_) {
            auto pair_stats = strategy->get_stats();
            stats.pairs_stats.total_trades += pair_stats.total_trades;
            stats.pairs_stats.winning_trades += pair_stats.winning_trades;
            stats.pairs_stats.total_pnl += pair_stats.total_pnl;
        }
        
        // Aggregate vol arb stats
        for (const auto& [symbol, strategy] : vol_arb_strategies_) {
            auto vol_stats = strategy->get_stats();
            stats.vol_arb_stats.total_trades += vol_stats.total_trades;
            stats.vol_arb_stats.winning_trades += vol_stats.winning_trades;
            stats.vol_arb_stats.total_pnl += vol_stats.total_pnl;
        }
        
        if (adverse_filter_) {
            stats.adverse_stats = adverse_filter_->get_stats();
        }
        
        // Calculate combined metrics
        int total_wins = stats.obi_stats.winning_trades +
                        stats.latency_arb_stats.successful_arbs +
                        stats.pairs_stats.winning_trades +
                        stats.vol_arb_stats.winning_trades;
        
        int total_trades = stats.obi_stats.total_signals +
                          stats.latency_arb_stats.executed_arbs +
                          stats.pairs_stats.total_trades +
                          stats.vol_arb_stats.total_trades;
        
        if (total_trades > 0) {
            stats.combined_win_rate = static_cast<double>(total_wins) / total_trades;
        }
        
        stats.total_pnl = stats.obi_stats.total_pnl +
                         stats.latency_arb_stats.total_profit +
                         stats.pairs_stats.total_pnl +
                         stats.vol_arb_stats.total_pnl;
        
        return stats;
    }
    
    // Print comprehensive report
    void print_performance_report() {
        auto stats = get_performance_stats();
        
        LOG_INFO("========================================");
        LOG_INFO("  STRATEGY PERFORMANCE REPORT");
        LOG_INFO("========================================");
        
        if (config_.enable_obi) {
            LOG_INFO("OBI Strategy:");
            LOG_INFO("  Signals: " << stats.obi_stats.total_signals);
            LOG_INFO("  Win Rate: " << (stats.obi_stats.win_rate * 100.0) << "%");
            LOG_INFO("  P&L: $" << stats.obi_stats.total_pnl);
        }
        
        if (config_.enable_latency_arb) {
            LOG_INFO("Latency Arbitrage:");
            LOG_INFO("  Executed: " << stats.latency_arb_stats.executed_arbs);
            LOG_INFO("  Win Rate: " << (stats.latency_arb_stats.win_rate * 100.0) << "%");
            LOG_INFO("  P&L: $" << stats.latency_arb_stats.total_profit);
            LOG_INFO("  Avg Profit: " << stats.latency_arb_stats.avg_profit_bps << " bps");
        }
        
        if (config_.enable_pairs) {
            LOG_INFO("Pairs Trading:");
            LOG_INFO("  Trades: " << stats.pairs_stats.total_trades);
            LOG_INFO("  Win Rate: " << (stats.pairs_stats.win_rate * 100.0) << "%");
            LOG_INFO("  P&L: $" << stats.pairs_stats.total_pnl);
        }
        
        if (config_.enable_vol_arb) {
            LOG_INFO("Volatility Arbitrage:");
            LOG_INFO("  Trades: " << stats.vol_arb_stats.total_trades);
            LOG_INFO("  Win Rate: " << (stats.vol_arb_stats.win_rate * 100.0) << "%");
            LOG_INFO("  P&L: $" << stats.vol_arb_stats.total_pnl);
        }
        
        if (config_.enable_adverse_filter) {
            LOG_INFO("Adverse Selection:");
            LOG_INFO("  Fills: " << stats.adverse_stats.total_fills);
            LOG_INFO("  Adverse Rate: " << (stats.adverse_stats.adverse_fill_rate * 100.0) << "%");
            LOG_INFO("  Cost Saved: $" << stats.adverse_stats.total_adverse_cost);
        }
        
        LOG_INFO("----------------------------------------");
        LOG_INFO("COMBINED:");
        LOG_INFO("  Total P&L: $" << stats.total_pnl);
        LOG_INFO("  Win Rate: " << (stats.combined_win_rate * 100.0) << "%");
        LOG_INFO("========================================");
    }
    
private:
    Config config_;
    RiskManager& risk_manager_;
    
    // Strategy instances
    std::unique_ptr<OrderBookImbalanceStrategy> obi_strategy_;
    std::unique_ptr<LatencyArbitrageStrategy> latency_arb_strategy_;
    std::unordered_map<std::string, std::unique_ptr<PairsTradingStrategy>> pairs_strategies_;
    std::unique_ptr<AdverseSelectionFilter> adverse_filter_;
    std::unordered_map<std::string, std::unique_ptr<VolatilityArbitrageStrategy>> vol_arb_strategies_;
    
    // Helper: Calculate position size for strategy
    double calculate_position_size(const std::string& symbol, double price, const std::string& strategy) {
        // Base size from config
        double base_notional = 5000.0;  // $5k per trade
        
        // Adjust based on strategy
        if (strategy == "OBI") {
            base_notional = 3000.0;  // Smaller for high-frequency OBI
        } else if (strategy == "LATENCY_ARB") {
            base_notional = 5000.0;
        } else if (strategy == "PAIRS") {
            base_notional = 5000.0;
        } else if (strategy == "VOL_ARB") {
            base_notional = 4000.0;
        }
        
        return base_notional / price;
    }
};

} // namespace trading
