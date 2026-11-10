#include "pairing.h"

namespace carplay::core {

namespace {

enum : uint8_t {
    kTlvMethod = 0x00,
    kTlvIdentifier = 0x01,
    kTlvSalt = 0x02,
    kTlvPublicKey = 0x03,
    kTlvProof = 0x04,
    kTlvEncrypted = 0x05,
    kTlvState = 0x06,
    kTlvError = 0x07,
    kTlvSignature = 0x0a,
};
const uint8_t kErrorAuthentication = 2;
const char kSetupUser[] = "Pair-Setup";
const char kSetupCode[] = "3939";
const size_t kFrameMax = 0x4000;

int error_reply(uint8_t state, Bytes &reply) {
    reply.clear();
    Tlv8Writer(reply).add(kTlvState, state).add(kTlvError, kErrorAuthentication);
    return 200; // pairing errors travel in the TLV, as in DiPlay
}

bool state_of(const Bytes &body, uint8_t &state) {
    Bytes value;
    if (!tlv8_find(body.data(), body.size(), kTlvState, value) || value.size() != 1) return false;
    state = value[0];
    return true;
}

bool find_exact(const Bytes &tlv, uint8_t type, size_t length, Bytes &value) {
    return tlv8_find(tlv.data(), tlv.size(), type, value) && value.size() == length;
}

bool seal(const uint8_t key[32], const char *label, const Bytes &plain, Bytes &out) {
    uint8_t nonce[12];
    nonce_label(label, nonce);
    return aead_seal(key, nonce, nullptr, 0, plain.data(), plain.size(), out);
}

bool open_sealed(const uint8_t key[32], const char *label, const Bytes &sealed, Bytes &out) {
    uint8_t nonce[12];
    nonce_label(label, nonce);
    return aead_open(key, nonce, nullptr, 0, sealed.data(), sealed.size(), out);
}

struct Joined {
    Bytes bytes;
    Joined &add(const uint8_t *data, size_t length) {
        ok = ok && bytes.append(data, length);
        return *this;
    }
    bool ok = true;
};

void wipe(void *p, size_t n) {
    volatile uint8_t *bytes = static_cast<volatile uint8_t *>(p);
    while (n--) *bytes++ = 0;
}

} // namespace

Tlv8Writer &Tlv8Writer::add(uint8_t type, const uint8_t *value, size_t length) {
    if (last_type_ == type) ok_ = ok_ && out_.push(0xff) && out_.push(0);
    size_t offset = 0;
    do {
        size_t take = std::min(length - offset, static_cast<size_t>(255));
        ok_ = ok_ && out_.push(type) && out_.push(static_cast<uint8_t>(take)) && out_.append(value + offset, take);
        offset += take;
    } while (offset < length);
    last_type_ = type;
    return *this;
}

bool tlv8_find(const uint8_t *data, size_t length, uint8_t type, Bytes &value) {
    value.clear();
    bool found = false;
    int last_type = -1;
    size_t last_length = 0;
    for (size_t at = 0; at + 2 <= length;) {
        uint8_t item = data[at];
        size_t size = data[at + 1];
        at += 2;
        if (size > length - at) break;
        if (item == type) {
            if (!(found && last_type == type && last_length == 255)) value.clear(); // a new item replaces the old
            if (!value.append(data + at, size)) return false;
            found = true;
        }
        last_type = item;
        last_length = size;
        at += size;
    }
    return found;
}

bool AirPlayIdentity::generate(Random &random) {
    uint8_t id[16];
    if (!random.fill(seed, sizeof seed) || !random.fill(id, sizeof id)) return false;
    ed25519_public(public_key, seed);
    id[6] = (id[6] & 0x0f) | 0x40; // UUID version 4
    id[8] = (id[8] & 0x3f) | 0x80;
    static const char kHex[] = "0123456789ABCDEF";
    size_t at = 0;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) pairing_id[at++] = '-';
        pairing_id[at++] = kHex[id[i] >> 4];
        pairing_id[at++] = kHex[id[i] & 15];
    }
    pairing_id[at] = 0;
    return true;
}

void AirPlayIdentity::public_key_hex(char out[65]) const {
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = kHex[public_key[i] >> 4];
        out[2 * i + 1] = kHex[public_key[i] & 15];
    }
    out[64] = 0;
}

