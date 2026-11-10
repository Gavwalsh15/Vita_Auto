#pragma once

#include "airplay.h"
#include "auth.h"
#include "crypto.h"
#include "srp.h"

namespace carplay::core {

class Tlv8Writer {
public:
    explicit Tlv8Writer(Bytes &out) : out_(out) {}
    Tlv8Writer &add(uint8_t type, const uint8_t *value, size_t length);
    Tlv8Writer &add(uint8_t type, uint8_t value) { return add(type, &value, 1); }
    bool ok() const { return ok_; }

private:
    Bytes &out_;
    int last_type_ = -1;
    bool ok_ = true;
};

bool tlv8_find(const uint8_t *data, size_t length, uint8_t type, Bytes &value);

struct AirPlayIdentity {
    uint8_t seed[32] = {};
    uint8_t public_key[32] = {}; 
    char pairing_id[37] = {};
    bool generate(Random &random);
    void public_key_hex(char out[65]) const;
};

class PairingStore {
public:
    bool save(const uint8_t *id, size_t id_length, const uint8_t key[32]);
    bool find(const uint8_t *id, size_t id_length, uint8_t key[32]) const;
    size_t size() const { return count_; }
    const uint8_t *id(size_t index, size_t &length) const {
        length = entries_[index].id_length;
        return entries_[index].id;
    }
    const uint8_t *key(size_t index) const { return entries_[index].key; }

private:
    struct Entry {
        uint8_t id[64];
        size_t id_length;
        uint8_t key[32];
    };
    Entry entries_[8] = {};
    size_t count_ = 0;
};

bool save_pairing_state(const AirPlayIdentity &identity, const PairingStore &store, Bytes &out);
bool load_pairing_state(const uint8_t *data, size_t length, AirPlayIdentity &identity, PairingStore &store);

class ControlCipher {
public:
    void set_keys(const uint8_t read_key[32], const uint8_t write_key[32]);
    bool ready() const { return ready_; }
    bool decrypt(Bytes &in, Bytes &plain);
    bool encrypt(const uint8_t *data, size_t length, Bytes &out);

private:
    uint8_t read_key_[32] = {}, write_key_[32] = {};
    uint64_t read_counter_ = 0, write_counter_ = 0;
    bool ready_ = false;
};

class PairingSecurity final : public AirPlaySecurity {
public:
    PairingSecurity(const AirPlayIdentity &identity, PairingStore &store, Random &random)
        : identity_(identity), store_(store), random_(random) {}

    const char *name() const override { return "pairing"; }
    int pair_setup(const Bytes &body, Bytes &reply) override;
    int pair_verify(const Bytes &body, Bytes &reply) override;
    int auth_setup(const Bytes &body, Bytes &reply) override;
    bool channel_encrypted() const override { return cipher_.ready(); }
    bool decrypt(Bytes &in, Bytes &plain) override { return cipher_.decrypt(in, plain); }
    bool encrypt(const uint8_t *data, size_t length, Bytes &out) override { return cipher_.encrypt(data, length, out); }

    void set_authenticator(Authenticator *auth) { auth_ = auth; }
    bool paired() const { return setup_done_; }
    bool verified() const { return cipher_.ready(); }
    // The pair-verify X25519 secret: the media and event channel keys derive from it.
    const uint8_t *shared_secret() const { return verify_secret_; }

private:
    int setup_m2(Bytes &reply);
    int setup_m4(const Bytes &body, Bytes &reply);
    int setup_m6(const Bytes &body, Bytes &reply);
    int verify_m2(const Bytes &body, Bytes &reply);
    int verify_m4(const Bytes &body, Bytes &reply);

    const AirPlayIdentity &identity_;
    PairingStore &store_;
    Random &random_;
    SrpServer *srp_ = nullptr;
    uint8_t setup_key_[kSha512Bytes] = {};
    bool setup_proven_ = false, setup_done_ = false;
    uint8_t verify_secret_[32] = {}, verify_key_[32] = {};
    uint8_t own_ephemeral_[32] = {}, peer_ephemeral_[32] = {};
    bool verify_started_ = false;
    Authenticator *auth_ = nullptr;
    ControlCipher cipher_;

public:
    ~PairingSecurity();
};

} // namespace carplay::core
