// Ported from DiPlay's BplistCodec
#pragma once

#include "rt.h"

namespace carplay::core {

class Plist {
public:
    enum class Type : uint8_t { Null, Bool, Int, Real, String, Data, Array, Dict };

    Plist() = default;
    ~Plist();
    Plist(const Plist &) = delete;
    Plist &operator=(const Plist &) = delete;
    Plist(Plist &&other) noexcept;
    Plist &operator=(Plist &&other) noexcept;

    static Plist boolean(bool value);
    static Plist integer(int64_t value);
    static Plist real(double value);
    static Plist string(const char *utf8);
    static Plist string(const char *utf8, size_t length);
    static Plist data(const uint8_t *bytes, size_t length);
    static Plist array();
    static Plist dict();

    Type type() const { return type_; }
    bool is(Type type) const { return type_ == type; }
    bool as_bool() const { return type_ == Type::Bool && scalar_.flag; }
    int64_t as_int() const { return type_ == Type::Int ? scalar_.number : 0; }
    double as_real() const { return type_ == Type::Real ? scalar_.real : 0; }
    const char *as_string() const;
    const uint8_t *bytes() const { return text_.data(); }
    size_t byte_count() const;
    bool to_int(int64_t &out) const;

    size_t size() const { return type_ == Type::Dict ? count_ / 2 : count_; }
    const Plist &at(size_t index) const { return type_ == Type::Dict ? items_[index * 2 + 1] : items_[index]; }
    const char *key(size_t index) const { return items_[index * 2].as_string(); }
    const Plist *get(const char *key) const;

    Plist &push(Plist &&value);
    Plist &set(const char *key, Plist &&value);
    bool ok() const;

private:
    bool grow(size_t wanted);

    Type type_ = Type::Null;
    bool failed_ = false;
    union {
        bool flag;
        int64_t number;
        double real;
    } scalar_ = {};
    Bytes text_;
    Plist *items_ = nullptr;
    size_t count_ = 0, capacity_ = 0;
};

bool bplist_encode(const Plist &root, Bytes &out);

bool bplist_decode(const uint8_t *bytes, size_t length, Plist &out);

} // namespace carplay::core
