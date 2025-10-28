#pragma once

#include "types.hpp"
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <string>

namespace trading {

// Circuit breaker state
enum class CircuitState {
    CLOSED,      // Normal operation
    OPEN,        // Breaker tripped, no trading
    HALF_OPEN    // Testing if system recovered
};

// Circuit breaker - prevents cascading failures
class CircuitBreaker {
public:
    struct Config {
        int failure_threshold;          // Number of failures to trip
        int success_threshold;          // Successes needed to close from half-open
        std::chrono::seconds timeout;   // Time before trying half-open
        std::chrono::seconds test_period; // How long to stay in half-open
        
        Config()
            : failure_threshold(5)
            , success_threshold(3)
            , timeout(std::chrono::seconds(30))
            , test_period(std::chrono::seconds(10))
        {}
    };
    
    explicit CircuitBreaker(const std::string& name, const Config& config = Config())
        : name_(name)
        , config_(config)
        , state_(CircuitState::CLOSED)
        , failure_count_(0)
        , success_count_(0)
    {}
    
    // Check if request is allowed
    bool allow_request() {
        auto state = state_.load(std::memory_order_acquire);
        
        if (state == CircuitState::OPEN) {
            // Check if timeout has elapsed
            auto now = Clock::now();
            auto last_fail = last_failure_time_.load(std::memory_order_acquire);
            
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_fail
            );
            
            if (elapsed >= config_.timeout) {
                // Try half-open
                CircuitState expected = CircuitState::OPEN;
                if (state_.compare_exchange_strong(expected, CircuitState::HALF_OPEN,
                                                   std::memory_order_acq_rel)) {
                    half_open_start_ = now;
                    LOG_WARN("Circuit breaker " << name_ << " entering HALF_OPEN state");
                }
                return true;  // Allow test request
            }
            
            return false;  // Still in timeout
        }
        
        if (state == CircuitState::HALF_OPEN) {
            // Check if test period expired
            auto now = Clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - half_open_start_
            );
            
            if (elapsed >= config_.test_period) {
                // Test period over, open again if not enough successes
                if (success_count_.load(std::memory_order_acquire) < config_.success_threshold) {
                    open("Test period failed");
                    return false;
                }
            }
        }
        
        return true;  // CLOSED or HALF_OPEN during test period
    }
    
    // Record success
    void record_success() {
        auto state = state_.load(std::memory_order_acquire);
        
        if (state == CircuitState::HALF_OPEN) {
            int successes = success_count_.fetch_add(1, std::memory_order_relaxed) + 1;
            
            if (successes >= config_.success_threshold) {
                // Enough successes, close circuit
                CircuitState expected = CircuitState::HALF_OPEN;
                if (state_.compare_exchange_strong(expected, CircuitState::CLOSED,
                                                   std::memory_order_acq_rel)) {
                    failure_count_.store(0, std::memory_order_relaxed);
                    success_count_.store(0, std::memory_order_relaxed);
                    LOG_INFO("Circuit breaker " << name_ << " CLOSED (recovered)");
                }
            }
        } else if (state == CircuitState::CLOSED) {
            // Decay failure count on success
            int current = failure_count_.load(std::memory_order_relaxed);
            if (current > 0) {
                failure_count_.store(current - 1, std::memory_order_relaxed);
            }
        }
    }
    
    // Record failure
    void record_failure(const std::string& reason = "") {
        auto state = state_.load(std::memory_order_acquire);
        
        if (state == CircuitState::HALF_OPEN) {
            // Failure during test, open immediately
            open("Failed during half-open: " + reason);
            return;
        }
        
        if (state == CircuitState::CLOSED) {
            int failures = failure_count_.fetch_add(1, std::memory_order_relaxed) + 1;
            
            if (failures >= config_.failure_threshold) {
                open("Threshold reached: " + reason);
            }
        }
    }
    
    // Force open
    void open(const std::string& reason) {
        CircuitState expected = state_.load(std::memory_order_acquire);
        
        if (expected != CircuitState::OPEN) {
            state_.store(CircuitState::OPEN, std::memory_order_release);
            last_failure_time_.store(Clock::now(), std::memory_order_release);
            
            LOG_ERROR("Circuit breaker " << name_ << " OPENED: " << reason);
        }
    }
    
    // Force close (manual override)
    void close() {
        state_.store(CircuitState::CLOSED, std::memory_order_release);
        failure_count_.store(0, std::memory_order_relaxed);
        success_count_.store(0, std::memory_order_relaxed);
        
        LOG_INFO("Circuit breaker " << name_ << " manually CLOSED");
    }
    
    // Get state
    CircuitState get_state() const {
        return state_.load(std::memory_order_acquire);
    }
    
    bool is_open() const {
        return state_.load(std::memory_order_acquire) == CircuitState::OPEN;
    }
    
    const std::string& name() const { return name_; }
    
