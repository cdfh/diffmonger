#ifndef DIFFMONGER_KEYPAIR_HPP
#define DIFFMONGER_KEYPAIR_HPP

#include <diffmonger/util/PasswordBuffer.hpp>
#include <diffmonger/util/Uuid.hpp>
#include <diffmonger/util/Serialisation.hpp>

#include <sodium/crypto_box.h>
#include <sodium/crypto_aead_xchacha20poly1305.h>
#include <sodium/crypto_pwhash.h>

#include <array>
#include <cstring>
#include <span>
#include <vector>
#include <cassert>


namespace diffmonger {

class DecryptedKeyPair;

/**
 * Parameters for KDF.
 * Note: no point in attempting to validate members;
 * sodium doesn't provide a robust way of doing this and instead performs
 * its own validation inside crypto_pwhash_*() implementations
 * (see, e.g., crypto_pwhash/argon2/pwhash_argon2id.c).
 */
struct KdfParams
{
#if 0
    struct OpsLimit { size_t value; };
    struct MemLimit { size_t value; };
    struct PasswordHashAlgorithm { int value; };

    KdfParams(OpsLimit opslimit,
              MemLimit memlimit,
              PasswordHashAlgorithm passwordHashAlgorithm);

    auto getOpsLimit() const { return opsLimit; }
    auto getMemLimit() const { return memLimit; }
    auto getPasswordHashAlgorithm() const { return passwordHashAlgorithm; }
private:
    OpsLimit opsLimit;
    MemLimit memLimit;
    PasswordHashAlgorithm passwordHashAlgorithm;
#else
    size_t opslimit = crypto_pwhash_OPSLIMIT_SENSITIVE;
    size_t memlimit = crypto_pwhash_MEMLIMIT_SENSITIVE;
    int passwordHashAlgorithm = crypto_pwhash_ALG_ARGON2ID13;
#endif
};

template <>
struct serialisation::Codec<KdfParams>;

class KeyPair;

template <>
struct serialisation::Codec<KeyPair>
{
    static void serialise(Serialiser &serialiser, KeyPair const &keyPair);
    static KeyPair deserialise(Deserialiser &deserialiser);
};

class EncryptionKey
{
public:
    struct Salt
    {
        std::array<unsigned char, crypto_pwhash_SALTBYTES> value;
        static Salt create();
    };
    EncryptionKey(PasswordBuffer const &password,
                  Salt const &salt,
                  KdfParams const &params);
    EncryptionKey(PasswordBuffer const &password, KeyPair const &keyPair);

    auto const &getKeyNotPassword() const { return keyNotPassword; }
    auto const &getSalt() const { return salt; }
    auto const &getKdfParams() const { return kdfParams; }

    static constexpr size_t size = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;
private:
    /**
     * The result of running the KDF on the passphrase.
     */
    PasswordBuffer keyNotPassword;
    Salt salt;
    KdfParams kdfParams;
};

class KeyPair
{
public:
    using KeyIdType = std::array<unsigned char, crypto_pwhash_SALTBYTES>;

    auto const &getPubKey() const { return pubKey.payload; }
    auto const &getPrivKeyRepresentation() const { return privKeyRepresentation.payload; }

    KeyIdType const &getKeyId() const { return salt.value; }
    auto const &getKdfParams() const { return kdfParams; }
    auto const &getSalt() const { return salt; }

    static KeyPair deserialise(std::span<std::byte const> serialised);

    std::vector<std::byte> serialise() const;

    DecryptedKeyPair decrypt(EncryptionKey const &key) const;

    friend serialisation::Codec<KeyPair>;
private:
    struct PubKey
    { std::array<unsigned char, crypto_box_PUBLICKEYBYTES> payload; };

    struct PrivKeyRepresentation
    {
        static constexpr size_t size =
            crypto_box_SECRETKEYBYTES + crypto_aead_xchacha20poly1305_IETF_ABYTES;
        std::array<unsigned char, size> payload;
    };

    friend class DecryptedKeyPair;

    EncryptionKey::Salt salt;
    struct Nonce
    {
        std::array<unsigned char, crypto_aead_xchacha20poly1305_IETF_NPUBBYTES> value;
        static Nonce create();
    } nonce;
    KdfParams kdfParams;
    PubKey pubKey;
    PrivKeyRepresentation privKeyRepresentation;
    Uuid version;

    KeyPair(EncryptionKey::Salt const &salt,
            Nonce const &nonce,
            KdfParams const &kdfParams,
            PubKey const &pubKey,
            PrivKeyRepresentation const &privKeyRepresentation,
            Uuid const &version);
};

class DecryptedKeyPair
{
public:
    static DecryptedKeyPair generateFromKey(EncryptionKey const &passphrase);
    static DecryptedKeyPair generateFromPassphrase(KdfParams const &params,
                                                   PasswordBuffer const &passphrase);

    KeyPair const &getKeyPair() const { return keyPair; }

    PasswordBuffer const &getPrivateKeyPlaintext() const { return *privKeyPlaintext.payload; }

    DecryptedKeyPair(KeyPair const &keyPair, EncryptionKey const &encryptionKey);
    DecryptedKeyPair(KeyPair const &keyPair, PasswordBuffer const &password);

private:

    struct PrivKeyPlaintext
    {
        static constexpr size_t size = crypto_box_SECRETKEYBYTES;
        std::unique_ptr<PasswordBuffer> payload =
            std::make_unique<PasswordBuffer>(size);
        PrivKeyPlaintext() {}
        PrivKeyPlaintext(PrivKeyPlaintext &&) = default;
        PrivKeyPlaintext &operator=(PrivKeyPlaintext &&) = default;
    };

    DecryptedKeyPair(KeyPair const &keyPair, PrivKeyPlaintext privkey);

    static std::vector<std::byte> aeadAdditionalData(
        EncryptionKey::Salt const &salt,
        KdfParams const &kdfParams,
        KeyPair::Nonce const &nonce,
        KeyPair::PubKey const &pubkey,
        Uuid const &version);

    KeyPair keyPair;
    PrivKeyPlaintext privKeyPlaintext;
};

}

#endif
