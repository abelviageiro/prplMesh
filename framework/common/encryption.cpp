/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Copyright (c) 2019 Arnout Vandecappelle (Essensium/Mind)
 *
 * This code is subject to the terms of the BSD+Patent license.
 * See LICENSE file for more details.
 */

#include <mapf/common/encryption.h>
#include <mapf/common/logger.h>

#include <openssl/bn.h>
#include <openssl/dh.h>

namespace mapf {
namespace encryption {

diffie_hellman::diffie_hellman() : m_dh(nullptr), m_pubkey(nullptr)
{
    MAPF_DBG("Generating DH keypair");

    m_dh = DH_new();
    if (m_dh == nullptr) {
        MAPF_ERR("Failed to allocate DH");
        return;
    }

    /**
      * Diffie-Hellman group 5, see RFC3523
      */
    static const uint8_t dh1536_p[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xC9, 0x0F, 0xDA, 0xA2, 0x21, 0x68, 0xC2,
        0x34, 0xC4, 0xC6, 0x62, 0x8B, 0x80, 0xDC, 0x1C, 0xD1, 0x29, 0x02, 0x4E, 0x08, 0x8A, 0x67,
        0xCC, 0x74, 0x02, 0x0B, 0xBE, 0xA6, 0x3B, 0x13, 0x9B, 0x22, 0x51, 0x4A, 0x08, 0x79, 0x8E,
        0x34, 0x04, 0xDD, 0xEF, 0x95, 0x19, 0xB3, 0xCD, 0x3A, 0x43, 0x1B, 0x30, 0x2B, 0x0A, 0x6D,
        0xF2, 0x5F, 0x14, 0x37, 0x4F, 0xE1, 0x35, 0x6D, 0x6D, 0x51, 0xC2, 0x45, 0xE4, 0x85, 0xB5,
        0x76, 0x62, 0x5E, 0x7E, 0xC6, 0xF4, 0x4C, 0x42, 0xE9, 0xA6, 0x37, 0xED, 0x6B, 0x0B, 0xFF,
        0x5C, 0xB6, 0xF4, 0x06, 0xB7, 0xED, 0xEE, 0x38, 0x6B, 0xFB, 0x5A, 0x89, 0x9F, 0xA5, 0xAE,
        0x9F, 0x24, 0x11, 0x7C, 0x4B, 0x1F, 0xE6, 0x49, 0x28, 0x66, 0x51, 0xEC, 0xE4, 0x5B, 0x3D,
        0xC2, 0x00, 0x7C, 0xB8, 0xA1, 0x63, 0xBF, 0x05, 0x98, 0xDA, 0x48, 0x36, 0x1C, 0x55, 0xD3,
        0x9A, 0x69, 0x16, 0x3F, 0xA8, 0xFD, 0x24, 0xCF, 0x5F, 0x83, 0x65, 0x5D, 0x23, 0xDC, 0xA3,
        0xAD, 0x96, 0x1C, 0x62, 0xF3, 0x56, 0x20, 0x85, 0x52, 0xBB, 0x9E, 0xD5, 0x29, 0x07, 0x70,
        0x96, 0x96, 0x6D, 0x67, 0x0C, 0x35, 0x4E, 0x4A, 0xBC, 0x98, 0x04, 0xF1, 0x74, 0x6C, 0x08,
        0xCA, 0x23, 0x73, 0x27, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    static const uint8_t dh1536_g[] = {0x02};

    // Convert binary to BIGNUM format
    if (0 == DH_set0_pqg(m_dh, BN_bin2bn(dh1536_p, sizeof(dh1536_p), nullptr), NULL,
                         BN_bin2bn(dh1536_g, sizeof(dh1536_g), nullptr))) {
        MAPF_ERR("Failed to set DH pqg");
        return;
    }

    // Obtain key pair
    if (0 == DH_generate_key(m_dh)) {
        MAPF_ERR("Failed to generate DH key");
        return;
    }

    const BIGNUM *pub_key;
    DH_get0_key(m_dh, &pub_key, nullptr);

    m_pubkey_length = BN_num_bytes(pub_key);
    m_pubkey        = new uint8_t[m_pubkey_length];
    BN_bn2bin(pub_key, m_pubkey);
}

diffie_hellman::~diffie_hellman()
{
    delete m_pubkey;
    DH_free(m_dh);
}

bool diffie_hellman::compute_key(uint8_t *key, unsigned &key_length, const uint8_t *remote_pubkey,
                                 unsigned remote_pubkey_length)
{
    if (!m_pubkey) {
        return false;
    }

    MAPF_DBG("Computing DH shared key");

    BIGNUM *pub_key = BN_bin2bn(remote_pubkey, remote_pubkey_length, NULL);
    if (pub_key == nullptr) {
        MAPF_ERR("Failed to set DH remote_pub_key");
        return 0;
    }

    // Compute the shared secret and save it in the output buffer
    if ((int)key_length < DH_size(m_dh)) {
        MAPF_ERR("Output buffer for DH shared key to small: ")
            << key_length << " < " << DH_size(m_dh);
        BN_clear_free(pub_key);
        return false;
    }
    int ret = DH_compute_key(key, pub_key, m_dh);
    BN_clear_free(pub_key);
    if (ret < 0) {
        MAPF_ERR("Failed to compute DH shared key");
        return false;
    }
    key_length = (unsigned)ret;
    return true;
}

} // namespace encryption
} // namespace mapf
