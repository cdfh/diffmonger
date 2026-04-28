#ifndef DIFFMONGER_KEYPAIR_HPP
#define DIFFMONGER_KEYPAIR_HPP

#include <diffmonger/util/PasswordBuffer.hpp>
#include <diffmonger/util/array.hpp>

#include <sodium/crypto_box.h>
#include <sodium/crypto_aead_xchacha20poly1305.h>
#include <sodium/crypto_pwhash.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <algorithm>
#include <span>
#include <vector>
#include <cassert>


namespace diffmonger {

class DecryptedKeyPair;

class KeyPair
{
public:
    auto const &getPubKey() const { return pubKey.payload; }
    auto const &getPrivKeyRepresentation() const { return privKeyRepresentation.payload; }

    static KeyPair deserialise(std::span<std::byte const> serialised);

    std::vector<std::byte> serialise() const;

    std::vector<std::byte> additional_data() const;

    DecryptedKeyPair decrypt(PasswordBuffer const &passphrase) const;
private:
    struct PubKey
    { std::array<unsigned char, crypto_box_PUBLICKEYBYTES> payload; };

    struct PrivKeyRepresentation
    {
        std::array<unsigned char,
                   crypto_box_SECRETKEYBYTES + crypto_aead_xchacha20poly1305_IETF_ABYTES>
        payload;
    };

    friend class DecryptedKeyPair;

    KeyPair() = default;

    void check() const;

    std::array<unsigned char, crypto_pwhash_SALTBYTES> salt;
    std::array<unsigned char, crypto_aead_xchacha20poly1305_IETF_NPUBBYTES> nonce;
    size_t opslimit = crypto_pwhash_OPSLIMIT_SENSITIVE;
    size_t memlimit = crypto_pwhash_MEMLIMIT_SENSITIVE;
    int pwhash_algorithm = crypto_pwhash_ALG_ARGON2ID13;
    PubKey pubKey;
    PrivKeyRepresentation privKeyRepresentation;
};

class DecryptedKeyPair
{
public:
    // TODO: Accept opslimit, etc, as params.
    static DecryptedKeyPair generateFromPassphrase(PasswordBuffer const &passphrase);

    DecryptedKeyPair(KeyPair keyPair, PasswordBuffer const &passphrase);

    KeyPair const &getKeyPair() const { return keyPair; }

    PasswordBuffer const &getPrivateKeyPlaintext() const { return *privkey_plaintext; }

private:

    DecryptedKeyPair() {}

    KeyPair keyPair;
    std::unique_ptr<PasswordBuffer> privkey_plaintext =
        std::make_unique<PasswordBuffer>(crypto_box_SECRETKEYBYTES);
};

}

#endif
