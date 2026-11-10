#include "srp.h"

#include <mbedtls/bignum.h>
#include <mbedtls/platform_util.h>

namespace carplay::core {

namespace {

const char kModulusHex[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
    "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
    "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";
const uint8_t kGenerator = 5;

struct Mpi {
    Mpi() { mbedtls_mpi_init(&v); }
    ~Mpi() { mbedtls_mpi_free(&v); }
    Mpi(const Mpi &) = delete;
    Mpi &operator=(const Mpi &) = delete;
    mbedtls_mpi v;
};

struct Group {
    Group() : ok(mbedtls_mpi_read_string(&n.v, 16, kModulusHex) == 0 && mbedtls_mpi_lset(&g.v, kGenerator) == 0) {}
    Mpi n, g;
    bool ok;
};

bool read(Mpi &r, const uint8_t *bytes, size_t length) { return mbedtls_mpi_read_binary(&r.v, bytes, length) == 0; }

bool write(const Mpi &a, uint8_t out[kSrpBytes]) { return mbedtls_mpi_write_binary(&a.v, out, kSrpBytes) == 0; }

void hash_parts(uint8_t out[kSha512Bytes], const uint8_t *a, size_t a_length, const uint8_t *b = nullptr,
                size_t b_length = 0) {
    Sha512 hash;
    hash.update(a, a_length);
    if (b) hash.update(b, b_length);
    hash.finish(out);
}

bool make_verifier(const Group &group, const uint8_t salt[kSrpSaltBytes], const char *user, const char *password,
                   uint8_t out[kSrpBytes]) {
    uint8_t inner[kSha512Bytes], x[kSha512Bytes];
    Sha512 identity;
    identity.update(user);
    identity.update(":");
    identity.update(password);
    identity.finish(inner);
    hash_parts(x, salt, kSrpSaltBytes, inner, sizeof inner);
    Mpi exponent, v;
    const bool ok = read(exponent, x, sizeof x) &&
                    mbedtls_mpi_exp_mod(&v.v, &group.g.v, &exponent.v, &group.n.v, nullptr) == 0 && write(v, out);
    mbedtls_platform_zeroize(x, sizeof x);
    return ok;
}

bool multiplier(const Group &group, Mpi &k) {
    uint8_t n[kSrpBytes], g[kSrpBytes] = {};
    if (!write(group.n, n)) return false;
    g[kSrpBytes - 1] = kGenerator;
    uint8_t digest[kSha512Bytes];
    hash_parts(digest, n, sizeof n, g, sizeof g);
    return read(k, digest, sizeof digest);
}

// K = H(S without leading zero bytes)
bool derive_session_key(const Mpi &s, uint8_t key[kSha512Bytes]) {
    uint8_t bytes[kSrpBytes];
    if (!write(s, bytes)) return false;
    size_t skip = 0;
    while (skip + 1 < kSrpBytes && bytes[skip] == 0) ++skip;
    hash_parts(key, bytes + skip, kSrpBytes - skip);
    mbedtls_platform_zeroize(bytes, sizeof bytes);
    return true;
}

bool client_proof(const Group &group, const uint8_t *user_hash, const uint8_t *salt, const uint8_t *a,
                  const uint8_t *b, const uint8_t *key, uint8_t out[kSha512Bytes]) {
    uint8_t n[kSrpBytes], hn[kSha512Bytes], hg[kSha512Bytes];
    if (!write(group.n, n)) return false;
    hash_parts(hn, n, sizeof n);
    hash_parts(hg, &kGenerator, 1);
    for (size_t i = 0; i < kSha512Bytes; ++i) hn[i] ^= hg[i];
    Sha512 hash;
    hash.update(hn, sizeof hn);
    hash.update(user_hash, kSha512Bytes);
    hash.update(salt, kSrpSaltBytes);
    hash.update(a, kSrpBytes);
    hash.update(b, kSrpBytes);
    hash.update(key, kSha512Bytes);
    hash.finish(out);
    return true;
}

} // namespace

bool SrpServer::start(Random &random, const char *user, const char *password) {
    if (!random.fill(salt_, sizeof salt_) || !random.fill(b_private_, sizeof b_private_)) return false;
    Sha512 hash;
    hash.update(user);
    hash.finish(user_hash_);
    Group group;
    if (!group.ok || !make_verifier(group, salt_, user, password, verifier_)) return false;
    // B = k*v + g^b mod N
    Mpi k, v, b, gb, kv, sum, result;
    return multiplier(group, k) && read(v, verifier_, sizeof verifier_) && read(b, b_private_, sizeof b_private_) &&
           mbedtls_mpi_exp_mod(&gb.v, &group.g.v, &b.v, &group.n.v, nullptr) == 0 &&
           mbedtls_mpi_mul_mpi(&kv.v, &k.v, &v.v) == 0 && mbedtls_mpi_add_mpi(&sum.v, &kv.v, &gb.v) == 0 &&
           mbedtls_mpi_mod_mpi(&result.v, &sum.v, &group.n.v) == 0 && write(result, b_public_);
}

bool SrpServer::verify(const uint8_t *a, size_t a_length, const uint8_t *proof, size_t proof_length,
                       uint8_t out[kSha512Bytes]) {
    if (a_length == 0 || a_length > kSrpBytes || proof_length != kSha512Bytes) return false;
    Group group;
    if (!group.ok) return false;
    uint8_t a_padded[kSrpBytes] = {};
    memcpy(a_padded + kSrpBytes - a_length, a, a_length);
    Mpi a_value, a_reduced;
    if (!read(a_value, a_padded, sizeof a_padded) ||
        mbedtls_mpi_mod_mpi(&a_reduced.v, &a_value.v, &group.n.v) != 0)
        return false;
    if (mbedtls_mpi_cmp_int(&a_reduced.v, 0) == 0) return false; // A mod N == 0 would fix the shared secret
    // S = (A * v^u)^b mod N, u = H(A | B)
    uint8_t u_bytes[kSha512Bytes];
    hash_parts(u_bytes, a_padded, kSrpBytes, b_public_, kSrpBytes);
    Mpi u, v, b, vu, product, base, s;
    if (!read(u, u_bytes, sizeof u_bytes) || !read(v, verifier_, sizeof verifier_) ||
        !read(b, b_private_, sizeof b_private_) ||
        mbedtls_mpi_exp_mod(&vu.v, &v.v, &u.v, &group.n.v, nullptr) != 0 ||
        mbedtls_mpi_mul_mpi(&product.v, &a_reduced.v, &vu.v) != 0 ||
        mbedtls_mpi_mod_mpi(&base.v, &product.v, &group.n.v) != 0 ||
        mbedtls_mpi_exp_mod(&s.v, &base.v, &b.v, &group.n.v, nullptr) != 0)
        return false;
    uint8_t key[kSha512Bytes], expected[kSha512Bytes];
    if (!derive_session_key(s, key) || !client_proof(group, user_hash_, salt_, a_padded, b_public_, key, expected) ||
        !equal_bytes(expected, proof, kSha512Bytes)) {
        mbedtls_platform_zeroize(key, sizeof key);
        return false;
    }
    memcpy(key_, key, sizeof key_);
    mbedtls_platform_zeroize(key, sizeof key);
    // M2 = H(A | M1 | K)
    Sha512 hash;
    hash.update(a_padded, kSrpBytes);
    hash.update(proof, kSha512Bytes);
    hash.update(key_, kSha512Bytes);
    hash.finish(out);
    return true;
}

SrpServer::~SrpServer() {
    mbedtls_platform_zeroize(b_private_, sizeof b_private_);
    mbedtls_platform_zeroize(key_, sizeof key_);
}

} // namespace carplay::core
