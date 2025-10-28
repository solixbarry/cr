#pragma once

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <type_traits>

namespace trading {

// High-performance circular buffer - cache-friendly, no allocations after construction
template<typename T>
class CircularBuffer {
public:
    explicit CircularBuffer(size_t capacity)
        : capacity_(capacity)
        , size_(0)
        , head_(0)
        , tail_(0)
    {
        if (capacity == 0) {
            throw std::invalid_argument("CircularBuffer capacity must be > 0");
        }
        
        // Allocate aligned memory for cache efficiency
        data_ = static_cast<T*>(aligned_alloc(64, capacity * sizeof(T)));
        if (!data_) {
            throw std::bad_alloc();
        }
        
        // Initialize objects if not trivially constructible
        if constexpr (!std::is_trivially_constructible_v<T>) {
            for (size_t i = 0; i < capacity_; ++i) {
                new (&data_[i]) T();
            }
        }
    }
    
    ~CircularBuffer() {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_t i = 0; i < capacity_; ++i) {
                data_[i].~T();
            }
        }
        free(data_);
    }
    
    // No copy (expensive)
    CircularBuffer(const CircularBuffer&) = delete;
    CircularBuffer& operator=(const CircularBuffer&) = delete;
    
    // Move is ok
    CircularBuffer(CircularBuffer&& other) noexcept
        : data_(other.data_)
        , capacity_(other.capacity_)
        , size_(other.size_)
        , head_(other.head_)
        , tail_(other.tail_)
    {
        other.data_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
    }
    
    // Push element (overwrites oldest if full)
    void push_back(const T& value) {
        data_[head_] = value;
        head_ = (head_ + 1) % capacity_;
        
        if (size_ < capacity_) {
            size_++;
        } else {
            tail_ = (tail_ + 1) % capacity_;  // Overwrite oldest
        }
    }
    
    void push_back(T&& value) {
        data_[head_] = std::move(value);
        head_ = (head_ + 1) % capacity_;
        
        if (size_ < capacity_) {
            size_++;
        } else {
            tail_ = (tail_ + 1) % capacity_;
        }
    }
    
    // Pop oldest element
    void pop_front() {
        if (size_ == 0) {
            throw std::underflow_error("CircularBuffer is empty");
        }
        
        tail_ = (tail_ + 1) % capacity_;
        size_--;
    }
    
    // Access elements
    T& operator[](size_t index) {
        if (index >= size_) {
            throw std::out_of_range("CircularBuffer index out of range");
        }
        return data_[(tail_ + index) % capacity_];
    }
    
    const T& operator[](size_t index) const {
        if (index >= size_) {
            throw std::out_of_range("CircularBuffer index out of range");
        }
        return data_[(tail_ + index) % capacity_];
    }
    
    T& front() {
        if (size_ == 0) {
            throw std::underflow_error("CircularBuffer is empty");
        }
        return data_[tail_];
    }
    
    const T& front() const {
        if (size_ == 0) {
            throw std::underflow_error("CircularBuffer is empty");
        }
        return data_[tail_];
    }
    
    T& back() {
        if (size_ == 0) {
            throw std::underflow_error("CircularBuffer is empty");
        }
        size_t back_idx = (head_ == 0) ? capacity_ - 1 : head_ - 1;
        return data_[back_idx];
    }
    
    const T& back() const {
        if (size_ == 0) {
            throw std::underflow_error("CircularBuffer is empty");
        }
        size_t back_idx = (head_ == 0) ? capacity_ - 1 : head_ - 1;
        return data_[back_idx];
    }
    
    // Size queries
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }
    bool full() const { return size_ == capacity_; }
    
    // Clear buffer
    void clear() {
        size_ = 0;
        head_ = 0;
        tail_ = 0;
    }
    
    // Iterator support for range-based for loops
    class Iterator {
    public:
        Iterator(CircularBuffer* buf, size_t index) 
            : buffer_(buf), index_(index) {}
        
        T& operator*() { return (*buffer_)[index_]; }
        T* operator->() { return &(*buffer_)[index_]; }
        
        Iterator& operator++() {
            ++index_;
            return *this;
        }
        
        bool operator!=(const Iterator& other) const {
            return index_ != other.index_;
        }
        
    private:
        CircularBuffer* buffer_;
        size_t index_;
    };
    
    class ConstIterator {
    public:
        ConstIterator(const CircularBuffer* buf, size_t index) 
            : buffer_(buf), index_(index) {}
        
        const T& operator*() const { return (*buffer_)[index_]; }
        const T* operator->() const { return &(*buffer_)[index_]; }
        
        ConstIterator& operator++() {
            ++index_;
            return *this;
        }
        
        bool operator!=(const ConstIterator& other) const {
            return index_ != other.index_;
        }
        
    private:
        const CircularBuffer* buffer_;
        size_t index_;
    };
    
    Iterator begin() { return Iterator(this, 0); }
    Iterator end() { return Iterator(this, size_); }
    ConstIterator begin() const { return ConstIterator(this, 0); }
    ConstIterator end() const { return ConstIterator(this, size_); }
    
private:
    T* data_;
    size_t capacity_;
    size_t size_;
    size_t head_;  // Next write position
    size_t tail_;  // Oldest element position
};

} // namespace trading