private:
    std::string name_;
    Config config_;
    
    std::atomic<CircuitState> state_;
    std::atomic<int> failure_count_;
    std::atomic<int> success_count_;
    std::atomic<TimePoint> last_failure_time_;
    TimePoint half_open_start_;
};

// Emergency kill switch - immediately stops all trading
class KillSwitch {
public:
    KillSwitch() : activated_(false) {}
    
    // Activate kill switch
    void activate(const std::string& reason) {
        bool expected = false;
        if (activated_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            activation_reason_ = reason;
            activation_time_ = Clock::now();
            
            LOG_ERROR("!!! KILL SWITCH ACTIVATED !!!");
            LOG_ERROR("Reason: " << reason);
            
            // Execute all shutdown handlers
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            for (auto& handler : shutdown_handlers_) {
                try {
                    handler();
                } catch (const std::exception& e) {
                    LOG_ERROR("Shutdown handler failed: " << e.what());
                }
            }
            
            LOG_ERROR("All shutdown handlers executed");
        }
    }
    
    // Register shutdown handler (called when kill switch activated)
    void register_shutdown_handler(std::function<void()> handler) {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        shutdown_handlers_.push_back(std::move(handler));
    }
    
    // Check if activated
    bool is_activated() const {
        return activated_.load(std::memory_order_acquire);
    }
    
    // Get activation details
    std::string get_reason() const {
        return activation_reason_;
    }
    
    TimePoint get_activation_time() const {
        return activation_time_;
    }
    
    // Reset (requires manual override)
    void reset() {
        activated_.store(false, std::memory_order_release);
        activation_reason_.clear();
        
        LOG_WARN("Kill switch manually reset");
    }
    
private:
    std::atomic<bool> activated_;
    std::string activation_reason_;
    TimePoint activation_time_;
    
    std::mutex handlers_mutex_;
    std::vector<std::function<void()>> shutdown_handlers_;
};

// Error rate tracker (for circuit breaker decisions)
class ErrorRateTracker {
public:
    struct Config {
        std::chrono::seconds window;    // Time window to track
        int threshold;                   // Max errors in window
        
        Config()
            : window(std::chrono::seconds(60))
            , threshold(10)
        {}
    };
    
    explicit ErrorRateTracker(const Config& config = Config())
        : config_(config)
    {}
    
    // Record error
    void record_error() {
        auto now = Clock::now();
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Remove old errors outside window
        while (!error_times_.empty()) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - error_times_.front()
            );
            
            if (age > config_.window) {
                error_times_.erase(error_times_.begin());
            } else {
                break;
            }
        }
        
        error_times_.push_back(now);
    }
    
    // Check if threshold exceeded
    bool threshold_exceeded() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_times_.size() >= static_cast<size_t>(config_.threshold);
    }
    
    // Get current error count
    size_t get_error_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return error_times_.size();
    }
    
    // Clear errors
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        error_times_.clear();
    }
    
private:
    Config config_;
    mutable std::mutex mutex_;
    std::vector<TimePoint> error_times_;
};

} // namespace trading
