#include "crypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/chachapoly.h>
// constant_time.h has no extern "C" guard of its own.
extern "C" {
#include <mbedtls/constant_time.h>
}
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

namespace carplay::core {

Sha512::Sha512() {
    mbedtls_sha512_init(&context_);
    mbedtls_sha512_starts(&context_, 0);
}

Sha512::~Sha512() { mbedtls_sha512_free(&context_); }

void Sha512::update(const uint8_t *data, size_t length) { mbedtls_sha512_update(&context_, data, length); }

void Sha512::finish(uint8_t digest[kSha512Bytes]) { mbedtls_sha512_finish(&context_, digest); }

Sha1::Sha1() {
    mbedtls_sha1_init(&context_);
    mbedtls_sha1_starts(&context_);
}

Sha1::~Sha1() { mbedtls_sha1_free(&context_); }

void Sha1::update(const uint8_t *data, size_t length) { mbedtls_sha1_update(&context_, data, length); }

void Sha1::finish(uint8_t digest[kSha1Bytes]) { mbedtls_sha1_finish(&context_, digest); }

Sha256::Sha256() {
    mbedtls_sha256_init(&context_);
    mbedtls_sha256_starts(&context_, 0);
}

Sha256::~Sha256() { mbedtls_sha256_free(&context_); }

void Sha256::update(const uint8_t *data, size_t length) { mbedtls_sha256_update(&context_, data, length); }

void Sha256::finish(uint8_t digest[kSha256Bytes]) { mbedtls_sha256_finish(&context_, digest); }

void sha512(const uint8_t *data, size_t length, uint8_t digest[kSha512Bytes]) {
    mbedtls_sha512(data, length, digest, 0);
}

void hkdf_sha512(const uint8_t *ikm, size_t ikm_length, const char *salt, const char *info, uint8_t *out,
                 size_t length) {
    mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA512), reinterpret_cast<const uint8_t *>(salt),
                 strlen(salt), ikm, ikm_length, reinterpret_cast<const uint8_t *>(info), strlen(info), out, length);
}

namespace {

class ChaChaPoly {
public:
    explicit ChaChaPoly(const uint8_t key[32]) {
        mbedtls_chachapoly_init(&context_);
        ok_ = mbedtls_chachapoly_setkey(&context_, key) == 0;
    }
    ~ChaChaPoly() { mbedtls_chachapoly_free(&context_); }
    ChaChaPoly(const ChaChaPoly &) = delete;
    ChaChaPoly &operator=(const ChaChaPoly &) = delete;

    bool ok() const { return ok_; }
    mbedtls_chachapoly_context *get() { return &context_; }

private:
    mbedtls_chachapoly_context context_;
    bool ok_;
};

} // namespace

bool aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_length,
               const uint8_t *plain, size_t length, Bytes &out) {
    ChaChaPoly cipher(key);
    if (!cipher.ok()) return false;
    const size_t start = out.size();
    uint8_t *sealed = out.extend(length + kAeadTagBytes);
    if (!sealed) return false;
    if (mbedtls_chachapoly_encrypt_and_tag(cipher.get(), length, nonce, aad, aad_length, plain, sealed,
                                           sealed + length) != 0) {
        out.truncate(start);
        return false;
    }
    return true;
}

bool aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *aad, size_t aad_length,
               const uint8_t *sealed, size_t length, Bytes &out) {
    if (length < kAeadTagBytes) return false;
    const size_t cipher_length = length - kAeadTagBytes;
    ChaChaPoly cipher(key);
    if (!cipher.ok()) return false;
    const size_t start = out.size();
    uint8_t empty;
    uint8_t *plain = cipher_length ? out.extend(cipher_length) : &empty;
    if (!plain) return false;
    if (mbedtls_chachapoly_auth_decrypt(cipher.get(), cipher_length, nonce, aad, aad_length,
                                        sealed + cipher_length, sealed, plain) != 0) {
        out.truncate(start);
        return false;
    }
    return true;
}

void nonce_label(const char *label, uint8_t nonce[12]) {
    memset(nonce, 0, 12);
    for (int i = 0; i < 8 && label[i]; ++i) nonce[4 + i] = static_cast<uint8_t>(label[i]);
}

void nonce_counter(uint64_t counter, uint8_t nonce[12]) {
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; ++i) nonce[4 + i] = static_cast<uint8_t>(counter >> (8 * i));
}

bool equal_bytes(const uint8_t *a, const uint8_t *b, size_t length) { return mbedtls_ct_memcmp(a, b, length) == 0; }

void aes128_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, size_t length, uint8_t *out) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    uint8_t counter[16], stream[16];
    memcpy(counter, iv, sizeof counter);
    size_t offset = 0;
    mbedtls_aes_crypt_ctr(&aes, length, &offset, counter, stream, in, out);
    mbedtls_aes_free(&aes);
    mbedtls_platform_zeroize(stream, sizeof stream);
}

