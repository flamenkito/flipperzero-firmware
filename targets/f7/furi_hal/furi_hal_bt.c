#include "ble_glue.h"
#include "ble_event_thread.h"
#include "bt_radio_start_state.h"
#include <core/check.h>
#include <core/resumable_phase.h>
#include <event_teardown_order.h>
#include <gap.h>
#include <gap_int.h>
#include <furi_hal_bt.h>
#include <furi_ble/profile_interface.h>

#include <ble/ble.h>
#include <interface/patterns/ble_thread/shci/shci.h>

#include <stdbool.h>
#include <stm32wbxx.h>
#include <stm32wbxx_ll_hsem.h>

#include <hsem_map.h>

#include <furi_hal_version.h>
#include <furi_hal_power.h>
#include <furi_hal_bus.c>
#include <services/battery_service.h>
#include <furi.h>

#define TAG "FuriHalBt"

#define furi_hal_bt_DEFAULT_MAC_ADDR {0x6c, 0x7a, 0xd8, 0xac, 0x57, 0x72}

/* Time, in ms, to wait for mode transition before crashing */
#define C2_MODE_SWITCH_TIMEOUT 10000
#define FURI_HAL_BT_REINIT_LOCK_TIMEOUT 1000U

typedef struct {
    FuriMutex* core2_mtx;
    FuriHalBtStack stack;
    ResumablePhaseMachine reinit;
    BtRadioStartState radio_start;
    BleEventTeardownOrder event_teardown;
    bool hardware_enabled;
} FuriHalBt;

typedef enum {
    FuriHalBtReinitPhaseAwaitIdle,
    FuriHalBtReinitPhaseQuiesceEventWorker,
    FuriHalBtReinitPhaseStopProfile,
    FuriHalBtReinitPhaseHciReset,
    FuriHalBtReinitPhaseStopGap,
    FuriHalBtReinitPhaseBleAppDeinit,
    FuriHalBtReinitPhaseReinitC2,
    FuriHalBtReinitPhaseStopGlue,
    FuriHalBtReinitPhaseDelay,
    FuriHalBtReinitPhaseDisableHardware,
    FuriHalBtReinitPhaseInitHardware,
    FuriHalBtReinitPhaseStartRadio,
} FuriHalBtReinitPhase;

typedef struct {
    uint32_t timeout;
} FuriHalBtReinitContext;

typedef struct {
    uint32_t timeout;
} FuriHalBtRadioStartContext;

static FuriHalBt furi_hal_bt = {
    .core2_mtx = NULL,
    .stack = FuriHalBtStackUnknown,
};

static FuriHalBleProfileBase* current_profile = NULL;
static GapConfig current_config = {0};

static bool furi_hal_bt_init_bounded(uint32_t timeout) {
    if(!furi_hal_bt.hardware_enabled) {
        FURI_LOG_I(TAG, "Start BT initialization");
        furi_hal_bus_enable(FuriHalBusHSEM);
        furi_hal_bus_enable(FuriHalBusIPCC);
        furi_hal_bus_enable(FuriHalBusAES2);
        furi_hal_bus_enable(FuriHalBusPKA);
        furi_hal_bus_enable(FuriHalBusCRC);

        if(!furi_hal_bt.core2_mtx) {
            furi_hal_bt.core2_mtx = furi_mutex_alloc(FuriMutexTypeNormal);
            furi_check(furi_hal_bt.core2_mtx);
        }

        furi_check(LL_HSEM_1StepLock(HSEM, CFG_HW_CLK48_CONFIG_SEMID) == 0);
        furi_hal_bt.hardware_enabled = true;
    }

    return ble_glue_init_bounded(timeout);
}

void furi_hal_bt_init(void) {
    furi_check(furi_hal_bt_init_bounded(FuriWaitForever));
}

void furi_hal_bt_lock_core2(void) {
    furi_check(furi_hal_bt.core2_mtx);
    furi_check(furi_mutex_acquire(furi_hal_bt.core2_mtx, FuriWaitForever) == FuriStatusOk);
}

