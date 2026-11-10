#include "bplist.h"

namespace carplay::core {

Plist::~Plist() {
    for (size_t i = 0; i < count_; ++i) items_[i].~Plist();
    free(items_);
}

Plist::Plist(Plist &&other) noexcept
    : type_(other.type_), failed_(other.failed_), scalar_(other.scalar_), text_(std::move(other.text_)),
      items_(other.items_), count_(other.count_), capacity_(other.capacity_) {
    other.type_ = Type::Null;
    other.items_ = nullptr;
    other.count_ = other.capacity_ = 0;
}

Plist &Plist::operator=(Plist &&other) noexcept {
    if (this != &other) {
        this->~Plist();
        new (this) Plist(std::move(other));
    }
    return *this;
}

Plist Plist::boolean(bool value) {
    Plist p;
    p.type_ = Type::Bool;
    p.scalar_.flag = value;
    return p;
}

Plist Plist::integer(int64_t value) {
    Plist p;
    p.type_ = Type::Int;
    p.scalar_.number = value;
    return p;
}

Plist Plist::real(double value) {
    Plist p;
    p.type_ = Type::Real;
    p.scalar_.real = value;
    return p;
}

Plist Plist::string(const char *utf8) { return string(utf8, strlen(utf8)); }

Plist Plist::string(const char *utf8, size_t length) {
    Plist p;
    p.type_ = Type::String;
    p.failed_ = !p.text_.append(reinterpret_cast<const uint8_t *>(utf8), length) || !p.text_.push(0);
    return p;
}

Plist Plist::data(const uint8_t *bytes, size_t length) {
    Plist p;
    p.type_ = Type::Data;
    p.failed_ = !p.text_.append(bytes, length);
    return p;
}

Plist Plist::array() {
    Plist p;
    p.type_ = Type::Array;
    return p;
}

Plist Plist::dict() {
    Plist p;
    p.type_ = Type::Dict;
    return p;
}

const char *Plist::as_string() const {
    return type_ == Type::String && text_.size() ? reinterpret_cast<const char *>(text_.data()) : "";
}

size_t Plist::byte_count() const {
    if (type_ == Type::String) return text_.size() ? text_.size() - 1 : 0;
    return type_ == Type::Data ? text_.size() : 0;
}

bool Plist::to_int(int64_t &out) const {
    if (type_ == Type::Int) {
        out = scalar_.number;
        return true;
    }
    if (type_ == Type::Real && scalar_.real >= -9.0e18 && scalar_.real <= 9.0e18) {
        int64_t whole = static_cast<int64_t>(scalar_.real);
        if (static_cast<double>(whole) != scalar_.real) return false;
        out = whole;
        return true;
    }
    return false;
}

const Plist *Plist::get(const char *wanted) const {
    if (type_ != Type::Dict) return nullptr;
    for (size_t i = 0; i + 1 < count_; i += 2) {
        const char *key = items_[i].as_string();
        size_t k = 0;
        while (key[k] && key[k] == wanted[k]) ++k;
        if (key[k] == wanted[k]) return &items_[i + 1];
    }
    return nullptr;
}

bool Plist::grow(size_t wanted) {
    if (wanted <= capacity_) return true;
    size_t next = capacity_ ? capacity_ * 2 : 4;
    while (next < wanted) next *= 2;
    Plist *grown = static_cast<Plist *>(malloc(next * sizeof(Plist)));
    if (!grown) return false;
    for (size_t i = 0; i < count_; ++i) {
        new (&grown[i]) Plist(std::move(items_[i]));
        items_[i].~Plist();
    }
    free(items_);
    items_ = grown;
    capacity_ = next;
    return true;
}

Plist &Plist::push(Plist &&value) {
    if (type_ != Type::Array || !grow(count_ + 1)) {
        failed_ = true;
        return *this;
    }
    new (&items_[count_++]) Plist(std::move(value));
    return *this;
}

Plist &Plist::set(const char *key, Plist &&value) {
    if (type_ != Type::Dict || !grow(count_ + 2)) {
        failed_ = true;
        return *this;
    }
    new (&items_[count_++]) Plist(string(key));
    new (&items_[count_++]) Plist(std::move(value));
    return *this;
}

bool Plist::ok() const {
    if (failed_) return false;
    for (size_t i = 0; i < count_; ++i)
        if (!items_[i].ok()) return false;
    return true;
}

namespace {

const uint8_t kMagic[8] = {0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30}; // "bplist00"

void put_be(uint8_t *at, uint64_t value, unsigned size) {
    for (unsigned i = size; i-- > 0;) {
        at[i] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

bool append_be(Bytes &out, uint64_t value, unsigned size) {
    uint8_t bytes[8];
    put_be(bytes, value, size);
    return out.append(bytes, size);
}

unsigned width_for(uint64_t value) {
    return value > 0xffffffffu ? 8 : value > 0xffff ? 4 : value > 0xff ? 2 : 1;
}

unsigned log2_width(unsigned width) { return width == 8 ? 3 : width == 4 ? 2 : width == 2 ? 1 : 0; }

bool append_marker(Bytes &out, unsigned type, size_t count) {
    if (count < 0x0f) return out.push(static_cast<uint8_t>(type << 4 | count));
    unsigned width = width_for(count);
    return out.push(static_cast<uint8_t>(type << 4 | 0x0f)) && out.push(static_cast<uint8_t>(0x10 | log2_width(width))) &&
           append_be(out, count, width);
}

uint32_t next_code_point(const uint8_t *text, size_t length, size_t &i) {
    uint8_t lead = text[i++];
    if (lead < 0x80) return lead;
    unsigned extra = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2 : lead >= 0xc0 ? 1 : 0;
    if (!extra || i + extra > length) return 0xfffd;
    uint32_t code = lead & (0x3f >> extra);
    for (unsigned k = 0; k < extra; ++k) {
        if ((text[i] & 0xc0) != 0x80) return 0xfffd;
        code = code << 6 | (text[i++] & 0x3f);
    }
    return code > 0x10ffff ? 0xfffd : code;
}

bool append_utf8(Bytes &out, uint32_t code) {
    uint8_t encoded[4];
    unsigned n;
    if (code < 0x80) {
        encoded[0] = static_cast<uint8_t>(code);
        n = 1;
    } else if (code < 0x800) {
        encoded[0] = static_cast<uint8_t>(0xc0 | code >> 6);
        encoded[1] = static_cast<uint8_t>(0x80 | (code & 0x3f));
        n = 2;
    } else if (code < 0x10000) {
        encoded[0] = static_cast<uint8_t>(0xe0 | code >> 12);
        encoded[1] = static_cast<uint8_t>(0x80 | (code >> 6 & 0x3f));
        encoded[2] = static_cast<uint8_t>(0x80 | (code & 0x3f));
        n = 3;
    } else {
        encoded[0] = static_cast<uint8_t>(0xf0 | code >> 18);
        encoded[1] = static_cast<uint8_t>(0x80 | (code >> 12 & 0x3f));
        encoded[2] = static_cast<uint8_t>(0x80 | (code >> 6 & 0x3f));
        encoded[3] = static_cast<uint8_t>(0x80 | (code & 0x3f));
        n = 4;
    }
    return out.append(encoded, n);
}

bool append_string(Bytes &out, const Plist &value) {
    const uint8_t *text = value.bytes();
    size_t length = value.byte_count();
    bool ascii = true;
    for (size_t i = 0; i < length; ++i) ascii = ascii && text[i] < 0x80;
    if (ascii) return append_marker(out, 0x5, length) && out.append(text, length);
    Bytes units; // UTF-16BE
    for (size_t i = 0; i < length;) {
        uint32_t code = next_code_point(text, length, i);
        if (code >= 0x10000) {
            code -= 0x10000;
            if (!append_be(units, 0xd800 | code >> 10, 2) || !append_be(units, 0xdc00 | (code & 0x3ff), 2)) return false;
        } else if (!append_be(units, code, 2)) {
            return false;
        }
    }
    return append_marker(out, 0x6, units.size() / 2) && out.append(units.data(), units.size());
}

size_t subtree_objects(const Plist &node) {
    size_t total = 1;
    if (node.is(Plist::Type::Dict)) total += node.size();
    if (node.is(Plist::Type::Dict) || node.is(Plist::Type::Array))
        for (size_t i = 0; i < node.size(); ++i) total += subtree_objects(node.at(i));
    return total;
}

struct Encoder {
    Bytes &out;
    uint64_t *offsets;
    unsigned ref_width;

    bool leaf(const Plist &node, size_t index) {
        offsets[index] = out.size();
        switch (node.type()) {
        case Plist::Type::Null: return out.push(0x00);
        case Plist::Type::Bool: return out.push(node.as_bool() ? 0x09 : 0x08);
        case Plist::Type::Int: {
            // 1, 2 and 4 byte integers are unsigned; negatives take the signed 8-byte form.
            int64_t value = node.as_int();
            unsigned width = value < 0 ? 8 : width_for(static_cast<uint64_t>(value));
            return out.push(static_cast<uint8_t>(0x10 | log2_width(width))) &&
                   append_be(out, static_cast<uint64_t>(value), width);
        }
        case Plist::Type::Real: {
            double value = node.as_real();
            uint64_t bits;
            memcpy(&bits, &value, sizeof bits);
            return out.push(0x23) && append_be(out, bits, 8);
        }
        case Plist::Type::String: return append_string(out, node);
        case Plist::Type::Data:
            return append_marker(out, 0x4, node.byte_count()) && out.append(node.bytes(), node.byte_count());
        default: return false;
        }
    }

    bool key(const char *text, size_t index) {
        offsets[index] = out.size();
        size_t length = strlen(text);
        Plist value = Plist::string(text, length);
        return value.ok() && append_string(out, value);
    }

    bool emit(const Plist &node, size_t index) {
        bool dict = node.is(Plist::Type::Dict);
        if (!dict && !node.is(Plist::Type::Array)) return leaf(node, index);
        offsets[index] = out.size();
        size_t n = node.size();
        if (!append_marker(out, dict ? 0xd : 0xa, n)) return false;
        // Object order DiPlay
        size_t first_value = index + 1 + (dict ? n : 0);
        if (dict)
            for (size_t i = 0; i < n; ++i)
                if (!append_be(out, index + 1 + i, ref_width)) return false;
        size_t next = first_value;
        for (size_t i = 0; i < n; ++i) {
            if (!append_be(out, next, ref_width)) return false;
            next += subtree_objects(node.at(i));
        }
        if (dict)
            for (size_t i = 0; i < n; ++i)
                if (!key(node.key(i), index + 1 + i)) return false;
        next = first_value;
        for (size_t i = 0; i < n; ++i) {
            if (!emit(node.at(i), next)) return false;
            next += subtree_objects(node.at(i));
        }
        return true;
    }
};

const unsigned kMaxDepth = 32;
const uint64_t kMaxReads = 100000;
const uint64_t kMaxPayload = 4u * 1024 * 1024;
const uint64_t kSmallPayload = 64;

struct Decoder {
    const uint8_t *bytes;
    uint64_t length;
    uint64_t table;
    unsigned offset_width, ref_width;
    uint64_t objects;
    uint64_t reads, payload;

    bool read_be(uint64_t at, unsigned width, uint64_t &value) const {
        if (at > length || width > length - at) return false;
        value = 0;
        for (unsigned i = 0; i < width; ++i) value = value << 8 | bytes[at + i];
        return true;
    }

    size_t left(uint64_t at) const { return static_cast<size_t>(length - at); }

    bool offset_of(uint64_t index, uint64_t &at) const {
        return index < objects && read_be(table + index * offset_width, offset_width, at) && at >= sizeof kMagic &&
               at < table;
    }

    bool charge(uint64_t size) {
        if (size <= kSmallPayload) return true;
        if (size > payload) return false;
        payload -= size;
        return true;
    }

    bool count(unsigned nibble, uint64_t &at, uint64_t &n) const {
        if (nibble != 0x0f) {
            n = nibble;
        } else {
            uint64_t size_marker;
            if (!read_be(at, 1, size_marker) || (size_marker & 0xf0) != 0x10 || (size_marker & 0x0f) > 3) return false;
            unsigned width = 1u << (size_marker & 0x0f);
            if (!read_be(at + 1, width, n)) return false;
            at += 1 + width;
        }
        return at <= length && n <= length - at;
    }

    bool utf16(uint64_t at, uint64_t units, Plist &out) {
        Bytes text;
        for (uint64_t i = 0; i < units; ++i) {
            uint32_t code = static_cast<uint32_t>(bytes[at + 2 * i] << 8 | bytes[at + 2 * i + 1]);
            if (code >= 0xd800 && code < 0xdc00 && i + 1 < units) {
                uint32_t low = static_cast<uint32_t>(bytes[at + 2 * i + 2] << 8 | bytes[at + 2 * i + 3]);
                if (low >= 0xdc00 && low < 0xe000) {
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    ++i;
                }
            }
            if (code >= 0xd800 && code < 0xe000) code = 0xfffd;
            if (!append_utf8(text, code)) return false;
        }
        out = Plist::string(reinterpret_cast<const char *>(text.data()), text.size());
        return out.ok();
    }

    bool object(uint64_t index, unsigned depth, Plist &out) {
        if (depth > kMaxDepth || reads == 0) return false;
        --reads;
        uint64_t at, marker;
        if (!offset_of(index, at) || !read_be(at, 1, marker)) return false;
        ++at;
        unsigned type = static_cast<unsigned>(marker >> 4), nibble = static_cast<unsigned>(marker & 0x0f);
        uint64_t value, n;
        switch (type) {
        case 0x0:
            if (nibble == 0x0) out = Plist();
            else if (nibble == 0x8 || nibble == 0x9) out = Plist::boolean(nibble == 0x9);
            else return false;
            return true;
        case 0x1: // 1, 2, 4 bytes unsigned; 8 bytes signed
            if (nibble > 3 || !read_be(at, 1u << nibble, value)) return false;
            out = Plist::integer(static_cast<int64_t>(value));
            return true;
        case 0x2:
        case 0x3: { // real; a date is its seconds since 2001-01-01 as a real
            if (nibble != 2 && nibble != 3) return false;
            if ((type == 0x3 && nibble != 3) || !read_be(at, 1u << nibble, value)) return false;
            double real;
            if (nibble == 2) {
                uint32_t bits = static_cast<uint32_t>(value);
                float single;
                memcpy(&single, &bits, sizeof single);
                real = single;
            } else {
                memcpy(&real, &value, sizeof real);
            }
            out = Plist::real(real);
            return true;
        }
        case 0x4:
        case 0x5:
            if (!count(nibble, at, n) || !charge(n)) return false;
            out = type == 0x4 ? Plist::data(bytes + at, static_cast<size_t>(n))
                              : Plist::string(reinterpret_cast<const char *>(bytes + at), static_cast<size_t>(n));
            return out.ok();
        case 0x6:
            if (!count(nibble, at, n) || n > left(at) / 2 || !charge(2 * n)) return false;
            return utf16(at, n, out);
        case 0x8: // UID (keyed archives): its integer value
            if (nibble > 7 || !read_be(at, nibble + 1, value)) return false;
            out = Plist::integer(static_cast<int64_t>(value));
            return true;
        case 0xa:
        case 0xc: 
            if (!count(nibble, at, n) || n > left(at) / ref_width) return false;
            out = Plist::array();
            for (uint64_t i = 0; i < n; ++i) {
                uint64_t ref;
                Plist child;
                if (!read_be(at + i * ref_width, ref_width, ref) || !object(ref, depth + 1, child)) return false;
                out.push(std::move(child));
                if (!out.ok()) return false;
            }
            return true;
        case 0xd:
            if (!count(nibble, at, n) || n > left(at) / (2 * ref_width)) return false;
            out = Plist::dict();
            for (uint64_t i = 0; i < n; ++i) {
                uint64_t key_ref, value_ref;
                Plist key, child;
                if (!read_be(at + i * ref_width, ref_width, key_ref) ||
                    !read_be(at + (n + i) * ref_width, ref_width, value_ref) || !object(key_ref, depth + 1, key) ||
                    !key.is(Plist::Type::String) || !object(value_ref, depth + 1, child))
                    return false;
                out.set(key.as_string(), std::move(child));
                if (!out.ok()) return false;
            }
            return true;
        default:
            return false;
        }
    }
};

} // namespace

bool bplist_encode(const Plist &root, Bytes &out) {
    out.clear();
    if (!root.ok()) return false;
    size_t objects = subtree_objects(root);
    uint64_t *offsets = static_cast<uint64_t *>(malloc(objects * sizeof(uint64_t)));
    if (!offsets) return false;
    Encoder encoder{out, offsets, width_for(objects)};
    bool ok = out.append(kMagic, sizeof kMagic) && encoder.emit(root, 0);
    uint64_t table = out.size();
    unsigned offset_width = width_for(table);
    for (size_t i = 0; ok && i < objects; ++i) ok = append_be(out, offsets[i], offset_width);
    free(offsets);
    uint8_t trailer[32] = {};
    trailer[6] = static_cast<uint8_t>(offset_width);
    trailer[7] = static_cast<uint8_t>(encoder.ref_width);
    put_be(trailer + 8, objects, 8);
    put_be(trailer + 16, 0, 8);
    put_be(trailer + 24, table, 8);
    ok = ok && out.append(trailer, sizeof trailer);
    if (!ok) out.clear();
    return ok;
}

bool bplist_decode(const uint8_t *bytes, size_t length, Plist &out) {
    out = Plist();
    if (length < sizeof kMagic + 32 || memcmp(bytes, kMagic, sizeof kMagic) != 0) return false;
    const uint64_t body_end = length - 32;
    const uint8_t *trailer = bytes + body_end;
    Decoder d{bytes, length, 0, trailer[6], trailer[7], 0, 0, 0};
    if (d.offset_width < 1 || d.offset_width > 8 || d.ref_width < 1 || d.ref_width > 8) return false;
    uint64_t top;
    d.read_be(body_end + 8, 8, d.objects);
    d.read_be(body_end + 16, 8, top);
    d.read_be(body_end + 24, 8, d.table);
    if (d.objects < 1 || d.objects > body_end || top >= d.objects) return false;
    if (d.table < sizeof kMagic || d.table > body_end || d.objects > static_cast<size_t>(body_end - d.table) / d.offset_width) return false;
    uint64_t allowance = static_cast<uint64_t>(length) * 4 + 1024;
    d.reads = std::min(allowance, kMaxReads);
    d.payload = std::min(allowance, kMaxPayload);
    Plist value;
    if (!d.object(top, 0, value)) return false;
    out = std::move(value);
    return true;
}

} // namespace carplay::core