namespace {

typedef int64_t gf[16];

const gf kGf0 = {0};
const gf kGf1 = {1};
const gf k121665 = {0xdb41, 1};
const gf kD = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
               0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
const gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
const gf kX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
               0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
const gf kY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
               0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
const gf kI = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
               0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
const int64_t kL[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                        0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

void set(gf r, const gf a) {
    for (int i = 0; i < 16; ++i) r[i] = a[i];
}

void carry(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += 1 << 16;
        int64_t c = o[i] >> 16;
        if (i < 15) o[i + 1] += c - 1;
        else o[0] += 38 * (c - 1);
        o[i] -= c * 65536;
    }
}

void select(gf p, gf q, int64_t b) {
    int64_t mask = ~(b - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack(uint8_t out[32], const gf n) {
    gf m, t;
    set(t, n);
    carry(t);
    carry(t);
    carry(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        select(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = static_cast<uint8_t>(t[i] & 0xff);
        out[2 * i + 1] = static_cast<uint8_t>(t[i] >> 8);
    }
}

bool differ(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack(c, a);
    pack(d, b);
    return !equal_bytes(c, d, 32);
}

uint8_t parity(const gf a) {
    uint8_t d[32];
    pack(d, a);
    return d[0] & 1;
}

void unpack(gf o, const uint8_t n[32]) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + (static_cast<int64_t>(n[2 * i + 1]) << 8);
    o[15] &= 0x7fff;
}

void add_gf(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i];
}

void sub_gf(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i];
}

void mul_gf(gf o, const gf a, const gf b) {
    int64_t t[31] = {};
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    carry(o);
    carry(o);
}

void square(gf o, const gf a) { mul_gf(o, a, a); }

void invert(gf o, const gf in) {
    gf c;
    set(c, in);
    for (int a = 253; a >= 0; --a) {
        square(c, c);
        if (a != 2 && a != 4) mul_gf(c, c, in);
    }
    set(o, c);
}

void pow2523(gf o, const gf in) {
    gf c;
    set(c, in);
    for (int a = 250; a >= 0; --a) {
        square(c, c);
        if (a != 1) mul_gf(c, c, in);
    }
    set(o, c);
}

void scalarmult_x(uint8_t q[32], const uint8_t n[32], const uint8_t p[32]) {
    uint8_t z[32];
    memcpy(z, n, 32);
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    gf x, a, b, c, d, e, f;
    unpack(x, p);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        select(a, b, r);
        select(c, d, r);
        add_gf(e, a, c);
        sub_gf(a, a, c);
        add_gf(c, b, d);
        sub_gf(b, b, d);
        square(d, e);
        square(f, a);
        mul_gf(a, c, a);
        mul_gf(c, b, e);
        add_gf(e, a, c);
        sub_gf(a, a, c);
        square(b, a);
        sub_gf(c, d, f);
        mul_gf(a, c, k121665);
        add_gf(a, a, d);
        mul_gf(c, c, a);
        mul_gf(a, d, f);
        mul_gf(d, b, x);
        square(b, e);
        select(a, b, r);
        select(c, d, r);
    }
    invert(c, c);
    mul_gf(a, a, c);
    pack(q, a);
    mbedtls_platform_zeroize(z, sizeof z);
}

void point_add(gf p[4], gf q[4]) {
    gf a, b, c, d, t, e, f, g, h;
    sub_gf(a, p[1], p[0]);
    sub_gf(t, q[1], q[0]);
    mul_gf(a, a, t);
    add_gf(b, p[0], p[1]);
    add_gf(t, q[0], q[1]);
    mul_gf(b, b, t);
    mul_gf(c, p[3], q[3]);
    mul_gf(c, c, kD2);
    mul_gf(d, p[2], q[2]);
    add_gf(d, d, d);
    sub_gf(e, b, a);
    sub_gf(f, d, c);
    add_gf(g, d, c);
    add_gf(h, b, a);
    mul_gf(p[0], e, f);
    mul_gf(p[1], h, g);
    mul_gf(p[2], g, f);
    mul_gf(p[3], e, h);
}

void point_swap(gf p[4], gf q[4], int64_t b) {
    for (int i = 0; i < 4; ++i) select(p[i], q[i], b);
}

void point_pack(uint8_t r[32], gf p[4]) {
    gf tx, ty, zi;
    invert(zi, p[2]);
    mul_gf(tx, p[0], zi);
    mul_gf(ty, p[1], zi);
    pack(r, ty);
    r[31] ^= static_cast<uint8_t>(parity(tx) << 7);
}

void point_mul(gf p[4], gf q[4], const uint8_t s[32]) {
    set(p[0], kGf0);
    set(p[1], kGf1);
    set(p[2], kGf1);
    set(p[3], kGf0);
    for (int i = 255; i >= 0; --i) {
        int64_t b = (s[i / 8] >> (i & 7)) & 1;
        point_swap(p, q, b);
        point_add(q, p);
        point_add(p, p);
        point_swap(p, q, b);
    }
}