static bool furi_hal_bt_lock_core2_bounded(uint32_t timeout) {
    furi_check(furi_hal_bt.core2_mtx);
    return furi_mutex_acquire(furi_hal_bt.core2_mtx, timeout) == FuriStatusOk;
}

void furi_hal_bt_unlock_core2(void) {
    furi_check(furi_hal_bt.core2_mtx);
    furi_check(furi_mutex_release(furi_hal_bt.core2_mtx) == FuriStatusOk);
}

static bool furi_hal_bt_radio_stack_is_supported(const BleGlueC2Info* info) {
    bool supported = false;
    if(info->StackType == INFO_STACK_TYPE_BLE_LIGHT) {
        if(info->VersionMajor >= FURI_HAL_BT_STACK_VERSION_MAJOR &&
           info->VersionMinor >= FURI_HAL_BT_STACK_VERSION_MINOR) {
            furi_hal_bt.stack = FuriHalBtStackLight;
            supported = true;
        }
    } else if(info->StackType == INFO_STACK_TYPE_BLE_FULL) {
        if(info->VersionMajor >= FURI_HAL_BT_STACK_VERSION_MAJOR &&
           info->VersionMinor >= FURI_HAL_BT_STACK_VERSION_MINOR) {
            furi_hal_bt.stack = FuriHalBtStackFull;
            supported = true;
        }
    } else {
        furi_hal_bt.stack = FuriHalBtStackUnknown;
    }
    return supported;
}

static bool furi_hal_bt_radio_start(void* context) {
    UNUSED(context);
    return ble_glue_start();
}

static bool furi_hal_bt_radio_start_quiesce(void* context) {
    FuriHalBtRadioStartContext* radio_context = context;
    return ble_event_thread_quiesce_bounded(radio_context->timeout);
}

static void furi_hal_bt_radio_start_deinit(void* context) {
    UNUSED(context);
    ble_app_deinit();
}

static bool furi_hal_bt_radio_start_stop_glue(void* context) {
    FuriHalBtRadioStartContext* radio_context = context;
    return ble_glue_stop_bounded(radio_context->timeout);
}

static bool furi_hal_bt_start_radio_stack_bounded(uint32_t lock_timeout) {
    if(!furi_hal_bt_lock_core2_bounded(lock_timeout)) return false;

    bool res = false;
    FuriHalBtRadioStartContext radio_context = {.timeout = lock_timeout};
    const BtRadioStartOps radio_ops = {
        .context = &radio_context,
        .start = furi_hal_bt_radio_start,
        .quiesce = furi_hal_bt_radio_start_quiesce,
        .deinit = furi_hal_bt_radio_start_deinit,
        .stop_glue = furi_hal_bt_radio_start_stop_glue,
    };

    if(bt_radio_start_state_cleanup_pending(&furi_hal_bt.radio_start)) {
        (void)bt_radio_start_state_run(&furi_hal_bt.radio_start, &radio_ops);
        furi_hal_bt_unlock_core2();
        return false;
    }

    if(bt_radio_start_state_cleanup_complete(&furi_hal_bt.radio_start)) {
        if(!ble_glue_init_bounded(lock_timeout)) {
            furi_hal_bt_unlock_core2();
            return false;
        }
        bt_radio_start_state_reset(&furi_hal_bt.radio_start);
    }

    // Explicitly tell that we are in charge of CLK48 domain
    furi_check(LL_HSEM_1StepLock(HSEM, CFG_HW_CLK48_CONFIG_SEMID) == 0);

    do {
        // Wait until C2 is started or timeout
        if(!ble_glue_wait_for_c2_start(FURI_HAL_BT_C2_START_TIMEOUT)) {
            FURI_LOG_E(TAG, "Core2 start failed");
            break;
        }

        // If C2 is running, start radio stack fw
        if(!furi_hal_bt_ensure_c2_mode(BleGlueC2ModeStack)) {
            break;
        }

        // Check whether we support radio stack
        const BleGlueC2Info* c2_info = ble_glue_get_c2_info();
        if(!furi_hal_bt_radio_stack_is_supported(c2_info)) {
            FURI_LOG_E(TAG, "Unsupported radio stack");
            // Don't stop SHCI for crypto enclave support
            break;
        }
        // Starting radio stack
        if(!bt_radio_start_state_run(&furi_hal_bt.radio_start, &radio_ops)) {
            FURI_LOG_E(TAG, "Failed to start radio stack");
            break;
        }
        res = true;
    } while(false);

    furi_hal_bt_unlock_core2();

    if(res) gap_extra_beacon_init();
    return res;
}

