#ifndef DIFFMONGER_ENCRYPTIONUTIL_HPP
#define DIFFMONGER_ENCRYPTIONUTIL_HPP

#include <diffmonger/util/PidOwner.hpp>
#include <diffmonger/util/PasswordBuffer.hpp>
#include <diffmonger/util/FdOwner.hpp>

#include <span>
#include <memory>

#include <err.h>


namespace diffmonger {

class KeyPair;
class DecryptedKeyPair;

/**
 * Encrypt data from the given input FdOwner to the given output FdOwner using a random symmetric
 * key that is encrypted with the public key of the given KeyPair and returned.
 */
void encryptToFd(std::span<std::unique_ptr<KeyPair> const> keyPairs,
                 FdOwner fromfd, FdOwner tofd);

/**
 * Decrypt data from the given input FdOwner to the given output FdOwner
 * using the public key of the given DecryptedKeyPair.
 */
void decryptFromFd(DecryptedKeyPair const &decryptedKeyPair, FdOwner fromfd, FdOwner tofd);

}

#endif