void point_base(gf p[4], const uint8_t s[32]) {
    gf q[4];
    set(q[0], kX);
    set(q[1], kY);
    set(q[2], kGf1);
    mul_gf(q[3], kX, kY);
    point_mul(p, q, s);
}

bool point_unpack_negated(gf r[4], const uint8_t p[32]) {
    gf t, chk, num, den, den2, den4, den6;
    set(r[2], kGf1);
    unpack(r[1], p);
    square(num, r[1]);
    mul_gf(den, num, kD);
    sub_gf(num, num, r[2]);
    add_gf(den, r[2], den);
    square(den2, den);
    square(den4, den2);
    mul_gf(den6, den4, den2);
    mul_gf(t, den6, num);
    mul_gf(t, t, den);
    pow2523(t, t);
    mul_gf(t, t, num);
    mul_gf(t, t, den);
    mul_gf(t, t, den);
    mul_gf(r[0], t, den);
    square(chk, r[0]);
    mul_gf(chk, chk, den);
    if (differ(chk, num)) mul_gf(r[0], r[0], kI);
    square(chk, r[0]);
    mul_gf(chk, chk, den);
    if (differ(chk, num)) return false;
    if (parity(r[0]) == (p[31] >> 7)) sub_gf(r[0], kGf0, r[0]);
    mul_gf(r[3], r[0], r[1]);
    return true;
}

void reduce_mod_l(uint8_t r[32], int64_t x[64]) {
    int64_t c;
    int j;
    for (int i = 63; i >= 32; --i) {
        c = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += c - 16 * x[i] * kL[j - (i - 32)];
            c = (x[j] + 128) >> 8;
            x[j] -= c * 256;
        }
        x[j] += c;
        x[i] = 0;
    }
    c = 0;
    for (j = 0; j < 32; ++j) {
        x[j] += c - (x[31] >> 4) * kL[j];
        c = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; ++j) x[j] -= c * kL[j];
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = static_cast<uint8_t>(x[i] & 255);
    }
}

void reduce64(uint8_t r[64]) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = r[i];
    memset(r, 0, 64);
    reduce_mod_l(r, x);
}

void expand_seed(const uint8_t seed[32], uint8_t d[64]) {
    sha512(seed, 32, d);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;
}

} // namespace

bool x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    scalarmult_x(out, scalar, point);
    uint8_t any = 0;
    for (int i = 0; i < 32; ++i) any |= out[i];
    return any != 0;
}

void x25519_public(uint8_t out[32], const uint8_t scalar[32]) {
    static const uint8_t kBase[32] = {9};
    scalarmult_x(out, scalar, kBase);
}

void ed25519_public(uint8_t public_key[32], const uint8_t seed[32]) {
    uint8_t d[64];
    expand_seed(seed, d);
    gf p[4];
    point_base(p, d);
    point_pack(public_key, p);
    mbedtls_platform_zeroize(d, sizeof d);
}

void ed25519_sign(uint8_t signature[64], const uint8_t seed[32], const uint8_t *message, size_t length) {
    uint8_t d[64], r[64], h[64], public_key[32];
    expand_seed(seed, d);
    ed25519_public(public_key, seed);
    Sha512 nonce;
    nonce.update(d + 32, 32);
    nonce.update(message, length);
    nonce.finish(r);
    reduce64(r);
    gf p[4];
    point_base(p, r);
    point_pack(signature, p);
    Sha512 challenge;
    challenge.update(signature, 32);
    challenge.update(public_key, 32);
    challenge.update(message, length);
    challenge.finish(h);
    reduce64(h);
    int64_t x[64] = {};
    for (int i = 0; i < 32; ++i) x[i] = r[i];
    for (int i = 0; i < 32; ++i)
        for (int j = 0; j < 32; ++j) x[i + j] += h[i] * static_cast<int64_t>(d[j]);
    reduce_mod_l(signature + 32, x);
    mbedtls_platform_zeroize(d, sizeof d);
    mbedtls_platform_zeroize(r, sizeof r);
}

bool ed25519_verify(const uint8_t signature[64], const uint8_t public_key[32], const uint8_t *message,
                    size_t length) {
    for (int i = 31; i >= 0; --i) {
        if (signature[32 + i] < kL[i]) break;
        if (signature[32 + i] > kL[i] || i == 0) return false;
    }
    gf q[4], p[4];
    if (!point_unpack_negated(q, public_key)) return false;
    uint8_t h[64];
    Sha512 challenge;
    challenge.update(signature, 32);
    challenge.update(public_key, 32);
    challenge.update(message, length);
    challenge.finish(h);
    reduce64(h);
    point_mul(p, q, h);
    point_base(q, signature + 32);
    point_add(p, q);
    uint8_t t[32];
    point_pack(t, p);
    return equal_bytes(t, signature, 32);
}

} // namespace carplay::core