bool furi_hal_bt_start_radio_stack(void) {
    return furi_hal_bt_start_radio_stack_bounded(FuriWaitForever);
}

FuriHalBtStack furi_hal_bt_get_radio_stack(void) {
    return furi_hal_bt.stack;
}

bool furi_hal_bt_is_gatt_gap_supported(void) {
    if(furi_hal_bt.stack == FuriHalBtStackLight || furi_hal_bt.stack == FuriHalBtStackFull) {
        return true;
    } else {
        return false;
    }
}

bool furi_hal_bt_is_testing_supported(void) {
    if(furi_hal_bt.stack == FuriHalBtStackFull) {
        return true;
    } else {
        return false;
    }
}

bool furi_hal_bt_check_profile_type(
    FuriHalBleProfileBase* profile,
    const FuriHalBleProfileTemplate* profile_template) {
    if(!profile || !profile_template) {
        return false;
    }

    return profile->config == profile_template;
}

FuriHalBleProfileBase* furi_hal_bt_start_app(
    const FuriHalBleProfileTemplate* profile_template,
    FuriHalBleProfileParams params,
    const GapRootSecurityKeys* root_keys,
    GapEventCallback event_cb,
    void* context) {
    furi_check(event_cb);
    furi_check(profile_template);
    furi_check(root_keys);
    furi_check(current_profile == NULL);

    do {
        if(!ble_glue_is_radio_stack_ready()) {
            FURI_LOG_E(TAG, "Can't start BLE App - radio stack did not start");
            break;
        }
        if(!furi_hal_bt_is_gatt_gap_supported()) {
            FURI_LOG_E(TAG, "Can't start Ble App - unsupported radio stack");
            break;
        }

        profile_template->get_gap_config(&current_config, params);

        if(!gap_init(&current_config, root_keys, event_cb, context)) {
            gap_thread_stop();
            FURI_LOG_E(TAG, "Failed to init GAP");
            break;
        }
        // Start selected profile services
        if(furi_hal_bt_is_gatt_gap_supported()) {
            current_profile = profile_template->start(params);
        }
    } while(false);

    return current_profile;
}

