#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_length;
    uint8_t block[64];
    size_t block_length;
} AirbridgeSha256;

typedef enum {
    AirbridgeBundleValid,
    AirbridgeBundleTooSmall,
    AirbridgeBundleBadMagic,
    AirbridgeBundleBadVersion,
    AirbridgeBundleBadReserved,
    AirbridgeBundleBadSize,
    AirbridgeBundleBadGzip,
} AirbridgeBundleValidation;

void airbridge_sha256_init(AirbridgeSha256* context);
void airbridge_sha256_update(AirbridgeSha256* context, const uint8_t* data, size_t length);
void airbridge_sha256_final(AirbridgeSha256* context, uint8_t digest[32]);
bool airbridge_digest_matches(const uint8_t lhs[32], const uint8_t rhs[32]);
AirbridgeBundleValidation airbridge_bundle_validate_header(
    const uint8_t* header,
    size_t header_length,
    uint32_t expected_decompressed_size);
