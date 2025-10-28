#pragma once

#include "types.hpp"
#include <vector>
#include <mutex>
#include <cstdlib>
#include <new>

namespace trading {

// Object pool for high-frequency allocations
// Eliminates malloc/free overhead in hot paths
template<typename T, size_t BlockSize = 1024>
class ObjectPool {
public:
    ObjectPool() {
        allocate_block();
    }
    
    ~ObjectPool() {
        // Destroy all objects
        for (auto* obj : all_objects_) {
            obj->~T();
        }
        
        // Free all blocks
        for (void* block : blocks_) {
            free(block);
        }
    }
    
    // Allocate object from pool
    template<typename... Args>
    T* allocate(Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (free_list_.empty()) {
            allocate_block();
        }
        
        T* obj = free_list_.back();
        free_list_.pop_back();
        
        // Construct object in-place
        new (obj) T(std::forward<Args>(args)...);
        
        return obj;
    }
    
    // Return object to pool
    void deallocate(T* obj) {
        if (!obj) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        // Destroy object but don't free memory
        obj->~T();
        
        free_list_.push_back(obj);
    }
    
    // Statistics
    size_t total_allocated() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return all_objects_.size();
    }
    
    size_t available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return free_list_.size();
    }
    
    size_t in_use() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return all_objects_.size() - free_list_.size();
    }
    
private:
    mutable std::mutex mutex_;
    std::vector<void*> blocks_;      // Memory blocks
    std::vector<T*> all_objects_;    // All allocated objects
    std::vector<T*> free_list_;      // Available objects
    
    void allocate_block() {
        // Allocate cache-aligned block
        size_t block_bytes = BlockSize * sizeof(T);
        void* block = aligned_alloc(64, block_bytes);
        
        if (!block) {
            throw std::bad_alloc();
        }
        
        blocks_.push_back(block);
        
        // Add all objects in block to free list
        T* objects = static_cast<T*>(block);
        for (size_t i = 0; i < BlockSize; ++i) {
            T* obj = &objects[i];
            all_objects_.push_back(obj);
            free_list_.push_back(obj);
        }
    }
};

// Specialized pools for common types
class OrderPool {
public:
    static OrderPool& instance() {
        static OrderPool pool;
        return pool;
    }
    
    Order* allocate() {
        return pool_.allocate();
    }
    
    void deallocate(Order* order) {
        pool_.deallocate(order);
    }
    
    size_t in_use() const {
        return pool_.in_use();
    }
    
private:
    ObjectPool<Order, 2048> pool_;  // 2048 orders per block
};

class FillPool {
public:
    static FillPool& instance() {
        static FillPool pool;
        return pool;
    }
    
    Fill* allocate() {
        return pool_.allocate();
    }
    
    void deallocate(Fill* fill) {
        pool_.deallocate(fill);
    }
    
    size_t in_use() const {
        return pool_.in_use();
    }
    
private:
    ObjectPool<Fill, 2048> pool_;  // 2048 fills per block
};

// RAII wrapper for automatic deallocation
template<typename T>
class PooledPtr {
public:
    PooledPtr() : ptr_(nullptr), pool_(nullptr) {}
    
    explicit PooledPtr(T* ptr, ObjectPool<T>* pool) 
        : ptr_(ptr), pool_(pool) {}
    
    ~PooledPtr() {
        if (ptr_ && pool_) {
            pool_->deallocate(ptr_);
        }
    }
    
    // No copy
    PooledPtr(const PooledPtr&) = delete;
    PooledPtr& operator=(const PooledPtr&) = delete;
    
    // Move
    PooledPtr(PooledPtr&& other) noexcept
        : ptr_(other.ptr_), pool_(other.pool_)
    {
        other.ptr_ = nullptr;
        other.pool_ = nullptr;
    }
    
    PooledPtr& operator=(PooledPtr&& other) noexcept {
        if (this != &other) {
            if (ptr_ && pool_) {
                pool_->deallocate(ptr_);
            }
            ptr_ = other.ptr_;
            pool_ = other.pool_;
            other.ptr_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }
    
    T* get() const { return ptr_; }
    T& operator*() const { return *ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    
    T* release() {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }
    
private:
    T* ptr_;
    ObjectPool<T>* pool_;
};

// Convenience functions
inline Order* allocate_order() {
    return OrderPool::instance().allocate();
}

inline void deallocate_order(Order* order) {
    OrderPool::instance().deallocate(order);
}

inline Fill* allocate_fill() {
    return FillPool::instance().allocate();
}

inline void deallocate_fill(Fill* fill) {
    FillPool::instance().deallocate(fill);
}

// Memory pool statistics
struct PoolStats {
    size_t orders_in_use;
    size_t fills_in_use;
    size_t total_order_capacity;
    size_t total_fill_capacity;
};

inline PoolStats get_pool_stats() {
    PoolStats stats;
    stats.orders_in_use = OrderPool::instance().in_use();
    stats.fills_in_use = FillPool::instance().in_use();
    // Capacity would need additional tracking
    stats.total_order_capacity = 0;
    stats.total_fill_capacity = 0;
    return stats;
}

} // namespace trading
