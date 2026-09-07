from __future__ import annotations

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FAP_SOURCE_DIR = ROOT / "applications_user/pocket_airbridge"
AIRBRIDGE_C_SOURCES = tuple(sorted(FAP_SOURCE_DIR.glob("*.c")))
GAP_SOURCE = ROOT / "targets/f7/ble_glue/gap.c"
BT_SOURCE = ROOT / "applications/services/bt/bt_service/bt.c"
BT_API_SOURCE = ROOT / "applications/services/bt/bt_service/bt_api.c"
BT_HAL_SOURCE = ROOT / "targets/f7/furi_hal/furi_hal_bt.c"
AIRBRIDGE_BLE_SOURCE = ROOT / "applications_user/pocket_airbridge/airbridge_ble.c"
AIRBRIDGE_RELAY_SOURCE = ROOT / "applications_user/pocket_airbridge/airbridge_relay.c"
AIRBRIDGE_TYPING_SOURCE = ROOT / "applications_user/pocket_airbridge/airbridge_typing.c"
AIRBRIDGE_STREAM_SOURCE = ROOT / "applications_user/pocket_airbridge/airbridge_stream.c"
AIRBRIDGE_UI_INTERNAL = ROOT / "applications_user/pocket_airbridge/airbridge_ui_i.h"
AIRBRIDGE_SCREENS_SOURCE = (
    ROOT / "applications_user/pocket_airbridge/airbridge_screens.c"
)
POCKET_APP_SOURCE = ROOT / "applications_user/pocket_airbridge/pocket_airbridge.c"
USB_HAL_SOURCE = ROOT / "targets/f7/furi_hal/furi_hal_usb.c"
APP_CONF = ROOT / "targets/f7/ble_glue/app_conf.h"


