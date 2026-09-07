#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define REQUIRE(condition) \
    do {                   \
        if(!(condition)) __builtin_trap(); \
    } while(false)

#include "../../applications_user/pocket_airbridge/airbridge_assets.h"
#include "../../applications_user/pocket_airbridge/airbridge_assets_digest.h"

static void sha256_matches_known_vector(void) {
    static const uint8_t expected[32] = {
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
        0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
        0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
        0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD,
    };
    AirbridgeSha256 context;
    uint8_t digest[32];

    airbridge_sha256_init(&context);
    airbridge_sha256_update(&context, (const uint8_t*)"a", 1);
    airbridge_sha256_update(&context, (const uint8_t*)"bc", 2);
    airbridge_sha256_final(&context, digest);

    REQUIRE(airbridge_digest_matches(digest, expected));
    digest[0] ^= 1U;
    REQUIRE(!airbridge_digest_matches(digest, expected));
}

static void bundle_header_rejects_wrong_magic_and_version(void) {
    uint8_t header[AIRBRIDGE_BUNDLE_HEADER_SIZE + 2U] = {
        'A', 'B', 'N', 'D', AIRBRIDGE_BUNDLE_FORMAT_VERSION, 0, 0, 0,
        AIRBRIDGE_APP_USB_DECOMPRESSED_SIZE & 0xFFU,
        (AIRBRIDGE_APP_USB_DECOMPRESSED_SIZE >> 8U) & 0xFFU,
        (AIRBRIDGE_APP_USB_DECOMPRESSED_SIZE >> 16U) & 0xFFU,
        (AIRBRIDGE_APP_USB_DECOMPRESSED_SIZE >> 24U) & 0xFFU,
        0x1F, 0x8B,
    };

    REQUIRE(
        airbridge_bundle_validate_header(
            header, sizeof(header), AIRBRIDGE_APP_USB_DECOMPRESSED_SIZE) ==
        AirbridgeBundleValid);
    header[0] ^= 1U;
    REQUIRE(
        airbridge_bundle_validate_header(
            header, sizeof(header), AIRBRIDGE_APP_USB_DECOMPRESSED_SIZE) ==
        AirbridgeBundleBadMagic);
    header[0] ^= 1U;
    header[4]++;
    REQUIRE(
        airbridge_bundle_validate_header(
            header, sizeof(header), AIRBRIDGE_APP_USB_DECOMPRESSED_SIZE) ==
        AirbridgeBundleBadVersion);
}

static void generated_digests_match_tracked_assets(void) {
    const struct {
        const char* path;
        const uint8_t* expected;
    } assets[] = {
        {"airbridge/web/bootstrap.js", AIRBRIDGE_BOOTSTRAP_SHA256},
        {"airbridge/dist/app-usb.html.gz", AIRBRIDGE_APP_USB_BUNDLE_SHA256},
    };
    for(size_t asset_index = 0; asset_index < sizeof(assets) / sizeof(assets[0]); asset_index++) {
        FILE* file = fopen(assets[asset_index].path, "rb");
        REQUIRE(file != NULL);
        AirbridgeSha256 context;
        uint8_t buffer[73];
        uint8_t digest[32];
        airbridge_sha256_init(&context);
        size_t read;
        while((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
            airbridge_sha256_update(&context, buffer, read);
        }
        REQUIRE(!ferror(file));
        REQUIRE(fclose(file) == 0);
        airbridge_sha256_final(&context, digest);
        REQUIRE(airbridge_digest_matches(digest, assets[asset_index].expected));
    }
}

int main(void) {
    sha256_matches_known_vector();
    bundle_header_rejects_wrong_magic_and_version();
    generated_digests_match_tracked_assets();
    return 0;
}