static ResumablePhaseStepResult furi_hal_bt_reinit_step(void* context, uint8_t phase) {
    FuriHalBtReinitContext* phase_context = context;
    switch((FuriHalBtReinitPhase)phase) {
    case FuriHalBtReinitPhaseAwaitIdle:
        FURI_LOG_I(TAG, "Disconnect and stop advertising");
        furi_hal_bt_stop_advertising();
        return furi_hal_bt_is_active() ? ResumablePhaseStepRetry : ResumablePhaseStepAdvance;
    case FuriHalBtReinitPhaseQuiesceEventWorker:
        return ble_event_teardown_order_record_quiesce(
                   &furi_hal_bt.event_teardown,
                   ble_event_thread_quiesce_bounded(phase_context->timeout)) ?
                   ResumablePhaseStepAdvance :
                   ResumablePhaseStepRetry;
    case FuriHalBtReinitPhaseStopProfile:
        furi_check(
            ble_event_teardown_order_may_destroy_handlers(&furi_hal_bt.event_teardown));
        if(current_profile) {
            FURI_LOG_I(TAG, "Stop current profile services");
            current_profile->config->stop(current_profile);
            current_profile = NULL;
        }
        ble_event_teardown_order_record_handler_destruction(&furi_hal_bt.event_teardown);
        return ResumablePhaseStepAdvance;
    case FuriHalBtReinitPhaseHciReset:
        hci_reset();
        return ble_app_take_hci_lock_failure() ? ResumablePhaseStepAdvanceAndRetry :
                                                ResumablePhaseStepAdvance;
    case FuriHalBtReinitPhaseStopGap:
        FURI_LOG_I(TAG, "Stop BLE related RTOS threads");
        return gap_thread_stop_bounded() ? ResumablePhaseStepAdvance : ResumablePhaseStepRetry;
    case FuriHalBtReinitPhaseBleAppDeinit:
        furi_check(
            ble_event_teardown_order_may_destroy_handlers(&furi_hal_bt.event_teardown));
        ble_app_deinit();
        return ResumablePhaseStepAdvance;
    case FuriHalBtReinitPhaseReinitC2: {
        FURI_LOG_I(TAG, "Reset SHCI");
        bool completed = false;
        bool clean = ble_glue_reinit_c2_bounded(&completed);
        if(!completed) return ResumablePhaseStepRetry;
        return clean ? ResumablePhaseStepAdvance : ResumablePhaseStepAdvanceAndRetry;
    }
    case FuriHalBtReinitPhaseStopGlue:
        return ble_glue_stop_bounded(phase_context->timeout) ? ResumablePhaseStepAdvance :
                                                              ResumablePhaseStepRetry;
    case FuriHalBtReinitPhaseDelay:
        furi_delay_ms(100);
        return ResumablePhaseStepAdvance;
    case FuriHalBtReinitPhaseDisableHardware:
        furi_hal_bus_disable(FuriHalBusHSEM);
        furi_hal_bus_disable(FuriHalBusIPCC);
        furi_hal_bus_disable(FuriHalBusAES2);
        furi_hal_bus_disable(FuriHalBusPKA);
        furi_hal_bus_disable(FuriHalBusCRC);
        furi_hal_bt.hardware_enabled = false;
        return ResumablePhaseStepAdvance;
    case FuriHalBtReinitPhaseInitHardware:
        return furi_hal_bt_init_bounded(phase_context->timeout) ? ResumablePhaseStepAdvance :
                                                                 ResumablePhaseStepRetry;
    case FuriHalBtReinitPhaseStartRadio:
        return ResumablePhaseStepComplete;
    }
    furi_crash();
}

static bool furi_hal_bt_reinit_bounded(void) {
    furi_hal_power_insomnia_enter();

    if(furi_hal_bt.reinit.phase != FuriHalBtReinitPhaseStartRadio) {
        if(!furi_hal_bt_lock_core2_bounded(FURI_HAL_BT_REINIT_LOCK_TIMEOUT)) {
            furi_hal_power_insomnia_exit();
            return false;
        }

        FuriHalBtReinitContext context = {.timeout = FURI_HAL_BT_REINIT_LOCK_TIMEOUT};
        bool ready_to_start =
            resumable_phase_machine_run(&furi_hal_bt.reinit, furi_hal_bt_reinit_step, &context);
        furi_hal_bt_unlock_core2();
        if(!ready_to_start) {
            furi_hal_power_insomnia_exit();
            return false;
        }
    }

    bool started = furi_hal_bt_start_radio_stack_bounded(FURI_HAL_BT_REINIT_LOCK_TIMEOUT);
    if(started) {
        furi_hal_bt.reinit.phase = FuriHalBtReinitPhaseAwaitIdle;
        furi_hal_bt.event_teardown = ble_event_teardown_order_initial();
    }
    furi_hal_power_insomnia_exit();
    return started;
}

void furi_hal_bt_reinit(void) {
    if(!furi_hal_bt_reinit_bounded()) FURI_LOG_E(TAG, "BT reinit deferred");
}

