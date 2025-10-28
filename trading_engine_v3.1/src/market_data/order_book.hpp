#pragma once

#include "../core/types.hpp"
#include <map>
#include <vector>

namespace trading {

// Price level in order book
struct Level {
    double price;
    double quantity;
    
    Level() : price(0.0), quantity(0.0) {}
    Level(double p, double q) : price(p), quantity(q) {}
};

// Order book representation
class OrderBook {
public:
    OrderBook() = default;
    
    // Get bid/ask spreads
    const std::map<double, double, std::greater<double>>& get_bids() const {
        return bids_;
    }
    
    const std::map<double, double>& get_asks() const {
        return asks_;
    }
    
    // Get best prices
    double get_best_bid() const {
        return bids_.empty() ? 0.0 : bids_.begin()->first;
    }
    
    double get_best_ask() const {
        return asks_.empty() ? 0.0 : asks_.begin()->first;
    }
    
    double get_mid_price() const {
        double bid = get_best_bid();
        double ask = get_best_ask();
        if (bid == 0.0 || ask == 0.0) return 0.0;
        return (bid + ask) / 2.0;
    }
    
    double get_spread() const {
        return get_best_ask() - get_best_bid();
    }
    
    // Update order book
    void update_bid(double price, double quantity) {
        if (quantity > 0.0) {
            bids_[price] = quantity;
        } else {
            bids_.erase(price);
        }
    }
    
    void update_ask(double price, double quantity) {
        if (quantity > 0.0) {
            asks_[price] = quantity;
        } else {
            asks_.erase(price);
        }
    }
    
    void clear() {
        bids_.clear();
        asks_.clear();
    }
    
    // Get book depth
    size_t bid_depth() const { return bids_.size(); }
    size_t ask_depth() const { return asks_.size(); }
    
private:
    std::map<double, double, std::greater<double>> bids_;  // Sorted descending
    std::map<double, double> asks_;                         // Sorted ascending
};

} // namespace trading