bool PairingStore::save(const uint8_t *id, size_t id_length, const uint8_t key[32]) {
    if (id_length == 0 || id_length > sizeof entries_[0].id) return false;
    size_t slot = count_;
    for (size_t i = 0; i < count_; ++i)
        if (entries_[i].id_length == id_length && memcmp(entries_[i].id, id, id_length) == 0) slot = i;
    if (slot == sizeof entries_ / sizeof entries_[0]) slot = 0;
    memcpy(entries_[slot].id, id, id_length);
    entries_[slot].id_length = id_length;
    memcpy(entries_[slot].key, key, 32);
    if (slot == count_) ++count_;
    return true;
}

bool PairingStore::find(const uint8_t *id, size_t id_length, uint8_t key[32]) const {
    for (size_t i = 0; i < count_; ++i)
        if (entries_[i].id_length == id_length && memcmp(entries_[i].id, id, id_length) == 0) {
            memcpy(key, entries_[i].key, 32);
            return true;
        }
    return false;
}

namespace {
const uint8_t kStateMagic[8] = {'N', 'B', 'T', 'P', 'A', 'I', 'R', '1'};
const size_t kStateCheck = 16;
} // namespace

bool save_pairing_state(const AirPlayIdentity &identity, const PairingStore &store, Bytes &out) {
    Bytes image;
    uint8_t count = static_cast<uint8_t>(store.size());
    bool ok = image.append(kStateMagic, sizeof kStateMagic) && image.append(identity.seed, 32) &&
              image.append(reinterpret_cast<const uint8_t *>(identity.pairing_id), 36) && image.push(count);
    for (size_t i = 0; ok && i < store.size(); ++i) {
        size_t length = 0;
        const uint8_t *id = store.id(i, length);
        ok = image.push(static_cast<uint8_t>(length)) && image.append(id, length) && image.append(store.key(i), 32);
    }
    uint8_t digest[kSha512Bytes];
    if (!ok) return false;
    sha512(image.data(), image.size(), digest);
    ok = image.append(digest, kStateCheck);
    out = std::move(image);
    return ok;
}

bool load_pairing_state(const uint8_t *data, size_t length, AirPlayIdentity &identity, PairingStore &store) {
    const size_t header = sizeof kStateMagic + 32 + 36 + 1;
    if (length < header + kStateCheck || memcmp(data, kStateMagic, sizeof kStateMagic) != 0) return false;
    uint8_t digest[kSha512Bytes];
    sha512(data, length - kStateCheck, digest);
    if (!equal_bytes(digest, data + length - kStateCheck, kStateCheck)) return false;
    PairingStore loaded;
    size_t at = header, end = length - kStateCheck;
    for (unsigned i = 0; i < data[header - 1]; ++i) {
        if (at >= end) return false;
        size_t id_length = data[at++];
        if (end - at < id_length + 32 || !loaded.save(data + at, id_length, data + at + id_length)) return false;
        at += id_length + 32;
    }
    if (at != end) return false;
    memcpy(identity.seed, data + sizeof kStateMagic, 32);
    ed25519_public(identity.public_key, identity.seed);
    memcpy(identity.pairing_id, data + sizeof kStateMagic + 32, 36);
    identity.pairing_id[36] = 0;
    store = loaded;
    return true;
}

void ControlCipher::set_keys(const uint8_t read_key[32], const uint8_t write_key[32]) {
    memcpy(read_key_, read_key, 32);
    memcpy(write_key_, write_key, 32);
    read_counter_ = write_counter_ = 0;
    ready_ = true;
}

bool ControlCipher::decrypt(Bytes &in, Bytes &plain) {
    size_t at = 0;
    while (in.size() - at >= 2) {
        size_t length = static_cast<size_t>(in[at] | in[at + 1] << 8);
        size_t frame = 2 + length + kAeadTagBytes;
        if (in.size() - at < frame) break;
        uint8_t nonce[12];
        nonce_counter(read_counter_++, nonce);
        if (!aead_open(read_key_, nonce, in.data() + at, 2, in.data() + at + 2, length + kAeadTagBytes, plain))
            return false;
        at += frame;
    }
    Bytes rest;
    if (!rest.assign(in.data() + at, in.size() - at)) return false;
    in = std::move(rest);
    return true;
}

