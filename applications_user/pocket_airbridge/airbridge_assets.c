#include "airbridge_assets.h"

#include <string.h>

#include "airbridge_assets_digest.h"

static const uint32_t sha256_round_constants[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U,
    0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U,
    0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U,
    0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU,
    0x5B9CCA4FU, 0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

static uint32_t airbridge_sha256_rotate_right(uint32_t value, uint8_t count) {
    return (value >> count) | (value << (32U - count));
}

static void airbridge_sha256_transform(AirbridgeSha256* context) {
    uint32_t words[64];
    for(size_t index = 0; index < 16; index++) {
        const size_t offset = index * 4U;
        words[index] = ((uint32_t)context->block[offset] << 24U) |
                       ((uint32_t)context->block[offset + 1U] << 16U) |
                       ((uint32_t)context->block[offset + 2U] << 8U) |
                       (uint32_t)context->block[offset + 3U];
    }
    for(size_t index = 16; index < 64; index++) {
        const uint32_t s0 = airbridge_sha256_rotate_right(words[index - 15U], 7U) ^
                            airbridge_sha256_rotate_right(words[index - 15U], 18U) ^
                            (words[index - 15U] >> 3U);
        const uint32_t s1 = airbridge_sha256_rotate_right(words[index - 2U], 17U) ^
                            airbridge_sha256_rotate_right(words[index - 2U], 19U) ^
                            (words[index - 2U] >> 10U);
        words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    uint32_t e = context->state[4];
    uint32_t f = context->state[5];
    uint32_t g = context->state[6];
    uint32_t h = context->state[7];
    for(size_t index = 0; index < 64; index++) {
        const uint32_t sum1 = airbridge_sha256_rotate_right(e, 6U) ^
                              airbridge_sha256_rotate_right(e, 11U) ^
                              airbridge_sha256_rotate_right(e, 25U);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temporary1 = h + sum1 + choice + sha256_round_constants[index] + words[index];
        const uint32_t sum0 = airbridge_sha256_rotate_right(a, 2U) ^
                              airbridge_sha256_rotate_right(a, 13U) ^
                              airbridge_sha256_rotate_right(a, 22U);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temporary2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void airbridge_sha256_init(AirbridgeSha256* context) {
    context->state[0] = 0x6A09E667U;
    context->state[1] = 0xBB67AE85U;
    context->state[2] = 0x3C6EF372U;
    context->state[3] = 0xA54FF53AU;
    context->state[4] = 0x510E527FU;
    context->state[5] = 0x9B05688CU;
    context->state[6] = 0x1F83D9ABU;
    context->state[7] = 0x5BE0CD19U;
    context->bit_length = 0;
    context->block_length = 0;
}

void airbridge_sha256_update(AirbridgeSha256* context, const uint8_t* data, size_t length) {
    for(size_t index = 0; index < length; index++) {
        context->block[context->block_length++] = data[index];
        if(context->block_length == sizeof(context->block)) {
            airbridge_sha256_transform(context);
            context->bit_length += 512U;
            context->block_length = 0;
        }
    }
}

void airbridge_sha256_final(AirbridgeSha256* context, uint8_t digest[32]) {
    context->bit_length += context->block_length * 8U;
    context->block[context->block_length++] = 0x80U;
    if(context->block_length > 56U) {
        while(context->block_length < sizeof(context->block)) {
            context->block[context->block_length++] = 0;
        }
        airbridge_sha256_transform(context);
        context->block_length = 0;
    }
    while(context->block_length < 56U) {
        context->block[context->block_length++] = 0;
    }
    for(size_t index = 0; index < 8U; index++) {
        context->block[63U - index] = (uint8_t)(context->bit_length >> (index * 8U));
    }
    airbridge_sha256_transform(context);
    for(size_t index = 0; index < 8U; index++) {
        digest[index * 4U] = (uint8_t)(context->state[index] >> 24U);
        digest[index * 4U + 1U] = (uint8_t)(context->state[index] >> 16U);
        digest[index * 4U + 2U] = (uint8_t)(context->state[index] >> 8U);
        digest[index * 4U + 3U] = (uint8_t)context->state[index];
    }
    memset(context, 0, sizeof(*context));
}

bool airbridge_digest_matches(const uint8_t lhs[32], const uint8_t rhs[32]) {
    uint8_t difference = 0;
    for(size_t index = 0; index < 32U; index++) {
        difference |= lhs[index] ^ rhs[index];
    }
    return difference == 0;
}

AirbridgeBundleValidation airbridge_bundle_validate_header(
    const uint8_t* header,
    size_t header_length,
    uint32_t expected_decompressed_size) {
    if(header_length < AIRBRIDGE_BUNDLE_HEADER_SIZE + 2U) return AirbridgeBundleTooSmall;
    if(memcmp(header, AIRBRIDGE_BUNDLE_MAGIC, sizeof(AIRBRIDGE_BUNDLE_MAGIC)) != 0) {
        return AirbridgeBundleBadMagic;
    }
    if(header[4] != AIRBRIDGE_BUNDLE_FORMAT_VERSION) return AirbridgeBundleBadVersion;
    if(header[5] != 0 || header[6] != 0 || header[7] != 0) return AirbridgeBundleBadReserved;
    const uint32_t decompressed_size = (uint32_t)header[8] | ((uint32_t)header[9] << 8U) |
                                       ((uint32_t)header[10] << 16U) |
                                       ((uint32_t)header[11] << 24U);
    if(decompressed_size == 0 || decompressed_size != expected_decompressed_size) {
        return AirbridgeBundleBadSize;
    }
    if(header[AIRBRIDGE_BUNDLE_HEADER_SIZE] != 0x1FU ||
       header[AIRBRIDGE_BUNDLE_HEADER_SIZE + 1U] != 0x8BU) {
        return AirbridgeBundleBadGzip;
    }
    return AirbridgeBundleValid;
}
