#pragma once
#include <stddef.h>
#include <stdint.h>

/* True when `sig` (DER ECDSA P-256) over the exact bytes of `manifest`
 * verifies with one of the keys in main/fw_sign_pubkey.h. The manifest
 * carries every file's SHA-256, so this one check covers a package. */
bool fw_verify_manifest(const uint8_t *manifest, size_t manifestLen, const uint8_t *sig, size_t sigLen);

/* Keys this image trusts. 0 until a maintainer adds one: every package is refused. */
int fw_verify_key_count(void);