bool ControlCipher::encrypt(const uint8_t *data, size_t length, Bytes &out) {
    size_t offset = 0;
    do {
        size_t take = std::min(length - offset, kFrameMax);
        uint8_t header[2] = {static_cast<uint8_t>(take), static_cast<uint8_t>(take >> 8)};
        uint8_t nonce[12];
        nonce_counter(write_counter_++, nonce);
        if (!out.append(header, 2) || !aead_seal(write_key_, nonce, header, 2, data + offset, take, out)) return false;
        offset += take;
    } while (offset < length);
    return true;
}

PairingSecurity::~PairingSecurity() {
    delete srp_;
    wipe(setup_key_, sizeof setup_key_);
    wipe(verify_secret_, sizeof verify_secret_);
    wipe(verify_key_, sizeof verify_key_);
}

int PairingSecurity::pair_setup(const Bytes &body, Bytes &reply) {
    uint8_t state = 0;
    if (!state_of(body, state)) return error_reply(0, reply);
    switch (state) {
    case 1: return setup_m2(reply);
    case 3: return setup_m4(body, reply);
    case 5: return setup_m6(body, reply);
    default: return error_reply(state, reply);
    }
}

int PairingSecurity::setup_m2(Bytes &reply) {
    delete srp_;
    srp_ = new SrpServer();
    setup_proven_ = false;
    if (!srp_ || !srp_->start(random_, kSetupUser, kSetupCode)) return error_reply(2, reply);
    reply.clear();
    Tlv8Writer writer(reply);
    writer.add(kTlvState, 2).add(kTlvPublicKey, srp_->public_key(), kSrpBytes).add(kTlvSalt, srp_->salt(), kSrpSaltBytes);
    return writer.ok() ? 200 : 500;
}

int PairingSecurity::setup_m4(const Bytes &body, Bytes &reply) {
    Bytes a, proof;
    if (!srp_ || !tlv8_find(body.data(), body.size(), kTlvPublicKey, a) ||
        !tlv8_find(body.data(), body.size(), kTlvProof, proof))
        return error_reply(4, reply);
    uint8_t server_proof[kSha512Bytes];
    if (!srp_->verify(a.data(), a.size(), proof.data(), proof.size(), server_proof)) return error_reply(4, reply);
    memcpy(setup_key_, srp_->session_key(), sizeof setup_key_);
    setup_proven_ = true;
    reply.clear();
    Tlv8Writer writer(reply);
    writer.add(kTlvState, 4).add(kTlvProof, server_proof, sizeof server_proof);
    return writer.ok() ? 200 : 500;
}

