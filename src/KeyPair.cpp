#include "KeyPair.hpp"

#include <diffmonger/util/Serialisation.hpp>
#include <diffmonger/util/PasswordBuffer.hpp>
#include <diffmonger/util/array.hpp>
#include <diffmonger/util/Uuid.hpp>

#include <sodium/crypto_box.h>
#include <sodium/crypto_aead_xchacha20poly1305.h>
#include <sodium/crypto_pwhash.h>
#include <sodium/randombytes.h>
#include <sodium/core.h>

#include <array>
#include <cstring>
#include <vector>
#include <cassert>


namespace diffmonger {

static Uuid constexpr version{
    make_array_cast<std::byte>(0xee, 0x2c, 0x0c, 0xcd, 0xd0, 0x53, 0x83, 0x8b,
                               0xe9, 0x97, 0x05, 0xd8, 0xfd, 0x1f, 0xfd, 0x61)};


EncryptionKey::EncryptionKey(PasswordBuffer const &password, KeyPair const &keyPair)
    : EncryptionKey{password, keyPair.getSalt(), keyPair.getKdfParams()}
{}


EncryptionKey::EncryptionKey(PasswordBuffer const &password,
                             Salt const &salt,
                             KdfParams const &kdfParams)
    : keyNotPassword(size),
      salt(salt),
      kdfParams(kdfParams)
{
    if (crypto_pwhash(
            keyNotPassword.getWriteUPointer(),
            keyNotPassword.getSize(),
            password.getPointer(), password.getSize(),
            salt.value.data(),
            kdfParams.opslimit,
            kdfParams.memlimit,
            kdfParams.passwordHashAlgorithm) != 0)
    {
        if (errno == EINVAL)
            throw std::runtime_error("Invalid parameters for password hashing");
        throw std::runtime_error("Password hashing failed: out of memory");
    }
}

EncryptionKey::Salt EncryptionKey::Salt::create()
{
    Salt out;
    randombytes_buf(out.value.data(), out.value.size());
    return out;
}

KeyPair::Nonce KeyPair::Nonce::create()
{
    Nonce out;
    randombytes_buf(out.value.data(), out.value.size());
    return out;
}


std::vector<std::byte> KeyPair::serialise() const
{
    std::vector<std::byte> out;
    serialisation::Serialiser{out}.serialise(*this);
    return out;
}


template <>
struct serialisation::Codec<KdfParams>
{
    static void serialise(Serialiser &serialiser, KdfParams const &kdfParams)
    {
        serialiser
            .serialise(kdfParams.opslimit)
            .serialise(kdfParams.memlimit)
            .serialise(kdfParams.passwordHashAlgorithm);
    }

