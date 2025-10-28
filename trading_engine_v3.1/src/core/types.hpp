#pragma once

#include <string>
#include <chrono>
#include <cstdint>

namespace trading {

using Clock = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;
using Nanoseconds = std::chrono::nanoseconds;

// Execution venue
enum class Venue : uint8_t {
    BINANCE,
    BYBIT,
    COINBASE,
    KRAKEN,
    FTX,
    UNKNOWN
};

// Order side
enum class Side : uint8_t {
    BUY,
    SELL
};

// Order type
enum class OrderType : uint8_t {
    LIMIT,
    MARKET,
    LIMIT_MAKER,
    LIMIT_IOC,
    STOP_LOSS,
    STOP_LIMIT
};

// Order status
enum class OrderStatus : uint8_t {
    PENDING,
    NEW,
    PARTIALLY_FILLED,
    FILLED,
    CANCELED,
    REJECTED,
    EXPIRED
};

// Fill information - PROPER institutional structure
struct Fill {
    // Identity
    std::string fill_id;           // Unique fill ID from exchange
    std::string order_id;           // Exchange order ID
    std::string client_order_id;    // Our internal order ID
    
    // CRITICAL: Symbol must be in Fill
    std::string symbol;             // e.g. "BTCUSDT"
    
    // Execution details
    Side side;                      // BUY or SELL
    double price;                   // Execution price
    double quantity;                // Fill quantity
    double fee;                     // Fee paid
    std::string fee_currency;       // Fee currency (e.g. "USDT")
    
    // Liquidity
    bool is_maker;                  // true = maker, false = taker
    
    // Venue & timing
    Venue venue;                    // Which exchange
    TimePoint exchange_time;        // Exchange timestamp
    TimePoint received_time;        // When we received it
    TimePoint processed_time;       // When we processed it
    
    // Derived metrics (calculated on receipt)
    int64_t latency_us;             // Latency from exchange to us
    
    // Quote at time of fill (for analysis)
    double bid_at_fill;             // Best bid when filled
    double ask_at_fill;             // Best ask when filled
    double mid_at_fill;             // Mid price when filled
    
    Fill() 
        : side(Side::BUY)
        , price(0.0)
        , quantity(0.0)
        , fee(0.0)
        , is_maker(false)
        , venue(Venue::UNKNOWN)
        , latency_us(0)
        , bid_at_fill(0.0)
        , ask_at_fill(0.0)
        , mid_at_fill(0.0)
    {}
    
    // Helper to calculate slippage
    double calculate_slippage() const {
        if (mid_at_fill == 0.0) return 0.0;
        
        if (side == Side::BUY) {
            // Positive slippage = paid more than mid
            return (price - mid_at_fill) / mid_at_fill;
        } else {
            // Positive slippage = received less than mid
            return (mid_at_fill - price) / mid_at_fill;
        }
    }
    
    // Net PnL for this fill (including fees)
    double net_value() const {
        double gross = price * quantity;
        return side == Side::BUY ? -(gross + fee) : (gross - fee);
    }
};

// Order information - proper structure
struct Order {
    // Identity
    std::string order_id;           // Exchange order ID (after ACK)
    std::string client_order_id;    // Our internal ID
    
    // Instrument
    std::string symbol;
    Venue venue;
    
    // Order details
    Side side;
    OrderType type;
    double price;                   // Limit price (0 for market)
    double quantity;                // Original quantity
    double filled_quantity;         // How much filled so far
    double remaining_quantity;      // Quantity still open
    
    // Status
    OrderStatus status;
    std::string reject_reason;      // If rejected
    
    // Timing
    TimePoint created_time;         // When we created order
    TimePoint sent_time;            // When we sent to exchange
    TimePoint ack_time;             // When exchange ACKed
    TimePoint completed_time;       // When fully filled/canceled
    
    // Strategy context
    std::string strategy_name;      // Which strategy generated this
    int signal_id;                  // Signal ID (for tracking)
    
    // Risk tracking
    double risk_notional;           // Notional value for risk
    
    Order()
        : venue(Venue::UNKNOWN)
        , side(Side::BUY)
        , type(OrderType::LIMIT)
        , price(0.0)
        , quantity(0.0)
        , filled_quantity(0.0)
        , remaining_quantity(0.0)
        , status(OrderStatus::PENDING)
        , signal_id(0)
        , risk_notional(0.0)
    {}
    
    // Calculate latencies
    int64_t creation_to_send_us() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            sent_time - created_time
        ).count();
    }
    
    int64_t send_to_ack_us() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            ack_time - sent_time
        ).count();
    }
    
    int64_t total_latency_us() const {
        if (status == OrderStatus::PENDING || status == OrderStatus::NEW) {
            return 0;
        }
        return std::chrono::duration_cast<std::chrono::microseconds>(
            completed_time - created_time
        ).count();
    }
    
    bool is_active() const {
        return status == OrderStatus::NEW || 
               status == OrderStatus::PARTIALLY_FILLED;
    }
    
    bool is_complete() const {
        return status == OrderStatus::FILLED ||
               status == OrderStatus::CANCELED ||
               status == OrderStatus::REJECTED ||
               status == OrderStatus::EXPIRED;
    }
};

// Order acknowledgement from exchange
struct OrderAck {
    std::string order_id;           // Exchange order ID
    std::string client_order_id;    // Our ID
    std::string symbol;
    Venue venue;
    OrderStatus status;             // Usually NEW
    double price;
    double quantity;
    Side side;
    TimePoint timestamp;
    
    OrderAck()
        : venue(Venue::UNKNOWN)
        , status(OrderStatus::NEW)
        , price(0.0)
        , quantity(0.0)
        , side(Side::BUY)
    {}
};

// Order rejection
struct OrderReject {
    std::string client_order_id;
    std::string symbol;
    Venue venue;
    std::string error_code;         // Exchange error code
    std::string error_message;      // Human readable
    TimePoint timestamp;
    
    // For retry logic
    bool is_retriable;              // Can we retry?
    int retry_after_ms;             // Backoff time
    
    OrderReject()
        : venue(Venue::UNKNOWN)
        , is_retriable(false)
        , retry_after_ms(0)
    {}
};

// Helper functions
inline const char* to_string(Side side) {
    return side == Side::BUY ? "BUY" : "SELL";
}

inline const char* to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::PENDING: return "PENDING";
        case OrderStatus::NEW: return "NEW";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED: return "FILLED";
        case OrderStatus::CANCELED: return "CANCELED";
        case OrderStatus::REJECTED: return "REJECTED";
        case OrderStatus::EXPIRED: return "EXPIRED";
        default: return "UNKNOWN";
    }
}

inline const char* to_string(Venue venue) {
    switch (venue) {
        case Venue::BINANCE: return "BINANCE";
        case Venue::BYBIT: return "BYBIT";
        case Venue::COINBASE: return "COINBASE";
        case Venue::KRAKEN: return "KRAKEN";
        case Venue::FTX: return "FTX";
        default: return "UNKNOWN";
    }
}

} // namespace trading
