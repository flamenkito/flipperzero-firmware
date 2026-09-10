#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if(!(condition)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            abort();                                                              \
        }                                                                         \
    } while(false)

typedef enum {
    FuriHalRtcBootModeNormal = 0,
    FuriHalRtcBootModeDfu,
    FuriHalRtcBootModePreUpdate,
    FuriHalRtcBootModeUpdate,
    FuriHalRtcBootModePostUpdate,
} FuriHalRtcBootMode;

typedef enum {
    FuriHalRtcHeapTrackModeNone = 0,
    FuriHalRtcHeapTrackModeMain,
    FuriHalRtcHeapTrackModeTree,
    FuriHalRtcHeapTrackModeAll,
} FuriHalRtcHeapTrackMode;

typedef enum {
    FuriHalRtcLocaleUnitsMetric = 0,
    FuriHalRtcLocaleUnitsImperial,
} FuriHalRtcLocaleUnits;

typedef enum {
    FuriHalRtcLocaleTimeFormat24h = 0,
    FuriHalRtcLocaleTimeFormat12h,
} FuriHalRtcLocaleTimeFormat;

typedef enum {
    FuriHalRtcLocaleDateFormatDMY = 0,
    FuriHalRtcLocaleDateFormatMDY,
    FuriHalRtcLocaleDateFormatYMD,
} FuriHalRtcLocaleDateFormat;

typedef enum {
    FuriHalRtcLogDeviceUsart = 0,
    FuriHalRtcLogDeviceLpuart,
    FuriHalRtcLogDeviceReserved,
    FuriHalRtcLogDeviceNone,
} FuriHalRtcLogDevice;

typedef enum {
    FuriHalRtcLogBaudRate230400 = 0,
    FuriHalRtcLogBaudRate9600,
    FuriHalRtcLogBaudRate38400,
    FuriHalRtcLogBaudRate57600,
    FuriHalRtcLogBaudRate115200,
    FuriHalRtcLogBaudRate460800,
    FuriHalRtcLogBaudRate921600,
    FuriHalRtcLogBaudRate1843200,
} FuriHalRtcLogBaudRate;

typedef enum {
    FuriHalUsbIdentityLogitech = 0,
    FuriHalUsbIdentityDell = 1,
    FuriHalUsbIdentityCount,
} FuriHalUsbIdentity;

/*
 * Drift tripwire: this mirrors SystemReg in
 * targets/f7/furi_hal/furi_hal_rtc.c:30-42. Keep every member and width aligned.
 */
typedef struct {
    uint8_t log_level    : 4;
    uint8_t usb_identity : 4;
    uint8_t flags;
    FuriHalRtcBootMode boot_mode                 : 4;
    FuriHalRtcHeapTrackMode heap_track_mode      : 2;
    FuriHalRtcLocaleUnits locale_units           : 1;
    FuriHalRtcLocaleTimeFormat locale_timeformat : 1;
    FuriHalRtcLocaleDateFormat locale_dateformat : 2;
    FuriHalRtcLogDevice log_device               : 2;
    FuriHalRtcLogBaudRate log_baud_rate          : 3;
    uint8_t reserved                             : 1;
} SystemReg;

_Static_assert(sizeof(SystemReg) == 4, "SystemReg size mismatch");
_Static_assert(FuriHalUsbIdentityCount <= 16, "USB identity does not fit in SystemReg");

static uint32_t mock_system_register;

static SystemReg system_reg_from_word(uint32_t word) {
    SystemReg data;
    memcpy(&data, &word, sizeof(data));
    return data;
}

static uint32_t system_reg_to_word(const SystemReg* data) {
    uint32_t word;
    memcpy(&word, data, sizeof(word));
    return word;
}

static void mock_set_usb_identity(FuriHalUsbIdentity identity) {
    if(identity >= FuriHalUsbIdentityCount) {
        identity = FuriHalUsbIdentityLogitech;
    }

    SystemReg data = system_reg_from_word(mock_system_register);
    data.usb_identity = identity;
    mock_system_register = system_reg_to_word(&data);
}

static FuriHalUsbIdentity mock_get_usb_identity(void) {
    SystemReg data = system_reg_from_word(mock_system_register);
    FuriHalUsbIdentity identity = data.usb_identity;
    if(identity >= FuriHalUsbIdentityCount) {
        identity = FuriHalUsbIdentityLogitech;
    }
    return identity;
}

static void writing_identity_preserves_every_neighbor(void) {
    const uint32_t original = UINT32_C(0xD5A39C7B);
    const uint32_t identity_mask = UINT32_C(0x000000F0);
    const uint32_t log_level_mask = UINT32_C(0x0000000F);
    const uint32_t flags_mask = UINT32_C(0x0000FF00);
    const uint32_t boot_mode_mask = UINT32_C(0x000F0000);
    const uint32_t byte_three_fields_mask = UINT32_C(0xFF000000);

    mock_system_register = original;
    mock_set_usb_identity(FuriHalUsbIdentityDell);

    REQUIRE(mock_system_register == ((original & ~identity_mask) | UINT32_C(0x10)));
    REQUIRE((mock_system_register & ~identity_mask) == (original & ~identity_mask));
    REQUIRE((mock_system_register & log_level_mask) == (original & log_level_mask));
    REQUIRE((mock_system_register & flags_mask) == (original & flags_mask));
    REQUIRE((mock_system_register & boot_mode_mask) == (original & boot_mode_mask));
    REQUIRE(
        (mock_system_register & byte_three_fields_mask) ==
        (original & byte_three_fields_mask));
}

static void identities_round_trip_and_clamp(void) {
    const uint32_t seed = UINT32_C(0xA5C35A09);
    const uint32_t identity_mask = UINT32_C(0x000000F0);

    mock_system_register = seed;
    mock_set_usb_identity(FuriHalUsbIdentityLogitech);
    REQUIRE(mock_get_usb_identity() == FuriHalUsbIdentityLogitech);
    mock_set_usb_identity(FuriHalUsbIdentityDell);
    REQUIRE(mock_get_usb_identity() == FuriHalUsbIdentityDell);

    for(uint32_t raw = 2; raw <= 15; raw++) {
        mock_system_register = (seed & ~identity_mask) | (raw << 4);
        REQUIRE(mock_get_usb_identity() == FuriHalUsbIdentityLogitech);

        mock_system_register = seed;
        mock_set_usb_identity((FuriHalUsbIdentity)raw);
        REQUIRE(mock_get_usb_identity() == FuriHalUsbIdentityLogitech);
        REQUIRE((mock_system_register & identity_mask) == 0);
    }
}

int main(void) {
    writing_identity_preserves_every_neighbor();
    identities_round_trip_and_clamp();
    return 0;
}
