#include "core/types.hpp"
#include "core/risk_manager.hpp"
#include "core/order_tracker.hpp"
#include "core/circuit_breaker.hpp"
#include "core/memory_pool.hpp"
#include "core/string_interning.hpp"
#include "strategies/strategy_coordinator.hpp"
#include "market_data/order_book.hpp"

#include <iostream>
#include <thread>
#include <chrono>

using namespace trading;

int main(int argc, char** argv) {
    std::cout << "Trading Engine v3.1 OPTIMIZED\n";
    std::cout << "==============================\n\n";
    
    // Parse command line
    bool paper_mode = false;
    bool all_strategies = false;
    double capital = 10000.0;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--paper") {
            paper_mode = true;
        } else if (arg == "--all-strategies") {
            all_strategies = true;
        } else if (arg == "--capital" && i + 1 < argc) {
            capital = std::stod(argv[++i]);
        }
    }
    
    std::cout << "Mode: " << (paper_mode ? "PAPER" : "LIVE") << "\n";
    std::cout << "Capital: $" << capital << "\n";
    std::cout << "Strategies: " << (all_strategies ? "ALL" : "DEFAULT") << "\n\n";
    
    // Initialize components
    std::cout << "Initializing system...\n";
    
    // 1. Register common symbols (string interning optimization)
    register_common_symbols();
    std::cout << "  ✓ Registered " << SymbolRegistry::instance().count() << " symbols\n";
    
    // 2. Initialize risk manager
    RiskManager::Config risk_config;
    risk_config.max_position_size = 100000.0;
    risk_config.max_total_exposure = capital;
    risk_config.max_loss_per_day = capital * 0.05;  // 5% max daily loss
    
    RiskManager risk_manager(risk_config);
    std::cout << "  ✓ Risk manager initialized (max loss: $" << risk_config.max_loss_per_day << "/day)\n";
    
    // 3. Initialize order tracker
    OrderTracker order_tracker;
    std::cout << "  ✓ Order tracker initialized\n";
    
    // 4. Initialize strategy coordinator
    StrategyCoordinator::Config strategy_config;
    strategy_config.enable_obi = all_strategies;
    strategy_config.enable_latency_arb = all_strategies;
    strategy_config.enable_pairs = all_strategies;
    strategy_config.enable_adverse_filter = all_strategies;
    strategy_config.enable_vol_arb = all_strategies;
    
    StrategyCoordinator coordinator(strategy_config, risk_manager);
    std::cout << "  ✓ Strategy coordinator initialized\n";
    
    std::cout << "\nSystem ready!\n\n";
    
    // Example: Process market data
    std::cout << "Processing market data...\n";
    
    // Create sample order book
    OrderBook btc_book;
    btc_book.update_bid(50000.0, 10.0);
    btc_book.update_bid(49995.0, 5.0);
    btc_book.update_ask(50005.0, 8.0);
    btc_book.update_ask(50010.0, 12.0);
    
    std::cout << "BTC Order Book:\n";
    std::cout << "  Best Bid: $" << btc_book.get_best_bid() << "\n";
    std::cout << "  Best Ask: $" << btc_book.get_best_ask() << "\n";
    std::cout << "  Mid Price: $" << btc_book.get_mid_price() << "\n";
    std::cout << "  Spread: $" << btc_book.get_spread() << "\n\n";
    
    // Generate signals
    std::unordered_map<Venue, OrderBook> all_books;
    all_books[Venue::BINANCE] = btc_book;
    
    std::unordered_map<std::string, double> current_prices;
    current_prices["BTCUSDT"] = btc_book.get_mid_price();
    current_prices["ETHUSDT"] = 3000.0;
    
    auto orders = coordinator.process_market_update(
        "BTCUSDT",
        btc_book,
        all_books,
        current_prices
    );
    
    std::cout << "Generated " << orders.size() << " signals\n";
    
    // Example: Print performance stats
    std::cout << "\nPerformance Statistics:\n";
    std::cout << "======================\n";
    coordinator.print_performance_report();
    
    // Memory pool stats
    auto pool_stats = get_pool_stats();
    std::cout << "\nMemory Pool Stats:\n";
    std::cout << "  Orders in use: " << pool_stats.orders_in_use << "\n";
    std::cout << "  Fills in use: " << pool_stats.fills_in_use << "\n";
    
    std::cout << "\nSystem shutdown complete.\n";
    
    return 0;
}
