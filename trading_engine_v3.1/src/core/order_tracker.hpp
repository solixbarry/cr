#pragma once

#include "types.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <vector>

namespace trading {

// Order tracking with proper symbol mapping
class OrderTracker {
public:
    OrderTracker() = default;
    
    // Track new order with automatic cleanup
    void track_order(const Order& order) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        // Auto-cleanup if too many orders
        if (orders_.size() >= MAX_ORDERS) {
            cleanup_oldest_internal(1000);
        }
        
        orders_[order.client_order_id] = order;
        
        // Build indices for fast lookup
        order_id_to_client_id_[order.order_id] = order.client_order_id;
        symbol_orders_[order.symbol].push_back(order.client_order_id);
        
        if (order.is_active()) {
            active_orders_.insert(order.client_order_id);
        }
    }
    
    // Update order status
    void update_order(const std::string& client_order_id, const Order& updated) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        auto it = orders_.find(client_order_id);
        if (it == orders_.end()) {
            return;
        }
        
        // Update active set
        if (it->second.is_active() && !updated.is_active()) {
            active_orders_.erase(client_order_id);
        } else if (!it->second.is_active() && updated.is_active()) {
            active_orders_.insert(client_order_id);
        }
        
        it->second = updated;
    }
    
    // CRITICAL: Get symbol for order_id (for fills)
    std::optional<std::string> get_symbol(const std::string& order_id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        // First try order_id (exchange ID)
        auto client_it = order_id_to_client_id_.find(order_id);
        if (client_it != order_id_to_client_id_.end()) {
            auto order_it = orders_.find(client_it->second);
            if (order_it != orders_.end()) {
                return order_it->second.symbol;
            }
        }
        
        // Fallback: try as client_order_id
        auto order_it = orders_.find(order_id);
        if (order_it != orders_.end()) {
            return order_it->second.symbol;
        }
        
        return std::nullopt;
    }
    
    // Get order details
    std::optional<Order> get_order(const std::string& client_order_id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        auto it = orders_.find(client_order_id);
        if (it != orders_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    
    // Get order by exchange ID
    std::optional<Order> get_order_by_exchange_id(const std::string& order_id) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        auto client_it = order_id_to_client_id_.find(order_id);
        if (client_it != order_id_to_client_id_.end()) {
            return get_order(client_it->second);
        }
        return std::nullopt;
    }
    
    // Get all active orders
    std::vector<Order> get_active_orders() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        std::vector<Order> result;
        result.reserve(active_orders_.size());
        
        for (const auto& client_id : active_orders_) {
            auto it = orders_.find(client_id);
            if (it != orders_.end()) {
                result.push_back(it->second);
            }
        }
        
        return result;
    }
    
    // Get all orders for symbol
    std::vector<Order> get_orders_for_symbol(const std::string& symbol) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        
        std::vector<Order> result;
        
        auto it = symbol_orders_.find(symbol);
        if (it != symbol_orders_.end()) {
            result.reserve(it->second.size());
            
            for (const auto& client_id : it->second) {
                auto order_it = orders_.find(client_id);
                if (order_it != orders_.end()) {
                    result.push_back(order_it->second);
                }
            }
        }
        
        return result;
    }
    
    // Remove completed orders (cleanup)
    size_t cleanup_completed(std::chrono::seconds retention_period) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        
        auto now = Clock::now();
        size_t removed = 0;
        
        auto it = orders_.begin();
        while (it != orders_.end()) {
            const auto& order = it->second;
            
            if (order.is_complete()) {
                auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    now - order.completed_time
                );
                
                if (age > retention_period) {
                    // Remove from indices
                    order_id_to_client_id_.erase(order.order_id);
                    active_orders_.erase(order.client_order_id);
                    
                    // Remove from symbol index
                    auto sym_it = symbol_orders_.find(order.symbol);
                    if (sym_it != symbol_orders_.end()) {
                        auto& vec = sym_it->second;
                        vec.erase(std::remove(vec.begin(), vec.end(), order.client_order_id), vec.end());
                    }
                    
                    it = orders_.erase(it);
                    ++removed;
                    continue;
                }
            }
            ++it;
        }
        
        return removed;
    }
    
    // Statistics
    size_t total_orders() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return orders_.size();
    }
    
    size_t active_count() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return active_orders_.size();
    }
    
private:
    static constexpr size_t MAX_ORDERS = 100000;  // Auto-cleanup threshold
    
    mutable std::shared_mutex mutex_;  // Read-write lock for performance
    
    // Primary storage
    std::unordered_map<std::string, Order> orders_;  // client_order_id -> Order
    
    // Indices for fast lookup
    std::unordered_map<std::string, std::string> order_id_to_client_id_;  // exchange ID -> client ID
    std::unordered_map<std::string, std::vector<std::string>> symbol_orders_;  // symbol -> client IDs
    std::unordered_set<std::string> active_orders_;  // Active order IDs
    
    // Internal cleanup (assumes lock held)
    void cleanup_oldest_internal(size_t n) {
        std::vector<std::pair<std::string, TimePoint>> completed_orders;
        
        for (const auto& [client_id, order] : orders_) {
            if (order.is_complete()) {
                completed_orders.emplace_back(client_id, order.completed_time);
            }
        }
        
        // Sort by completion time (oldest first)
        std::sort(completed_orders.begin(), completed_orders.end(),
                 [](const auto& a, const auto& b) { return a.second < b.second; });
        
        // Remove oldest N
        size_t to_remove = std::min(n, completed_orders.size());
        for (size_t i = 0; i < to_remove; ++i) {
            const auto& client_id = completed_orders[i].first;
            auto it = orders_.find(client_id);
            if (it != orders_.end()) {
                // Remove from indices
                order_id_to_client_id_.erase(it->second.order_id);
                active_orders_.erase(client_id);
                
                auto sym_it = symbol_orders_.find(it->second.symbol);
                if (sym_it != symbol_orders_.end()) {
                    auto& vec = sym_it->second;
                    vec.erase(std::remove(vec.begin(), vec.end(), client_id), vec.end());
                }
                
                orders_.erase(it);
            }
        }
    }
};

} // namespace trading
