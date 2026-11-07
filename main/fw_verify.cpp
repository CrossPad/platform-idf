#include "fw_verify.h"

#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

#include "fw_sign_pubkey.h"

static constexpr size_t kSha256Len = 32;
static constexpr int kSha256NotSha224 = 0;

bool fw_verify_manifest(const uint8_t* manifest, size_t manifestLen, const uint8_t* sig, size_t sigLen) {
    uint8_t hash[kSha256Len];
    if (!manifest || !sig || sigLen == 0 || mbedtls_sha256(manifest, manifestLen, hash, kSha256NotSha224) != 0)
        return false;
    for (const FwSignKey& key : kFwSignKeys) {
        if (!key.der) continue;
        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);
        const bool ok = mbedtls_pk_parse_public_key(&pk, key.der, key.len) == 0 &&
                        mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA) &&
                        mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, sig, sigLen) == 0;
        mbedtls_pk_free(&pk);
        if (ok) return true;
    }
    return false;
}

int fw_verify_key_count(void) { return FW_SIGN_KEY_COUNT; }
