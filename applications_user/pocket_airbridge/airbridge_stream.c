#include "airbridge_stream.h"

#include <furi.h>
#include "airbridge_usb.h"
#include <furi_hal_usb_hid.h>

#include "airbridge_assets.h"
#include "airbridge_assets_digest.h"

void airbridge_stream_init(
    AirbridgeStream* stream,
    Storage* storage,
    AirbridgeStreamShowError show_error,
    void* error_context) {
    stream->file = storage_file_alloc(storage);
    stream->show_error = show_error;
    stream->error_context = error_context;
}

void airbridge_stream_deinit(AirbridgeStream* stream) {
    storage_file_free(stream->file);
}

void airbridge_stream_close(AirbridgeStream* stream) {
    if(stream->open) {
        storage_file_close(stream->file);
        stream->open = false;
    }
}

bool airbridge_stream_start(AirbridgeStream* stream) {
    if(!storage_file_open(
           stream->file, APP_DATA_PATH("app-usb.html.gz"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_close(stream->file);
        stream->show_error(stream->error_context, "NO app-usb.gz ON SD");
        return false;
    }
    stream->open = true;

    uint64_t file_size = storage_file_size(stream->file);
    if(file_size > BUNDLE_MAX_SIZE) {
        airbridge_stream_close(stream);
        stream->show_error(stream->error_context, "app-usb.gz TOO LARGE");
        return false;
    }

    uint8_t prefix[AIRBRIDGE_BUNDLE_HEADER_SIZE + 2U];
    if(storage_file_read(stream->file, prefix, sizeof(prefix)) != sizeof(prefix)) {
        airbridge_stream_close(stream);
        stream->show_error(stream->error_context, "app-usb.gz BAD SIZE");
        return false;
    }
    const AirbridgeBundleValidation validation = airbridge_bundle_validate_header(
        prefix, sizeof(prefix), AIRBRIDGE_APP_USB_DECOMPRESSED_SIZE);
    if(validation != AirbridgeBundleValid) {
        const char* message = "app-usb.gz BAD HEADER";
        switch(validation) {
        case AirbridgeBundleBadMagic:
            message = "app-usb.gz BAD MAGIC";
            break;
        case AirbridgeBundleBadVersion:
            message = "app-usb.gz BAD VERSION";
            break;
        case AirbridgeBundleBadGzip:
            message = "app-usb.gz NOT GZIP";
            break;
        case AirbridgeBundleTooSmall:
        case AirbridgeBundleBadReserved:
        case AirbridgeBundleBadSize:
        case AirbridgeBundleValid:
            break;
        }
        airbridge_stream_close(stream);
        stream->show_error(stream->error_context, message);
        return false;
    }
    if(!storage_file_seek(stream->file, 0, true)) {
        airbridge_stream_close(stream);
        stream->show_error(stream->error_context, "app-usb.gz READ ERROR");
        return false;
    }

    uint8_t buffer[HID_VENDOR_PACKET_LEN];
    uint8_t digest[AIRBRIDGE_ASSET_SHA256_SIZE];
    AirbridgeSha256 sha256;
    airbridge_sha256_init(&sha256);
    uint32_t checksum = 0;
    uint64_t total_read = 0;
    size_t read = 0;
    while((read = storage_file_read(stream->file, buffer, sizeof(buffer))) > 0) {
        airbridge_sha256_update(&sha256, buffer, read);
        for(size_t index = 0; index < read; index++) {
            if(total_read + index >= AIRBRIDGE_BUNDLE_HEADER_SIZE) {
                checksum += buffer[index];
            }
        }
        total_read += read;
    }
    airbridge_sha256_final(&sha256, digest);
    if(total_read != file_size) {
        airbridge_stream_close(stream);
        stream->show_error(stream->error_context, "app-usb.gz READ ERROR");
        return false;
    }
    if(!airbridge_digest_matches(digest, AIRBRIDGE_APP_USB_BUNDLE_SHA256)) {
        airbridge_stream_close(stream);
        stream->show_error(stream->error_context, "app-usb.gz HASH MISMATCH");
        return false;
    }
    if(!storage_file_seek(stream->file, AIRBRIDGE_BUNDLE_HEADER_SIZE, true)) {
        airbridge_stream_close(stream);
        stream->show_error(stream->error_context, "app-usb.gz READ ERROR");
        return false;
    }

    stream->total_len = file_size - AIRBRIDGE_BUNDLE_HEADER_SIZE;
    stream->checksum = checksum;
    stream->sent = 0;
    stream->header_pending = true;
    return true;
}

bool airbridge_stream_step_usb(AirbridgeStream* stream) {
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
        if(!airbridge_usb_vendor_send_response_blocking(
               report, HID_VENDOR_PACKET_LEN, STREAM_TIMEOUT_MS)) {
            airbridge_stream_close(stream);
            stream->show_error(stream->error_context, "STREAM ERROR");
            return false;
        }
        stream->header_pending = false;
        return false;
    }

    if(stream->sent == stream->total_len) {
        airbridge_stream_close(stream);
        return true;
    }

    uint64_t remaining = stream->total_len - stream->sent;
    size_t expected = (size_t)MIN(remaining, sizeof(report));
    size_t read = storage_file_read(stream->file, report, expected);
    if(read != expected || !airbridge_usb_vendor_send_response_blocking(
                               report, HID_VENDOR_PACKET_LEN, STREAM_TIMEOUT_MS)) {
        airbridge_stream_close(stream);
        stream->show_error(stream->error_context, "STREAM ERROR");
        return false;
    }
    stream->sent += read;
    return false;
}