class T12StaticInvariantTest(unittest.TestCase):
    def test_status_callback_registration_has_no_immediate_invoke(self) -> None:
        source = BT_API_SOURCE.read_text(encoding="utf-8")
        body = source.split("void bt_set_status_changed_callback(", 1)[1].split("\n}", 1)[0]

        self.assertNotIn("status_changed_cb(bt->status", body)

    def test_exit_callback_detach_drains_via_dispatch_mutex(self) -> None:
        """Unregister must drain in-flight callbacks (bounded), not return early.

        R11 contract: bt_set_status_changed_callback_bounded delegates to
        bt_status_callback_set_bounded, which acquires the recursive dispatch
        mutex BEFORE touching callback slots — so it cannot return while a
        delivery holds dispatch mid-invocation. No api_lock/message_queue
        round-trips, no unbounded waits.
        """
        source = BT_API_SOURCE.read_text(encoding="utf-8")
        body = source.split("bool bt_set_status_changed_callback_bounded(", 1)[1].split(
            "\n}", 1
        )[0]

        self.assertIn("bt_status_callback_set_bounded", body)
        self.assertNotIn("message_queue", body)
        self.assertNotIn("api_lock", body)
        self.assertNotIn("FuriWaitForever", body)

        registration_header = (
            ROOT
            / "applications/services/bt/bt_service/bt_status_registration.h"
        ).read_text(encoding="utf-8")
        helper = registration_header.split(
            "static inline bool bt_status_callback_set_bounded(", 1
        )[1].split("\n}", 1)[0]

        self.assertLess(helper.index("acquire_dispatch"), helper.index("callback_slot ="))
        self.assertIn("release_dispatch", helper)
        self.assertNotIn("FuriWaitForever", helper)

    def test_active_link_disconnect_does_not_hold_profile_reader_across_service_cleanup(
        self,
    ) -> None:
        source = BT_SOURCE.read_text(encoding="utf-8")
        body = source.split("static void bt_gap_profile_disconnected(Bt* bt) {", 1)[1].split(
            "\n}", 1
        )[0]

        self.assertIn("current_profile_is_airbridge", body)
        self.assertNotIn("ble_svc_airbridge_serial_set_callbacks", body)

    def test_ble_mblock_count_uses_computed_expression(self) -> None:
        source = APP_CONF.read_text(encoding="utf-8")

        self.assertNotIn("#define CFG_BLE_MBLOCK_COUNT 96", source)

    def test_active_link_exit_defers_profile_restore(self) -> None:
        source = AIRBRIDGE_BLE_SOURCE.read_text(encoding="utf-8")
        prepare_body = source.split(
            "bool airbridge_ble_prepare_restore(AirbridgeBle* ble, Bt** restore_bt) {", 1
        )[1].split("void airbridge_ble_queue_restore(Bt* bt) {", 1)[0]
        queue_body = source.split("void airbridge_ble_queue_restore(Bt* bt) {", 1)[1]

        self.assertNotIn("bt_disconnect", prepare_body)
        self.assertNotIn("bt_profile_restore_default", prepare_body)
        self.assertNotIn("bt_profile_restore_default(bt)", queue_body)
        self.assertIn("bt_profile_restore_default_async(bt)", queue_body)

    def test_async_profile_restore_has_bounded_enqueue_and_no_wait(self) -> None:
        source = BT_API_SOURCE.read_text(encoding="utf-8")
        body = source.split("bool bt_profile_restore_default_async(Bt* bt) {", 1)[1].split(
            "\n}", 1
        )[0]

        self.assertNotIn("FuriWaitForever", body)
        self.assertNotIn("api_lock", body)

    def test_gap_stop_is_bounded_and_never_frees_a_live_thread(self) -> None:
        source = GAP_SOURCE.read_text(encoding="utf-8")
        body = source.split("bool gap_thread_stop_bounded(void) {", 1)[1].split(
            "void gap_thread_stop(void)", 1
        )[0]

        self.assertNotIn("FuriWaitForever", body)
        self.assertIn("furi_thread_get_state", body)
        self.assertIn("return false", body)
        self.assertLess(body.index("FuriThreadStateStopped"), body.index("furi_thread_free"))

    def test_gap_reload_does_not_delete_the_shared_advertising_timer(self) -> None:
        source = GAP_SOURCE.read_text(encoding="utf-8")
        stop_body = source.split("bool gap_thread_stop_bounded(void) {", 1)[1].split(
            "void gap_thread_stop(void)", 1
        )[0]

        self.assertIn("static FuriTimer* gap_advertise_timer", source)
        self.assertNotIn("furi_timer_free", stop_body)

    def test_gap_stop_never_performs_storage_io(self) -> None:
        source = GAP_SOURCE.read_text(encoding="utf-8")
        stop_body = source.split("bool gap_thread_stop_bounded(void) {", 1)[1].split(
            "void gap_thread_stop(void)", 1
        )[0]

        self.assertNotIn("furi_record_open", stop_body)

    def test_profile_change_failure_is_deferred_for_retry(self) -> None:
        source = BT_SOURCE.read_text(encoding="utf-8")
        body = source.split("static void bt_change_profile(Bt* bt, BtMessage* message) {", 1)[
            1
        ].split("static void bt_close_connection", 1)[0]
        service = source.split("int32_t bt_srv(void* p) {", 1)[1]

        self.assertIn("bt_schedule_profile_retry", body)
        self.assertIn("profile_retry_pending", service)
        self.assertIn("FuriStatusErrorTimeout", service)

    def test_reinit_propagates_gap_stop_failure(self) -> None:
        source = BT_HAL_SOURCE.read_text(encoding="utf-8")
        body = source.split("static ResumablePhaseStepResult furi_hal_bt_reinit_step(", 1)[
            1
        ].split("static bool furi_hal_bt_reinit_bounded(void) {", 1)[0]

        self.assertIn("gap_thread_stop_bounded()", body)
        self.assertIn("ResumablePhaseStepRetry", body)

    def test_exit_usb_restore_is_async_and_bounded(self) -> None:
        relay_source = AIRBRIDGE_RELAY_SOURCE.read_text(encoding="utf-8")
        relay_body = relay_source.split("bool airbridge_relay_restore_usb(", 1)[1].split(
            "\n}", 1
        )[0]
        usb_source = USB_HAL_SOURCE.read_text(encoding="utf-8")
        async_body = usb_source.split("bool furi_hal_usb_set_config_async(", 1)[1].split(
            "\n}", 1
        )[0]

        self.assertIn("furi_hal_usb_set_config_async", relay_body)
        self.assertNotIn("furi_hal_usb_set_config(relay->usb_mode_prev", relay_body)
        self.assertNotIn("api_lock", async_body)
        self.assertNotIn("FuriWaitForever", async_body)

    def test_cooperative_exit_has_no_synchronous_ble_gap_cleanup(self) -> None:
        source = POCKET_APP_SOURCE.read_text(encoding="utf-8")
        teardown = source.split("furi_thread_set_signal_callback(app_thread, NULL, NULL);", 1)[1]

        self.assertNotIn("airbridge_ble_set_hids_adv", teardown)
        self.assertNotIn("airbridge_typing_drain_release", teardown)

    def test_deploy_is_usb_only(self) -> None:
        sources = "\n".join(
            path.read_text(encoding="utf-8") for path in AIRBRIDGE_C_SOURCES
        )

        self.assertNotIn("AirbridgeTypingTransportBle", sources)
        self.assertNotIn("bootstrap-ble.js", sources)
        self.assertNotIn("app-ble.html.gz", sources)
        self.assertNotIn("bt_airbridge_kb_report", sources)
        self.assertNotIn("airbridge_stream_step_ble", sources)

    def test_fap_implementation_modules_stay_below_250_lines(self) -> None:
        oversized = {
            source.name: len(source.read_text(encoding="utf-8").splitlines())
            for source in AIRBRIDGE_C_SOURCES
            if len(source.read_text(encoding="utf-8").splitlines()) >= 250
        }

        self.assertEqual({}, oversized)

    def test_ui_owns_no_domain_module_pointer(self) -> None:
        ui_state = AIRBRIDGE_UI_INTERNAL.read_text(encoding="utf-8")

        for domain_type in (
            "AirbridgeApp*",
            "AirbridgeBle*",
            "AirbridgeConfig*",
            "AirbridgeRelay*",
            "AirbridgeStream*",
            "AirbridgeTyping*",
            "AirbridgeScreen*",
            "AirbridgeError*",
        ):
            self.assertNotIn(domain_type, ui_state)

    def test_screen_state_writes_are_confined_to_screen_policy(self) -> None:
        other_sources = "\n".join(
            source.read_text(encoding="utf-8")
            for source in AIRBRIDGE_C_SOURCES
            if source != AIRBRIDGE_SCREENS_SOURCE
        )
        screen_policy = AIRBRIDGE_SCREENS_SOURCE.read_text(encoding="utf-8")

        self.assertNotIn("AirbridgeScreen*", other_sources)
        self.assertNotIn("->screen =", other_sources)
        self.assertNotIn("screens->current =", other_sources)
        self.assertIn("screens->current =", screen_policy)

    def test_deploy_assets_are_authenticated_before_use(self) -> None:
        typing = AIRBRIDGE_TYPING_SOURCE.read_text(encoding="utf-8")
        stream = AIRBRIDGE_STREAM_SOURCE.read_text(encoding="utf-8")

        self.assertIn("AIRBRIDGE_BOOTSTRAP_SHA256", typing)
        self.assertLess(
            typing.index("airbridge_digest_matches"),
            typing.index("for(size_t offset = 0; offset < typing->bootstrap_len"),
        )
        self.assertIn("airbridge_bundle_validate_header", stream)
        self.assertIn("AIRBRIDGE_APP_USB_BUNDLE_SHA256", stream)
        self.assertIn("AIRBRIDGE_BUNDLE_HEADER_SIZE", stream)

    def test_generated_digest_header_matches_dist_assets(self) -> None:
        """fbt does not track the generated digest header; catch drift here.

        After running build_bundle.py the FAP must be force-rebuilt
        (touch applications_user/pocket_airbridge/airbridge_assets.c) because
        scons does not rescan the generated include.
        """
        import hashlib
        import re

        header = (FAP_SOURCE_DIR / "airbridge_assets_digest.h").read_text(
            encoding="utf-8"
        )

        def header_digest(name: str) -> bytes:
            match = re.search(name + r"\[32\] = \{([^}]+)\}", header)
            self.assertIsNotNone(match, name)
            return bytes(int(token, 16) for token in match.group(1).split(","))

        bundle_path = ROOT / "airbridge/dist/app-usb.html.gz"
        bootstrap_path = ROOT / "airbridge/web/bootstrap.js"
        self.assertEqual(
            header_digest("AIRBRIDGE_APP_USB_BUNDLE_SHA256"),
            hashlib.sha256(bundle_path.read_bytes()).digest(),
        )
        self.assertEqual(
            header_digest("AIRBRIDGE_BOOTSTRAP_SHA256"),
            hashlib.sha256(bootstrap_path.read_bytes()).digest(),
        )

    def test_profile_restore_is_queued_only_after_fap_cleanup(self) -> None:
        app_source = POCKET_APP_SOURCE.read_text(encoding="utf-8")
        ble_source = AIRBRIDGE_BLE_SOURCE.read_text(encoding="utf-8")

        self.assertIn("bool airbridge_ble_prepare_restore", ble_source)
        self.assertNotIn("bt_profile_restore_default_async(ble->bt)", ble_source)
        self.assertLess(
            ble_source.index("bt_set_status_changed_callback_bounded("),
            ble_source.index("furi_record_close(RECORD_BT)"),
        )

        app_free = app_source.index("free(app);")
        restore_queue = app_source.index("airbridge_ble_queue_restore(bt)")
        self.assertLess(app_free, restore_queue)


if __name__ == "__main__":
    _ = unittest.main()