    static KdfParams deserialise(Deserialiser &deserialiser)
    {
        KdfParams out;
        deserialiser
            .deserialise(out.opslimit)
            .deserialise(out.memlimit)
            .deserialise(out.passwordHashAlgorithm);
        return out;
    }
};


void serialisation::Codec<KeyPair>::serialise(Serialiser &serialiser, KeyPair const &keyPair)
{
    serialiser
        .serialise(version)
        .serialise(keyPair.salt.value)
        .serialise(keyPair.nonce.value)
        .serialise(keyPair.kdfParams)
        .serialise(keyPair.pubKey.payload)
        .serialise(keyPair.privKeyRepresentation.payload);
}

KeyPair serialisation::Codec<KeyPair>::deserialise(Deserialiser &deserialiser)
{
    Uuid version;
    EncryptionKey::Salt salt;
    KeyPair::Nonce nonce;
    KdfParams kdfParams;
    KeyPair::PubKey pubKey;
    KeyPair::PrivKeyRepresentation privKeyRepresentation;

    deserialiser
        .deserialise(version)
        .deserialise(salt.value)
        .deserialise(nonce.value)
        .deserialise(kdfParams)
        .deserialise(pubKey.payload)
        .deserialise(privKeyRepresentation.payload);

    if (version != diffmonger::version)
        throw std::runtime_error("Version mismatch");

    return KeyPair{salt, nonce, kdfParams, pubKey, privKeyRepresentation, version};
}


std::vector<std::byte> DecryptedKeyPair::aeadAdditionalData(EncryptionKey::Salt const &salt,
                                                            KdfParams const &kdfParams,
                                                            KeyPair::Nonce const &nonce,
                                                            KeyPair::PubKey const &pubkey,
                                                            Uuid const &version)
{
    std::vector<std::byte> out;
    serialisation::Serialiser(out)
        .serialise(version.serialised())
        .serialise(salt.value)
        .serialise(nonce.value)
        .serialise(kdfParams.opslimit)
        .serialise(kdfParams.memlimit)
        .serialise(kdfParams.passwordHashAlgorithm)
        .serialise(pubkey.payload);
    return out;
}


DecryptedKeyPair KeyPair::decrypt(EncryptionKey const &encryptionKey) const
{
    return DecryptedKeyPair({ *this }, encryptionKey);
}


KeyPair::KeyPair(EncryptionKey::Salt const &salt,
                 Nonce const &nonce,
                 KdfParams const &kdfParams,
                 PubKey const &pubKey,
                 PrivKeyRepresentation const &privKeyRepresentation,
                 Uuid const &version)
    : salt(salt),
      nonce(nonce),
      kdfParams(kdfParams),
      pubKey(pubKey),
      privKeyRepresentation(privKeyRepresentation),
      version(version)
{
}


DecryptedKeyPair DecryptedKeyPair::generateFromPassphrase(KdfParams const &kdfParams,
                                                          PasswordBuffer const &password)
{
    return generateFromKey(EncryptionKey{password,
                                         EncryptionKey::Salt::create(),
                                         kdfParams});
}


DecryptedKeyPair DecryptedKeyPair::generateFromKey(EncryptionKey const &key)
{
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium init failed");

    KeyPair::PubKey pubKey;
    PrivKeyPlaintext privKeyPlaintext;

    if (crypto_box_keypair(pubKey.payload.data(),
                           reinterpret_cast<unsigned char *>(
                               privKeyPlaintext.payload->getWritePointer())) != 0)
        throw std::runtime_error("Failed to create keypair");

    static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES >= crypto_pwhash_BYTES_MIN);
    static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES <= crypto_pwhash_BYTES_MAX);

    auto nonce = KeyPair::Nonce::create();

    auto const additional_data =
        aeadAdditionalData(key.getSalt(), key.getKdfParams(), nonce, pubKey, version);

    KeyPair::PrivKeyRepresentation privkey;

    crypto_aead_xchacha20poly1305_ietf_encrypt(
        privkey.payload.data(),
        nullptr,
        privKeyPlaintext.payload->getUPointer(),
        privKeyPlaintext.payload->getSize(),
        reinterpret_cast<unsigned char const *>(additional_data.data()),
        additional_data.size(),
        nullptr,
        nonce.value.data(),
        key.getKeyNotPassword().getUPointer());

    return DecryptedKeyPair{
        KeyPair{key.getSalt(), nonce, key.getKdfParams(), pubKey, privkey, version},
        std::move(privKeyPlaintext)};
}


DecryptedKeyPair::DecryptedKeyPair(KeyPair const &keyPair, PasswordBuffer const &password)
    : DecryptedKeyPair{keyPair, EncryptionKey{password, keyPair}}
{
}

DecryptedKeyPair::DecryptedKeyPair(KeyPair const &_keyPair,
                                   EncryptionKey const &encryptionKey)
    : keyPair(_keyPair)
{
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium init failed");

    static_assert(EncryptionKey::size >= crypto_pwhash_BYTES_MIN);
    static_assert(EncryptionKey::size <= crypto_pwhash_BYTES_MAX);

    static_assert(decltype(keyPair.privKeyRepresentation)::size ==
                  (decltype(privKeyPlaintext)::size +
                   crypto_aead_xchacha20poly1305_IETF_ABYTES));

    auto const additional_data = aeadAdditionalData(keyPair.salt,
                                                    keyPair.getKdfParams(),
                                                    keyPair.nonce,
                                                    keyPair.pubKey,
                                                    keyPair.version);

    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            privKeyPlaintext.payload->getWriteUPointer(),
            nullptr,
            nullptr,
            keyPair.privKeyRepresentation.payload.data(),
            keyPair.privKeyRepresentation.payload.size(),
            reinterpret_cast<unsigned char const *>(additional_data.data()),
            additional_data.size(),
            keyPair.nonce.value.data(),
            encryptionKey.getKeyNotPassword().getUPointer()) != 0)
        throw std::runtime_error("Decryption failure");
}

DecryptedKeyPair::DecryptedKeyPair(KeyPair const &keyPair, PrivKeyPlaintext privKeyPlaintext)
    : keyPair(keyPair),
      privKeyPlaintext(std::move(privKeyPlaintext))
{
}


}
