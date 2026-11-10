#pragma once

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <utility>

namespace carplay::core {

class Bytes {
public:
    Bytes() = default;
    ~Bytes() { free(data_); }
    Bytes(const Bytes &) = delete;
    Bytes &operator=(const Bytes &) = delete;
    Bytes(Bytes &&other) noexcept : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = other.capacity_ = 0;
    }
    Bytes &operator=(Bytes &&other) noexcept {
        if (this != &other) {
            free(data_);
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = other.capacity_ = 0;
        }
        return *this;
    }

    const uint8_t *data() const { return data_; }
    uint8_t *data() { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    uint8_t operator[](size_t index) const { return data_[index]; }

    bool append(const uint8_t *bytes, size_t count) {
        if (count == 0) return true;
        if (!reserve(size_ + count)) return false;
        memcpy(data_ + size_, bytes, count);
        size_ += count;
        return true;
    }
    bool push(uint8_t byte) { return append(&byte, 1); }
    bool assign(const uint8_t *bytes, size_t count) {
        clear();
        return append(bytes, count);
    }
    void clear() { size_ = 0; }
    uint8_t *extend(size_t count) {
        if (!reserve(size_ + count)) return nullptr;
        uint8_t *at = data_ + size_;
        size_ += count;
        return at;
    }
    void truncate(size_t size) {
        if (size < size_) size_ = size;
    }
    void erase_front(size_t count) {
        if (count >= size_) {
            size_ = 0;
            return;
        }
        memmove(data_, data_ + count, size_ - count);
        size_ -= count;
    }

private:
    bool reserve(size_t wanted) {
        if (wanted <= capacity_) return true;
        size_t next = capacity_ ? capacity_ : 64;
        while (next < wanted) next *= 2;
        void *grown = realloc(data_, next);
        if (!grown) return false;
        data_ = static_cast<uint8_t *>(grown);
        capacity_ = next;
        return true;
    }

    uint8_t *data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

template <class T, size_t N> class FixedList {
public:
    FixedList() = default;
    ~FixedList() { clear(); }
    FixedList(const FixedList &) = delete;
    FixedList &operator=(const FixedList &) = delete;

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    T &operator[](size_t index) { return *slot(index); }
    const T &operator[](size_t index) const { return *slot(index); }
    T &front() { return *slot(0); }

    bool push_back(T &&value) {
        if (size_ == N) return false;
        new (raw(size_)) T(std::move(value));
        ++size_;
        return true;
    }
    T take(size_t index) {
        T value(std::move(*slot(index)));
        for (size_t i = index; i + 1 < size_; ++i) *slot(i) = std::move(*slot(i + 1));
        slot(size_ - 1)->~T();
        --size_;
        return value;
    }
    void clear() {
        while (size_) slot(--size_)->~T();
    }
    template <class Less> void sort(Less less) {
        for (size_t i = 1; i < size_; ++i)
            for (size_t j = i; j > 0 && less(*slot(j), *slot(j - 1)); --j) {
                T held(std::move(*slot(j)));
                *slot(j) = std::move(*slot(j - 1));
                *slot(j - 1) = std::move(held);
            }
    }

private:
    void *raw(size_t index) { return storage_ + index * sizeof(T); }
    T *slot(size_t index) { return reinterpret_cast<T *>(storage_ + index * sizeof(T)); }
    const T *slot(size_t index) const { return reinterpret_cast<const T *>(storage_ + index * sizeof(T)); }

    alignas(T) unsigned char storage_[N * sizeof(T)];
    size_t size_ = 0;
};

} // namespace carplay::core