int PairingSecurity::setup_m6(const Bytes &body, Bytes &reply) {
    Bytes sealed, sub, id, ltpk, signature;
    if (!setup_proven_ || !tlv8_find(body.data(), body.size(), kTlvEncrypted, sealed)) return error_reply(6, reply);
    uint8_t key[32];
    hkdf_sha512(setup_key_, sizeof setup_key_, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", key, sizeof key);
    if (!open_sealed(key, "PS-Msg05", sealed, sub) || !tlv8_find(sub.data(), sub.size(), kTlvIdentifier, id) ||
        !find_exact(sub, kTlvPublicKey, 32, ltpk) || !find_exact(sub, kTlvSignature, 64, signature))
        return error_reply(6, reply);
    // The controller signs HKDF(K, controller-sign) | its id | its long-term key.
    uint8_t controller_x[32];
    hkdf_sha512(setup_key_, sizeof setup_key_, "Pair-Setup-Controller-Sign-Salt", "Pair-Setup-Controller-Sign-Info",
                controller_x, sizeof controller_x);
    Joined signed_data;
    signed_data.add(controller_x, sizeof controller_x).add(id.data(), id.size()).add(ltpk.data(), ltpk.size());
    if (!signed_data.ok ||
        !ed25519_verify(signature.data(), ltpk.data(), signed_data.bytes.data(), signed_data.bytes.size()) ||
        !store_.save(id.data(), id.size(), ltpk.data()))
        return error_reply(6, reply);

    uint8_t accessory_x[32];
    hkdf_sha512(setup_key_, sizeof setup_key_, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info",
                accessory_x, sizeof accessory_x);
    const uint8_t *own_id = reinterpret_cast<const uint8_t *>(identity_.pairing_id);
    size_t own_id_length = strlen(identity_.pairing_id);
    Joined accessory_data;
    accessory_data.add(accessory_x, sizeof accessory_x).add(own_id, own_id_length).add(identity_.public_key, 32);
    uint8_t accessory_signature[64];
    ed25519_sign(accessory_signature, identity_.seed, accessory_data.bytes.data(), accessory_data.bytes.size());
    Bytes response_sub, response_sealed;
    Tlv8Writer sub_writer(response_sub);
    sub_writer.add(kTlvIdentifier, own_id, own_id_length)
        .add(kTlvPublicKey, identity_.public_key, 32)
        .add(kTlvSignature, accessory_signature, sizeof accessory_signature);
    if (!accessory_data.ok || !sub_writer.ok() || !seal(key, "PS-Msg06", response_sub, response_sealed))
        return error_reply(6, reply);
    reply.clear();
    Tlv8Writer writer(reply);
    writer.add(kTlvState, 6).add(kTlvEncrypted, response_sealed.data(), response_sealed.size());
    setup_done_ = true;
    wipe(key, sizeof key);
    return writer.ok() ? 200 : 500;
}

// CarPlay /auth-setup (MFiSAP), accessory side, as DiPlay's MfiSapAuthSetup:
//   request   [1 byte version = 1][32-byte controller X25519 public key]
//   response  [32-byte accessory X25519 public key][u32 BE certificate length][certificate]
//             [u32 BE signature length][signature], the signature AES-128-CTR encrypted
//   key, iv   the first 16 bytes of SHA-1("AES-KEY" || shared) and SHA-1("AES-IV" || shared)
//   signed    SHA-1 (protocol 2) or SHA-256 (protocol 3) of accessory key || controller key
// The certificate and signature come from the Authenticator, so nothing here is Apple key material.
int PairingSecurity::auth_setup(const Bytes &body, Bytes &reply) {
    reply.clear();
    if (!auth_) return 470;
    if (body.size() != 33 || body[0] != 1) return 400;
    int major = 0;
    Bytes certificate;
    if (auth_->protocol_major(major) != AuthResult::Ok || auth_->certificate(certificate) != AuthResult::Ok ||
        certificate.empty())
        return 470;

    uint8_t secret[32], own_public[32], shared[32];
    if (!random_.fill(secret, sizeof secret)) return 500;
    x25519_public(own_public, secret);
    if (!x25519(shared, secret, body.data() + 1)) return 400;

    uint8_t key_digest[kSha1Bytes], iv_digest[kSha1Bytes];
    Sha1 key_hash;
    key_hash.update("AES-KEY");
    key_hash.update(shared, sizeof shared);
    key_hash.finish(key_digest);
    Sha1 iv_hash;
    iv_hash.update("AES-IV");
    iv_hash.update(shared, sizeof shared);
    iv_hash.finish(iv_digest);

    uint8_t digest[kSha256Bytes];
    size_t digest_length;
    if (major == 2) {
        Sha1 hash;
        hash.update(own_public, 32);
        hash.update(body.data() + 1, 32);
        hash.finish(digest);
        digest_length = kSha1Bytes;
    } else {
        Sha256 hash;
        hash.update(own_public, 32);
        hash.update(body.data() + 1, 32);
        hash.finish(digest);
        digest_length = kSha256Bytes;
    }
    Bytes signature;
    if (auth_->sign_challenge(digest, digest_length, signature) != AuthResult::Ok || signature.empty()) return 470;
    Bytes encrypted;
    for (size_t i = 0; i < signature.size(); ++i) encrypted.push(0);
    aes128_ctr(key_digest, iv_digest, signature.data(), signature.size(), encrypted.data());

    auto be32 = [](Bytes &out, size_t v) {
        out.push(static_cast<uint8_t>(v >> 24)), out.push(static_cast<uint8_t>(v >> 16));
        out.push(static_cast<uint8_t>(v >> 8)), out.push(static_cast<uint8_t>(v));
    };
    if (!reply.append(own_public, 32)) return 500;
    be32(reply, certificate.size());
    if (!reply.append(certificate.data(), certificate.size())) return 500;
    be32(reply, encrypted.size());
    if (!reply.append(encrypted.data(), encrypted.size())) return 500;
    return 200;
}

int PairingSecurity::pair_verify(const Bytes &body, Bytes &reply) {
    uint8_t state = 0;
    if (!state_of(body, state)) return error_reply(0, reply);
    if (state == 1) return verify_m2(body, reply);
    if (state == 3) return verify_m4(body, reply);
    return error_reply(state, reply);
}

int PairingSecurity::verify_m2(const Bytes &body, Bytes &reply) {
    Bytes peer;
    verify_started_ = false;
    if (!find_exact(body, kTlvPublicKey, 32, peer)) return error_reply(2, reply);
    memcpy(peer_ephemeral_, peer.data(), 32);
    uint8_t ephemeral_secret[32], shared[32];
    if (!random_.fill(ephemeral_secret, sizeof ephemeral_secret)) return error_reply(2, reply);
    x25519_public(own_ephemeral_, ephemeral_secret);
    bool agreed = x25519(shared, ephemeral_secret, peer_ephemeral_);
    wipe(ephemeral_secret, sizeof ephemeral_secret);
    if (!agreed) return error_reply(2, reply);
    memcpy(verify_secret_, shared, 32);
    hkdf_sha512(shared, 32, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", verify_key_, 32);

    const uint8_t *own_id = reinterpret_cast<const uint8_t *>(identity_.pairing_id);
    size_t own_id_length = strlen(identity_.pairing_id);
    Joined signed_data;
    signed_data.add(own_ephemeral_, 32).add(own_id, own_id_length).add(peer_ephemeral_, 32);
    uint8_t signature[64];
    ed25519_sign(signature, identity_.seed, signed_data.bytes.data(), signed_data.bytes.size());
    Bytes sub, sealed;
    Tlv8Writer sub_writer(sub);
    sub_writer.add(kTlvIdentifier, own_id, own_id_length).add(kTlvSignature, signature, sizeof signature);
    if (!signed_data.ok || !sub_writer.ok() || !seal(verify_key_, "PV-Msg02", sub, sealed)) return error_reply(2, reply);
    reply.clear();
    Tlv8Writer writer(reply);
    writer.add(kTlvState, 2).add(kTlvPublicKey, own_ephemeral_, 32).add(kTlvEncrypted, sealed.data(), sealed.size());
    verify_started_ = true;
    return writer.ok() ? 200 : 500;
}

int PairingSecurity::verify_m4(const Bytes &body, Bytes &reply) {
    Bytes sealed, sub, id, signature;
    if (!verify_started_ || !tlv8_find(body.data(), body.size(), kTlvEncrypted, sealed) ||
        !open_sealed(verify_key_, "PV-Msg03", sealed, sub) || !tlv8_find(sub.data(), sub.size(), kTlvIdentifier, id) ||
        !find_exact(sub, kTlvSignature, 64, signature))
        return error_reply(4, reply);
    uint8_t controller_key[32];
    if (!store_.find(id.data(), id.size(), controller_key)) return error_reply(4, reply);
    Joined signed_data;
    signed_data.add(peer_ephemeral_, 32).add(id.data(), id.size()).add(own_ephemeral_, 32);
    if (!signed_data.ok ||
        !ed25519_verify(signature.data(), controller_key, signed_data.bytes.data(), signed_data.bytes.size()))
        return error_reply(4, reply);
    // The accessory reads what the controller writes.
    uint8_t read_key[32], write_key[32];
    hkdf_sha512(verify_secret_, 32, "Control-Salt", "Control-Write-Encryption-Key", read_key, 32);
    hkdf_sha512(verify_secret_, 32, "Control-Salt", "Control-Read-Encryption-Key", write_key, 32);
    cipher_.set_keys(read_key, write_key);
    wipe(read_key, sizeof read_key);
    wipe(write_key, sizeof write_key);
    verify_started_ = false;
    reply.clear();
    Tlv8Writer writer(reply);
    writer.add(kTlvState, 4);
    return writer.ok() ? 200 : 500;
}

} // namespace carplay::core
