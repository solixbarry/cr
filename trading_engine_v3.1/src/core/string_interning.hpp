#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cstdint>

namespace trading {

// String interning - convert strings to integer IDs for fast comparison
// All symbol operations use IDs, not strings
class SymbolRegistry {
public:
    using SymbolId = uint16_t;
    static constexpr SymbolId INVALID_SYMBOL = 0;
    
    static SymbolRegistry& instance() {
        static SymbolRegistry registry;
        return registry;
    }
    
    // Register symbol and get ID (idempotent)
    SymbolId register_symbol(std::string_view symbol) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Check if already registered
        auto it = symbol_to_id_.find(std::string(symbol));
        if (it != symbol_to_id_.end()) {
            return it->second;
        }
        
        // Assign new ID
        SymbolId id = next_id_++;
        std::string symbol_str(symbol);
        
        symbol_to_id_[symbol_str] = id;
        id_to_symbol_[id] = std::move(symbol_str);
        
        return id;
    }
    
    // Get ID for symbol (returns INVALID_SYMBOL if not registered)
    SymbolId get_id(std::string_view symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = symbol_to_id_.find(std::string(symbol));
        return it != symbol_to_id_.end() ? it->second : INVALID_SYMBOL;
    }
    
    // Get symbol for ID (returns empty string_view if invalid)
    std::string_view get_symbol(SymbolId id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = id_to_symbol_.find(id);
        return it != id_to_symbol_.end() ? std::string_view(it->second) : std::string_view();
    }
    
    // Check if symbol is registered
    bool is_registered(std::string_view symbol) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return symbol_to_id_.count(std::string(symbol)) > 0;
    }
    
    // Get all registered symbols
    std::vector<std::string> get_all_symbols() const {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::vector<std::string> symbols;
        symbols.reserve(symbol_to_id_.size());
        
        for (const auto& [symbol, id] : symbol_to_id_) {
            symbols.push_back(symbol);
        }
        
        return symbols;
    }
    
    size_t count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return symbol_to_id_.size();
    }
    
private:
    SymbolRegistry() : next_id_(1) {}  // Start from 1, reserve 0 for invalid
    
    mutable std::mutex mutex_;
    std::unordered_map<std::string, SymbolId> symbol_to_id_;
    std::unordered_map<SymbolId, std::string> id_to_symbol_;
    SymbolId next_id_;
};

// Convenience functions
inline SymbolRegistry::SymbolId register_symbol(std::string_view symbol) {
    return SymbolRegistry::instance().register_symbol(symbol);
}

inline SymbolRegistry::SymbolId get_symbol_id(std::string_view symbol) {
    return SymbolRegistry::instance().get_id(symbol);
}

inline std::string_view get_symbol_name(SymbolRegistry::SymbolId id) {
    return SymbolRegistry::instance().get_symbol(id);
}

// Pre-register common symbols at startup
inline void register_common_symbols() {
    // Major pairs
    register_symbol("BTCUSDT");
    register_symbol("ETHUSDT");
    register_symbol("BNBUSDT");
    register_symbol("SOLUSDT");
    register_symbol("XRPUSDT");
    register_symbol("ADAUSDT");
    register_symbol("AVAXUSDT");
    register_symbol("DOGEUSDT");
    register_symbol("DOTUSDT");
    register_symbol("MATICUSDT");
    register_symbol("LINKUSDT");
    register_symbol("UNIUSDT");
    register_symbol("ATOMUSDT");
    register_symbol("LTCUSDT");
    register_symbol("ETCUSDT");
    
    // Cross pairs
    register_symbol("ETHBTC");
    register_symbol("BNBBTC");
    register_symbol("SOLBTC");
}

// String view helper for hot paths
class InternedString {
public:
    InternedString() : id_(SymbolRegistry::INVALID_SYMBOL) {}
    
    explicit InternedString(std::string_view symbol) 
        : id_(SymbolRegistry::instance().register_symbol(symbol))
    {}
    
    explicit InternedString(SymbolRegistry::SymbolId id) : id_(id) {}
    
    SymbolRegistry::SymbolId id() const { return id_; }
    std::string_view view() const { return get_symbol_name(id_); }
    std::string str() const { return std::string(view()); }
    
    bool operator==(const InternedString& other) const {
        return id_ == other.id_;
    }
    
    bool operator!=(const InternedString& other) const {
        return id_ != other.id_;
    }
    
    bool operator<(const InternedString& other) const {
        return id_ < other.id_;
    }
    
    explicit operator bool() const {
        return id_ != SymbolRegistry::INVALID_SYMBOL;
    }
    
private:
    SymbolRegistry::SymbolId id_;
};

} // namespace trading

// Hash specialization for InternedString
namespace std {
    template<>
    struct hash<trading::InternedString> {
        size_t operator()(const trading::InternedString& s) const noexcept {
            return std::hash<trading::SymbolRegistry::SymbolId>{}(s.id());
        }
    };
}
