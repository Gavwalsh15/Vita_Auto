#include "carplay/mfi_authenticator.h"

#include "logger/vita_log.h"

#include <psp2/kernel/rng.h>

#include <mbedtls/bignum.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace carplay {
namespace {

constexpr size_t MAX_FILE_BYTES = 16 * 1024;

int randomCallback(void*, unsigned char* data, size_t length)
{
    return sceKernelGetRandomNumber(data, length) < 0 ? -1 : 0;
}

bool readFile(const char* path, std::vector<uint8_t>& out)
{
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
        return false;
    out.resize(MAX_FILE_BYTES);
    const size_t got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (got == 0 || got == MAX_FILE_BYTES)
        return false;
    out.resize(got);
    return true;
}

} // namespace

struct MfiAuthenticator::Key {
    Key() { mbedtls_pk_init(&pk); }
    ~Key() { mbedtls_pk_free(&pk); }
    mbedtls_pk_context pk;
};

MfiAuthenticator::~MfiAuthenticator() { delete key_; }

bool MfiAuthenticator::load(const char* directory)
{
    ready_ = false;
    delete key_;
    key_ = nullptr;

    char path[160];
    std::vector<uint8_t> keyFile, certFile;
    std::snprintf(path, sizeof(path), "%s/identity.pk8", directory);
    const bool haveKey = readFile(path, keyFile);
    std::snprintf(path, sizeof(path), "%s/certificate.p7b", directory);
    const bool haveCert = readFile(path, certFile);
    if (!haveKey || !haveCert) {
        std::snprintf(status_, sizeof(status_), "missing %s/identity.pk8 or certificate.p7b", directory);
        return false;
    }

    key_ = new Key;
    const int parsed = mbedtls_pk_parse_key(&key_->pk, keyFile.data(), keyFile.size(), nullptr, 0,
                                            randomCallback, nullptr);
    std::memset(keyFile.data(), 0, keyFile.size());
    if (parsed != 0 || mbedtls_pk_get_type(&key_->pk) != MBEDTLS_PK_ECKEY ||
        mbedtls_pk_get_bitlen(&key_->pk) != 256) {
        std::snprintf(status_, sizeof(status_), "identity.pk8 is not a P-256 key (rc=-0x%04x)",
                      static_cast<unsigned>(-parsed));
        delete key_;
        key_ = nullptr;
        return false;
    }
    if (!certificate_.assign(certFile.data(), certFile.size())) {
        std::snprintf(status_, sizeof(status_), "out of memory");
        return false;
    }

    // Sign a random digest once so a bad key shows up here, not mid-session.
    ready_ = true;
    uint8_t digest[32];
    core::Bytes signature;
    sceKernelGetRandomNumber(digest, sizeof(digest));
    if (sign_challenge(digest, sizeof(digest), signature) != core::AuthResult::Ok) {
        ready_ = false;
        std::snprintf(status_, sizeof(status_), "signing self-test failed (mbedtls -0x%04x)",
                      static_cast<unsigned>(-lastError_));
        return false;
    }
    std::snprintf(status_, sizeof(status_), "identity loaded (certificate %u bytes)",
                  static_cast<unsigned>(certificate_.size()));
    return true;
}

core::AuthResult MfiAuthenticator::protocol_major(int& major)
{
    if (!ready_)
        return core::AuthResult::Unavailable;
    major = 3; // P-256 identities speak MFi authentication 3.0 (SHA-256 digests)
    return core::AuthResult::Ok;
}

core::AuthResult MfiAuthenticator::certificate(core::Bytes& out)
{
    if (!ready_)
        return core::AuthResult::Unavailable;
    return out.assign(certificate_.data(), certificate_.size()) ? core::AuthResult::Ok
                                                                 : core::AuthResult::Failed;
}

core::AuthResult MfiAuthenticator::sign_challenge(const uint8_t* challenge, size_t length,
                                                     core::Bytes& out)
{
    if (!ready_ || key_ == nullptr)
        return core::AuthResult::Unavailable;
    if (length == 0 || length > 64)
        return core::AuthResult::Failed;
    mbedtls_ecp_group group;
    mbedtls_mpi d, r, s;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    uint8_t raw[64];
    int result = mbedtls_ecp_export(mbedtls_pk_ec(key_->pk), &group, &d, nullptr);
    if (result == 0)
        result = mbedtls_ecdsa_sign(&group, &r, &s, &d, challenge, length, randomCallback, nullptr);
    if (result == 0)
        result = mbedtls_mpi_write_binary(&r, raw, 32);
    if (result == 0)
        result = mbedtls_mpi_write_binary(&s, raw + 32, 32);
    mbedtls_ecp_group_free(&group);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    if (result != 0) {
        lastError_ = result;
        return core::AuthResult::Failed;
    }
    return out.assign(raw, sizeof(raw)) ? core::AuthResult::Ok : core::AuthResult::Failed;
}

} // namespace carplay
