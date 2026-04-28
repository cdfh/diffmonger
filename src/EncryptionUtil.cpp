#include "EncryptionUtil.hpp"
#include "KeyPair.hpp"

#include <diffmonger/util/FdOwner.hpp>
#include <diffmonger/util/PidOwner.hpp>
#include <diffmonger/util/PasswordBuffer.hpp>
#include <diffmonger/util/ioutil.hpp>
#include <diffmonger/util/finally.hpp>
#include <diffmonger/util/Serialisation.hpp>

#include <err.h>

#include <sodium.h>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <span>
#include <array>

namespace diffmonger {

using NbytesT = uint32_t;
// Note: data is sent to the driver via a pipe. While there is a benefit
// in increasing chunk size to reduce the number of syscalls,
// be aware that pipes have a finite underlying buffer (65,536 bytes on Linux),
// and so increasing chunk size beyond this value is unlikely to produce
// meaningful gains in performance. Further, there may be a benefit to
// staying below half the buffer size to reduce blocking and the corresponding
// context switching. It's interesting to investigate the dependence of
// rate on chunk size by using
//   dd bs=$bs if=/dev/zero of=/dev/null
// as a convenient tool.
// On an Intel Xeon E5-1630 running Linux 6.17.0-19, I found that the bottleneck
// shifts from syscalls to processing around between bs=4096 and bs=8192.
static constexpr size_t plaintextChunkSize = 8192;
static constexpr size_t ciphertextMaxChunkSize =
    plaintextChunkSize + crypto_secretstream_xchacha20poly1305_ABYTES;

void encryptToFd(KeyPair const &keyPair, FdOwner fromfd, FdOwner tofd)
{
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium init failed");

    // Generate a random symmetric key
    PasswordBuffer symKey(crypto_aead_xchacha20poly1305_IETF_KEYBYTES);
    randombytes_buf(symKey.getWriteUPointer(), symKey.getSize());

    // Init secret stream
    crypto_secretstream_xchacha20poly1305_state state{};

    // Encrypt the symmetric key with the recipient's public key
    {
        std::array<unsigned char,
                   crypto_box_SEALBYTES + crypto_aead_xchacha20poly1305_IETF_KEYBYTES>
            encryptedSymKey;

        if (crypto_box_seal(encryptedSymKey.data(), symKey.getUPointer(), symKey.getSize(),
                            keyPair.getPubKey().data()) != 0)
            throw std::runtime_error("Failed to encrypt symmetric key");

        std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> header{};
        if (crypto_secretstream_xchacha20poly1305_init_push(
                &state, header.data(), symKey.getUPointer()) != 0)
            throw std::runtime_error("Failed to init secret stream");

        ioutil::write(tofd, std::as_bytes(std::span(encryptedSymKey)));
        ioutil::write(tofd, std::as_bytes(std::span(header.begin(), header.end())));
    }

    std::vector<std::byte> buffer_in(plaintextChunkSize);
    std::vector<uint8_t> buffer_out(
        buffer_in.size() + crypto_secretstream_xchacha20poly1305_ABYTES);

    ioutil::consume2(
        std::move(fromfd), buffer_in,
        [&] (std::span<std::byte const> const buffer)
        {
            unsigned long long nbytes = 0;
            crypto_secretstream_xchacha20poly1305_push(
                &state, buffer_out.data(), &nbytes,
                reinterpret_cast<unsigned char const *>(buffer.data()), buffer.size(),
                nullptr, 0, 0);

            ioutil::write(tofd, serialise(static_cast<NbytesT>(nbytes)));
            ioutil::write(tofd, std::as_bytes(std::span(buffer_out.data(), nbytes)));
        });

    {
        unsigned long long nbytes = 0;
        crypto_secretstream_xchacha20poly1305_push(
            &state, buffer_out.data(), &nbytes, nullptr, 0, nullptr, 0,
            crypto_secretstream_xchacha20poly1305_TAG_FINAL);
        ioutil::write(tofd, serialise(static_cast<NbytesT>(nbytes)));
        ioutil::write(tofd, std::as_bytes(std::span(buffer_out.data(), nbytes)));
    }
}


void decryptFromFd(DecryptedKeyPair const &decryptedKeyPair, FdOwner fromfd, FdOwner tofd)
{
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium init failed");

    PasswordBuffer symKey(crypto_aead_xchacha20poly1305_IETF_KEYBYTES);

    // Decrypt the session key to symKey
    {
        std::array<unsigned char,
                   crypto_box_SEALBYTES + crypto_aead_xchacha20poly1305_IETF_KEYBYTES>
            encryptedSymKey;

        ioutil::readBytesExact(fromfd, std::as_writable_bytes(std::span(encryptedSymKey)));

        if (crypto_box_seal_open(symKey.getWriteUPointer(),
                                 encryptedSymKey.data(),
                                 encryptedSymKey.size(),
                                 decryptedKeyPair.getKeyPair().getPubKey().data(),
                                 decryptedKeyPair.getPrivateKeyPlaintext().getUPointer())
            != 0)
            throw std::runtime_error("Error decrypting session key");
    }

    crypto_secretstream_xchacha20poly1305_state state{};

    {
        std::array<unsigned char, crypto_secretstream_xchacha20poly1305_HEADERBYTES> header{};

        ioutil::readBytesExact(fromfd, std::as_writable_bytes(std::span(header)));

        if (crypto_secretstream_xchacha20poly1305_init_pull(
                &state, header.data(), symKey.getUPointer()) != 0)
            throw std::runtime_error("Failed to init secret stream");
    }

    std::vector<uint8_t> buffer_in(ciphertextMaxChunkSize);
    std::vector<uint8_t> buffer_out(plaintextChunkSize);

    unsigned char tag = 0;

    while (tag == 0)
    {
        std::array<std::byte, sizeof(NbytesT)> nbytesbuf;
        ioutil::readBytesExact(fromfd, std::span(nbytesbuf));
        auto const nbytes = std::min(ciphertextMaxChunkSize,
                                     size_t(deserialise<NbytesT>(nbytesbuf)));
        std::span const buf(buffer_in.data(), nbytes);
        ioutil::readBytesExact(fromfd, std::as_writable_bytes(buf));

        unsigned long long mlen = 0;
        if (crypto_secretstream_xchacha20poly1305_pull(
                &state, buffer_out.data(), &mlen, &tag,
                buf.data(), buf.size(),
                nullptr, 0) != 0)
            throw std::runtime_error("Failed to decrypt data");
        ioutil::write(tofd, std::as_bytes(std::span(buffer_out.data(), mlen)));
    }

    if (tag != crypto_secretstream_xchacha20poly1305_TAG_FINAL)
        throw std::runtime_error("Invalid data (tag) while decrypting");
}

}
