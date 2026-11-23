// Windows only: installs TF-PSA-Crypto's threading primitives. Called once by
// Init(), before psa_crypto_init().
#pragma once

namespace micnote::crypto {

void InstallThreading();

}  // namespace micnote::crypto