FuriHalBleProfileBase* furi_hal_bt_change_app(
    const FuriHalBleProfileTemplate* profile_template,
    FuriHalBleProfileParams profile_params,
    const GapRootSecurityKeys* root_keys,
    GapEventCallback event_cb,
    void* context) {
    if(!furi_hal_bt_reinit_bounded()) return NULL;
    return furi_hal_bt_start_app(profile_template, profile_params, root_keys, event_cb, context);
}

bool furi_hal_bt_is_active(void) {
    return gap_get_state() > GapStateIdle;
}

bool furi_hal_bt_is_connected(void) {
    return gap_get_state() == GapStateConnected;
}

void furi_hal_bt_start_advertising(void) {
    if(gap_get_state() == GapStateIdle) {
        gap_start_advertising();
    }
}

void furi_hal_bt_stop_advertising(void) {
    if(furi_hal_bt_is_active()) {
        gap_stop_advertising();
        /* With honest GAP state this genuinely waits for the disconnect-complete
         * after a terminate; bound the wait so a lost HCI event can never wedge
         * the caller (BtSrv) forever. Do NOT force state on timeout. */
        uint32_t waited_ms = 0;
        while(furi_hal_bt_is_active()) {
            if(waited_ms >= 1000) {
                FURI_LOG_E(TAG, "stop_advertising: timed out waiting for GAP idle");
                return;
            }
            furi_delay_tick(1);
            waited_ms++;
        }
    }
}

void furi_hal_bt_set_adv_hids(bool enable) {
    gap_set_adv_hids(enable);
}

void furi_hal_bt_update_battery_level(uint8_t battery_level) {
    ble_svc_battery_state_update(&battery_level, NULL);
}

void furi_hal_bt_update_power_state(bool charging) {
    ble_svc_battery_state_update(NULL, &charging);
}

void furi_hal_bt_get_key_storage_buff(uint8_t** key_buff_addr, uint16_t* key_buff_size) {
    ble_app_get_key_storage_buff(key_buff_addr, key_buff_size);
}

void furi_hal_bt_set_key_storage_change_callback(
    BleGlueKeyStorageChangedCallback callback,
    void* context) {
    furi_check(callback);
    ble_glue_set_key_storage_changed_callback(callback, context);
}

void furi_hal_bt_nvm_sram_sem_acquire(void) {
    while(LL_HSEM_1StepLock(HSEM, CFG_HW_BLE_NVM_SRAM_SEMID)) {
        furi_thread_yield();
    }
}

void furi_hal_bt_nvm_sram_sem_release(void) {
    LL_HSEM_ReleaseLock(HSEM, CFG_HW_BLE_NVM_SRAM_SEMID, 0);
}

bool furi_hal_bt_clear_white_list(void) {
    furi_hal_bt_nvm_sram_sem_acquire();
    tBleStatus status = aci_gap_clear_security_db();
    if(status) {
        FURI_LOG_E(TAG, "Clear while list failed with status %d", status);
    }
    furi_hal_bt_nvm_sram_sem_release();
    return status != BLE_STATUS_SUCCESS;
}

void furi_hal_bt_dump_state(FuriString* buffer) {
    furi_check(buffer);

    if(furi_hal_bt_is_alive()) {
        uint8_t HCI_Version;
        uint16_t HCI_Revision;
        uint8_t LMP_PAL_Version;
        uint16_t Manufacturer_Name;
        uint16_t LMP_PAL_Subversion;

        tBleStatus ret = hci_read_local_version_information(
            &HCI_Version, &HCI_Revision, &LMP_PAL_Version, &Manufacturer_Name, &LMP_PAL_Subversion);

        furi_string_cat_printf(
            buffer,
            "Ret: %d, HCI_Version: %d, HCI_Revision: %d, LMP_PAL_Version: %d, Manufacturer_Name: %d, LMP_PAL_Subversion: %d",
            ret,
            HCI_Version,
            HCI_Revision,
            LMP_PAL_Version,
            Manufacturer_Name,
            LMP_PAL_Subversion);
    } else {
        furi_string_cat_printf(buffer, "BLE not ready");
    }
}

