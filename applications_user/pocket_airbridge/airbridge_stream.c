#include "airbridge_stream.h"

#include <furi.h>
#include <furi_hal_usb_airbridge.h>
#include <furi_hal_usb_hid.h>

#include <bt/bt_service/bt.h>

void airbridge_stream_init(
    AirbridgeStream* stream,
    Storage* storage,
    AirbridgeTypingTransport* transport,
    AirbridgeScreen* screen,
    AirbridgeStreamShowError show_error,
    AirbridgeApp* app) {
    stream->file = storage_file_alloc(storage);
    stream->transport = transport;
    stream->screen = screen;
    stream->show_error = show_error;
    stream->app = app;
}

void airbridge_stream_deinit(AirbridgeStream* stream) {
    storage_file_free(stream->file);
}

void airbridge_stream_close(AirbridgeStream* stream) {
    if(stream->open) {
        storage_file_close(stream->file);
        stream->open = false;
    }
    stream->pending_len = 0;
}

bool airbridge_stream_start(AirbridgeStream* stream) {
    const bool use_ble_bundle = *stream->transport == AirbridgeTypingTransportBle;
    const char* bundle_path = use_ble_bundle ? APP_DATA_PATH("app-ble.html.gz") :
                                               APP_DATA_PATH("app-usb.html.gz");
    const char* missing_error = use_ble_bundle ? "NO app-ble.gz ON SD" : "NO app-usb.gz ON SD";
    const char* too_large_error = use_ble_bundle ? "app-ble.gz TOO LARGE" : "app-usb.gz TOO LARGE";
    const char* read_error = use_ble_bundle ? "app-ble.gz READ ERROR" : "app-usb.gz READ ERROR";
    if(!storage_file_open(stream->file, bundle_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(stream->file);
        stream->show_error(stream->app, missing_error);
        return false;
    }
    stream->open = true;

    uint64_t file_size = storage_file_size(stream->file);
    if(file_size > BUNDLE_MAX_SIZE) {
        airbridge_stream_close(stream);
        stream->show_error(stream->app, too_large_error);
        return false;
    }

    uint8_t buffer[HID_VENDOR_PACKET_LEN];
    uint32_t checksum = 0;
    size_t read = 0;
    while((read = storage_file_read(stream->file, buffer, sizeof(buffer))) > 0) {
        for(size_t index = 0; index < read; index++) {
            checksum += buffer[index];
        }
    }
    if(!storage_file_seek(stream->file, 0, true)) {
        airbridge_stream_close(stream);
        stream->show_error(stream->app, read_error);
        return false;
    }

    stream->total_len = file_size;
    stream->checksum = checksum;
    stream->sent = 0;
    stream->header_pending = true;
    stream->tx_strikes = 0;
    stream->pending_len = 0;
    stream->started_tick = furi_get_tick();
    *stream->screen = AirbridgeScreenStreaming;
    return true;
}

void airbridge_stream_step_usb(AirbridgeStream* stream) {
    uint8_t report[HID_VENDOR_PACKET_LEN] = {0};
    if(stream->header_pending) {
        report[0] = stream->total_len & 0xFF;
        report[1] = (stream->total_len >> 8) & 0xFF;
        report[2] = (stream->total_len >> 16) & 0xFF;
        report[3] = (stream->total_len >> 24) & 0xFF;
        report[4] = stream->checksum & 0xFF;
        report[5] = (stream->checksum >> 8) & 0xFF;
        report[6] = (stream->checksum >> 16) & 0xFF;
        report[7] = (stream->checksum >> 24) & 0xFF;
        if(!furi_hal_hid_vendor_send_response_blocking(
               report, HID_VENDOR_PACKET_LEN, STREAM_TIMEOUT_MS)) {
            airbridge_stream_close(stream);
            stream->show_error(stream->app, "STREAM ERROR");
            return;
        }
        stream->header_pending = false;
        return;
    }

    if(stream->sent == stream->total_len) {
        airbridge_stream_close(stream);
        *stream->screen = AirbridgeScreenDone;
        stream->done_since = furi_get_tick();
        return;
    }

    uint64_t remaining = stream->total_len - stream->sent;
    size_t expected = (size_t)MIN(remaining, sizeof(report));
    size_t read = storage_file_read(stream->file, report, expected);
    if(read != expected || !furi_hal_hid_vendor_send_response_blocking(
                               report, HID_VENDOR_PACKET_LEN, STREAM_TIMEOUT_MS)) {
        airbridge_stream_close(stream);
        stream->show_error(stream->app, "STREAM ERROR");
        return;
    }
    stream->sent += read;
}

void airbridge_stream_step_ble(AirbridgeStream* stream) {
    if(stream->header_pending) {
        uint8_t header[8] = {
            stream->total_len & 0xFF,
            (stream->total_len >> 8) & 0xFF,
            (stream->total_len >> 16) & 0xFF,
            (stream->total_len >> 24) & 0xFF,
            stream->checksum & 0xFF,
            (stream->checksum >> 8) & 0xFF,
            (stream->checksum >> 16) & 0xFF,
            (stream->checksum >> 24) & 0xFF,
        };
        if(bt_serial_tx(header, sizeof(header))) {
            stream->header_pending = false;
            stream->tx_strikes = 0;
        } else {
            stream->tx_strikes++;
            if(stream->tx_strikes >= BLE_STREAM_RETRY_MAX) {
                airbridge_stream_close(stream);
                stream->show_error(stream->app, "STREAM STALLED");
            }
        }
        return;
    }

    if(stream->sent == stream->total_len) {
        airbridge_stream_close(stream);
        *stream->screen = AirbridgeScreenDone;
        stream->done_since = furi_get_tick();
        return;
    }

    if(stream->pending_len == 0) {
        uint64_t remaining = stream->total_len - stream->sent;
        size_t expected = (size_t)MIN(remaining, sizeof(stream->pending_chunk));
        stream->pending_len = storage_file_read(stream->file, stream->pending_chunk, expected);
        if(stream->pending_len != expected) {
            airbridge_stream_close(stream);
            stream->show_error(stream->app, "STREAM ERROR");
            return;
        }
    }

    if(!bt_serial_tx(stream->pending_chunk, stream->pending_len)) {
        stream->tx_strikes++;
        if(stream->tx_strikes >= BLE_STREAM_RETRY_MAX) {
            airbridge_stream_close(stream);
            stream->show_error(stream->app, "STREAM STALLED");
        }
        return;
    }
    stream->sent += stream->pending_len;
    stream->pending_len = 0;
    stream->tx_strikes = 0;
}
