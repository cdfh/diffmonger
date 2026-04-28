#include "KeyPair.hpp"

#include <diffmonger/util/Serialisation.hpp>
#include <diffmonger/util/PasswordBuffer.hpp>
#include <diffmonger/util/array.hpp>

#include <sodium/crypto_box.h>
#include <sodium/crypto_aead_xchacha20poly1305.h>
#include <sodium/crypto_pwhash.h>
#include <sodium/randombytes.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <algorithm>
#include <span>
#include <vector>
#include <cassert>


namespace diffmonger {

static auto constexpr version =
    make_array_cast<uint8_t>(0xee, 0x2c, 0x0c, 0xcd, 0xd0, 0x53, 0x83, 0x8b,
                             0xe9, 0x97, 0x05, 0xd8, 0xfd, 0x1f, 0xfd, 0x61);

KeyPair KeyPair::deserialise(std::span<std::byte const> const serialised)
{
    Deserialiser deserialiser(serialised);

    KeyPair keyPair;

    std::remove_cv_t<decltype(version)> version;

    deserialiser
        .deserialise(version)
        .deserialise(keyPair.salt)
        .deserialise(keyPair.nonce)
        .deserialise(keyPair.opslimit)
        .deserialise(keyPair.memlimit)
        .deserialise(keyPair.pwhash_algorithm)
        .deserialise(keyPair.pubKey.payload)
        .deserialise(keyPair.privKeyRepresentation.payload);

    if (version != diffmonger::version)
        throw std::runtime_error("Version mismatch");

    return keyPair;
}

std::vector<std::byte> KeyPair::serialise() const
{
    std::vector<std::byte> out;
    Serialiser serialiser(out);
    serialiser
        .serialise(version)
        .serialise(salt)
        .serialise(nonce)
        .serialise(opslimit)
        .serialise(memlimit)
        .serialise(pwhash_algorithm)
        .serialise(pubKey.payload)
        .serialise(privKeyRepresentation.payload);
    return out;
}

std::vector<std::byte> KeyPair::additional_data() const
{
    std::vector<std::byte> out;
    Serialiser serialiser(out);
    serialiser
        .serialise(version)
        .serialise(salt)
        .serialise(nonce)
        .serialise(opslimit)
        .serialise(memlimit)
        .serialise(pwhash_algorithm)
        .serialise(pubKey.payload);
    return out;
}

DecryptedKeyPair KeyPair::decrypt(PasswordBuffer const &passphrase) const
{
    return DecryptedKeyPair({ *this }, passphrase);
}


void KeyPair::check() const
{
    if (!((opslimit >= crypto_pwhash_OPSLIMIT_MIN) &&
          (opslimit <= crypto_pwhash_OPSLIMIT_MAX)))
        throw std::runtime_error("Invalid opslimit");
    if (!((memlimit >= crypto_pwhash_MEMLIMIT_MIN) &&
          (opslimit <= crypto_pwhash_MEMLIMIT_MAX)))
        throw std::runtime_error("Invalid memlimit");
}

DecryptedKeyPair DecryptedKeyPair::generateFromPassphrase(PasswordBuffer const &passphrase)
{
    DecryptedKeyPair out{};

    std::array<unsigned char, crypto_box_PUBLICKEYBYTES> &pubKey = out.keyPair.pubKey.payload;
    std::array<unsigned char,
               crypto_box_SECRETKEYBYTES + crypto_aead_xchacha20poly1305_IETF_ABYTES>
        &privkey = out.keyPair.privKeyRepresentation.payload;

    auto &privkey_plaintext = out.privkey_plaintext;

    crypto_box_keypair(pubKey.data(),
                       reinterpret_cast<unsigned char *>(
                           privkey_plaintext->getWritePointer()));

    static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES >= crypto_pwhash_BYTES_MIN);
    static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES <= crypto_pwhash_BYTES_MAX);

    PasswordBuffer key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);

    std::array<unsigned char, crypto_pwhash_SALTBYTES> &salt = out.keyPair.salt;

    randombytes_buf(salt.data(), salt.size());

    if (crypto_pwhash
        (key.getWriteUPointer(), key.getSize(),
         passphrase.getPointer(), passphrase.getSize(),
         salt.data(),
         out.keyPair.opslimit,
         out.keyPair.memlimit,
         out.keyPair.pwhash_algorithm) != 0)
        throw std::runtime_error("Password hashing failed: out of memory");

    std::array<unsigned char, crypto_aead_xchacha20poly1305_IETF_NPUBBYTES> &nonce =
        out.keyPair.nonce;

    randombytes_buf(nonce.data(), nonce.size());

    auto const additional_data = out.keyPair.additional_data();

    crypto_aead_xchacha20poly1305_ietf_encrypt(
        privkey.data(),
        nullptr,
        privkey_plaintext->getUPointer(),
        privkey_plaintext->getSize(),
        reinterpret_cast<unsigned char const *>(additional_data.data()),
        additional_data.size(),
        nullptr,
        nonce.data(),
        key.getUPointer());

    return out;
}

DecryptedKeyPair::DecryptedKeyPair(KeyPair keyPair, PasswordBuffer const &passphrase)
    : keyPair(std::move(keyPair))
{
    PasswordBuffer key(crypto_aead_xchacha20poly1305_ietf_KEYBYTES);

    static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES >= crypto_pwhash_BYTES_MIN);
    static_assert(crypto_aead_xchacha20poly1305_ietf_KEYBYTES <= crypto_pwhash_BYTES_MAX);

    std::array<unsigned char, crypto_pwhash_SALTBYTES> &salt = keyPair.salt;

    if (crypto_pwhash
        (key.getWriteUPointer(), key.getSize(), passphrase.getPointer(), passphrase.getSize(),
         salt.data(),
         keyPair.opslimit,
         keyPair.memlimit,
         keyPair.pwhash_algorithm) != 0)
        throw std::runtime_error("Password hashing failed: out of memory");

    std::array<unsigned char, crypto_aead_xchacha20poly1305_IETF_NPUBBYTES> &nonce = keyPair.nonce;

    if (keyPair.privKeyRepresentation.payload.size() !=
        (privkey_plaintext->getSize() + crypto_aead_xchacha20poly1305_IETF_ABYTES))
        throw std::runtime_error("Unexpected ciphertext length");

    auto const additional_data = keyPair.additional_data();

    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            privkey_plaintext->getWriteUPointer(),
            nullptr,
            nullptr,
            keyPair.privKeyRepresentation.payload.data(),
            keyPair.privKeyRepresentation.payload.size(),
            reinterpret_cast<unsigned char const *>(additional_data.data()),
            additional_data.size(),
            nonce.data(),
            key.getUPointer()) != 0)
        throw std::runtime_error("Decryption failure");
}

}