bool furi_hal_bt_is_alive(void) {
    return ble_glue_is_alive();
}

void furi_hal_bt_start_tone_tx(uint8_t channel, uint8_t power) {
    aci_hal_set_tx_power_level(0, power);
    aci_hal_tone_start(channel, 0);
}

void furi_hal_bt_stop_tone_tx(void) {
    aci_hal_tone_stop();
}

void furi_hal_bt_start_packet_tx(uint8_t channel, uint8_t pattern, uint8_t datarate) {
    hci_le_enhanced_transmitter_test(channel, 0x25, pattern, datarate);
}

void furi_hal_bt_start_packet_rx(uint8_t channel, uint8_t datarate) {
    hci_le_enhanced_receiver_test(channel, datarate, 0);
}

uint16_t furi_hal_bt_stop_packet_test(void) {
    uint16_t num_of_packets = 0;
    hci_le_test_end(&num_of_packets);
    return num_of_packets;
}

void furi_hal_bt_start_rx(uint8_t channel) {
    aci_hal_rx_start(channel);
}

float furi_hal_bt_get_rssi(void) {
    float val;
    uint8_t rssi_raw[3];

    if(aci_hal_read_raw_rssi(rssi_raw) != BLE_STATUS_SUCCESS) {
        return 0.0f;
    }

    // Some ST magic with rssi
    uint8_t agc = rssi_raw[2] & 0xFF;
    int rssi = (((int)rssi_raw[1] << 8) & 0xFF00) + (rssi_raw[0] & 0xFF);
    if(rssi == 0 || agc > 11) {
        val = -127.0;
    } else {
        val = agc * 6.0f - 127.0f;
        while(rssi > 30) {
            val += 6.0;
            rssi >>= 1;
        }
        val += (float)((417 * rssi + 18080) >> 10);
    }
    return val;
}

uint32_t furi_hal_bt_get_transmitted_packets(void) {
    uint32_t packets = 0;
    aci_hal_le_tx_test_packet_number(&packets);
    return packets;
}

void furi_hal_bt_stop_rx(void) {
    aci_hal_rx_stop();
}

bool furi_hal_bt_ensure_c2_mode(BleGlueC2Mode mode) {
    BleGlueCommandResult fw_start_res = ble_glue_force_c2_mode(mode);
    if(fw_start_res == BleGlueCommandResultOK) {
        return true;
    } else if(fw_start_res == BleGlueCommandResultRestartPending) {
        // Do nothing and wait for system reset
        furi_delay_ms(C2_MODE_SWITCH_TIMEOUT);
        furi_crash("Waiting for FUS->radio stack transition");
        return true;
    }

    FURI_LOG_E(TAG, "Failed to switch C2 mode: %d", fw_start_res);
    return false;
}

bool furi_hal_bt_extra_beacon_set_data(const uint8_t* data, uint8_t len) {
    return gap_extra_beacon_set_data(data, len);
}

uint8_t furi_hal_bt_extra_beacon_get_data(uint8_t* data) {
    return gap_extra_beacon_get_data(data);
}

bool furi_hal_bt_extra_beacon_set_config(const GapExtraBeaconConfig* config) {
    return gap_extra_beacon_set_config(config);
}

const GapExtraBeaconConfig* furi_hal_bt_extra_beacon_get_config(void) {
    return gap_extra_beacon_get_config();
}

bool furi_hal_bt_extra_beacon_start(void) {
    return gap_extra_beacon_start();
}

bool furi_hal_bt_extra_beacon_stop(void) {
    return gap_extra_beacon_stop();
}

bool furi_hal_bt_extra_beacon_is_active(void) {
    return gap_extra_beacon_get_state() == GapExtraBeaconStateStarted;
}
