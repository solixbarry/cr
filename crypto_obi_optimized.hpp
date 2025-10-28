#pragma once

#include "../strategies/order_book_imbalance.hpp"
#include <cmath>

namespace trading {

// CRYPTO OBI - JANE STREET OPTIMIZED (PRODUCTION READY)
// Only includes proven optimizations, removes risky/unproven features

class CryptoOBIOptimized {
public:
    // ✅ JANE STREET PRINCIPLE #1: Adaptive volatility-based thresholds
    static OrderBookImbalanceStrategy::Config get_adaptive_config(
        const std::string& symbol,
        double current_volatility_bps)
    {
        OrderBookImbalanceStrategy::Config config;
        
        // Volatility-based parameter adjustment
        if (current_volatility_bps > 150.0) {
            // HIGH VOLATILITY REGIME (BTC moving 1.5%+ in last hour)
            config.imbalance_threshold = 0.25;      // Lower = more signals
            config.target_profit_bps = 8.0;         // Wider targets
            config.stop_loss_bps = 5.0;             // Wider stops
            config.signal_decay_ms = 80;            // Faster decay (vol changes fast)
            
        } else if (current_volatility_bps < 50.0) {
            // LOW VOLATILITY REGIME (BTC range-bound <0.5%)
            config.imbalance_threshold = 0.35;      // Higher = fewer, better signals
            config.target_profit_bps = 3.0;         // Tighter targets
            config.stop_loss_bps = 2.0;             // Tighter stops
            config.signal_decay_ms = 150;           // Slower decay
            
        } else {
            // NORMAL REGIME (0.5-1.5% moves)
            config.imbalance_threshold = 0.30;
            config.target_profit_bps = 5.0;
            config.stop_loss_bps = 3.0;
            config.signal_decay_ms = 100;
        }
        
        // Symbol-specific overrides
        if (symbol == "SOLUSD") {
            config.imbalance_threshold -= 0.03;     // SOL more volatile, be aggressive
            config.target_profit_bps += 1.0;
        } else if (symbol == "BTCUSD" || symbol == "ETHUSD") {
            config.imbalance_threshold += 0.02;     // Major pairs tighter spreads
            config.target_profit_bps -= 0.5;
        }
        
        config.num_levels = 12;                     // Deep analysis
        config.min_volume_threshold = 3.0;
        
        return config;
    }
    
    // ✅ JANE STREET PRINCIPLE #2: Kelly criterion position sizing
    static double calculate_kelly_position_size(
        double win_rate,
        double avg_win_bps,
        double avg_loss_bps,
        double current_capital)
    {
        // Kelly formula: f* = (p × b - q) / b
        // where p = win rate, b = profit/loss ratio, q = 1-p
        
        if (avg_loss_bps == 0.0) return 0.0;  // Prevent division by zero
        
        double profit_loss_ratio = avg_win_bps / avg_loss_bps;
        double kelly_fraction = (win_rate * profit_loss_ratio - (1.0 - win_rate)) / profit_loss_ratio;
        
        // Safety constraints:
        // 1. Half-Kelly (more conservative)
        double half_kelly = kelly_fraction * 0.5;
        
        // 2. Never risk more than 5% per trade
        double max_risk_pct = 0.05;
        double position_fraction = std::min(half_kelly, max_risk_pct);
        
        // 3. Positive Kelly only (never bet if edge is negative)
        position_fraction = std::max(0.0, position_fraction);
        
        // Convert to dollar position size
        return current_capital * position_fraction;
    }
    
    // ✅ JANE STREET PRINCIPLE #3: Dynamic sizing based on recent performance
    static double get_performance_adjusted_size(
        double base_position_size,
        double recent_win_rate,      // Last 50 trades
        double recent_profit_factor)  // Total wins / Total losses
    {
        // If strategy performing well recently, increase size
        // If performing poorly, decrease size
        
        double performance_multiplier = 1.0;
        
        if (recent_win_rate > 0.60 && recent_profit_factor > 1.5) {
            performance_multiplier = 1.3;  // +30% size when hot
        } else if (recent_win_rate < 0.50 || recent_profit_factor < 1.0) {
            performance_multiplier = 0.7;  // -30% size when cold
        }
        
        return base_position_size * performance_multiplier;
    }
    
    // Calculate current volatility (helper function)
    static double calculate_volatility_bps(
        const std::vector<double>& recent_prices,
        int lookback_minutes = 60)
    {
        if (recent_prices.size() < 2) return 0.0;
        
        // Calculate returns
        std::vector<double> returns;
        for (size_t i = 1; i < recent_prices.size(); ++i) {
            double ret = (recent_prices[i] - recent_prices[i-1]) / recent_prices[i-1];
            returns.push_back(ret);
        }
        
        // Calculate standard deviation
        double mean = 0.0;
        for (double ret : returns) mean += ret;
        mean /= returns.size();
        
        double variance = 0.0;
        for (double ret : returns) {
            variance += (ret - mean) * (ret - mean);
        }
        variance /= returns.size();
        
        double std_dev = std::sqrt(variance);
        
        // Annualize and convert to bps
        // sqrt(525600 minutes/year) × std_dev × 10000 bps
        double annualized_vol_bps = std_dev * std::sqrt(525600.0 / lookback_minutes) * 10000.0;
        
        return annualized_vol_bps;
    }
};

// USAGE EXAMPLE:
//
// // Calculate current volatility
// std::vector<double> last_60_prices = get_recent_prices("BTCUSD", 60);
// double vol_bps = CryptoOBIOptimized::calculate_volatility_bps(last_60_prices);
//
// // Get adaptive config
// auto config = CryptoOBIOptimized::get_adaptive_config("BTCUSD", vol_bps);
// OrderBookImbalanceStrategy obi_strategy(config);
//
// // Calculate Kelly position size
// double win_rate = 0.58;        // From historical stats
// double avg_win = 5.2;          // Average win in bps
// double avg_loss = 3.1;         // Average loss in bps
// double capital = 19000.0;      // Your capital
//
// double kelly_size = CryptoOBIOptimized::calculate_kelly_position_size(
//     win_rate, avg_win, avg_loss, capital
// );
//
// // Adjust for recent performance
// double recent_win_rate = 0.62;  // Last 50 trades
// double recent_pf = 1.7;         // Profit factor
// double adjusted_size = CryptoOBIOptimized::get_performance_adjusted_size(
//     kelly_size, recent_win_rate, recent_pf
// );

} // namespace trading
