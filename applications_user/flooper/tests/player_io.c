#include "player_fake.h"
#include "fakes/furi_hal_speaker.h"
#include "fakes/storage/storage.h"

struct Storage {
    unsigned unused;
};
struct File {
    size_t offset;
    bool attempted, closed;
};
static Storage storage;
static char loaded[16385];

void fake_document(const char* document) {
    fake.document = document;
    fake.document_size = strlen(document);
}
void fake_read_document(const char* root, const char* relative) {
    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/%s", root, relative) < (int)sizeof(path));
    FILE* file = fopen(path, "rb");
    assert(file);
    size_t size = fread(loaded, 1, sizeof(loaded) - 1, file);
    assert(feof(file) && !ferror(file) && fclose(file) == 0);
    loaded[size] = 0;
    fake_document(loaded);
}
bool furi_record_exists(const char* name) {
    assert(fake_worker && strcmp(name, RECORD_STORAGE) == 0);
    return !fake.record_missing;
}
void* furi_record_open(const char* name) {
    if(strcmp(name, "gui") == 0) {
        assert(!fake_worker && fake.gui_records == 0);
        fake.gui_records++;
        return &storage;
    }
    assert(fake_worker && strcmp(name, RECORD_STORAGE) == 0 && !fake.record_missing);
    fake.record_count++;
    return &storage;
}
void furi_record_close(const char* name) {
    if(strcmp(name, "gui") == 0) {
        assert(!fake_worker && fake.gui_records == 1);
        assert(!fake.worker_started || fake.joined);
        fake.gui_records--;
        return;
    }
    assert(fake_worker && strcmp(name, RECORD_STORAGE) == 0 && fake.record_count == 1);
    fake.record_count--;
}
File* storage_file_alloc(Storage* instance) {
    assert(fake_worker && !fake.owned && instance == &storage);
    fake.file_count++;
    return calloc(1, sizeof(File));
}
void storage_file_free(File* file) {
    assert(fake_worker && fake.file_count == 1);
    assert(!file->attempted || file->closed);
    fake.file_count--;
    free(file);
}
bool storage_file_open(File* file, const char* path, FS_AccessMode access, FS_OpenMode mode) {
    assert(file && access == FSAM_READ && mode == FSOM_OPEN_EXISTING);
    assert(
        (fake.expected_path && strcmp(path, fake.expected_path) == 0) ||
        strcmp(path, "/assets/flipper_bulerias_pattern_v2_1.json") == 0 ||
        strcmp(path, "/assets/flipper_tangos_pattern_v2_1.json") == 0);
    fake.opens++;
    file->attempted = true;
    fake_stage(FakeStageOpen);
    return !fake.open_fail;
}
bool storage_file_close(File* file) {
    assert(file);
    file->closed = true;
    fake.closes++;
    fake_stage(FakeStageClose);
    return !fake.close_fail;
}
uint64_t storage_file_size(File* file) {
    assert(file);
    fake_stage(FakeStageSize);
    return fake.document_size;
}
size_t storage_file_read(File* file, void* buffer, size_t size) {
    fake.reads++;
    assert(size <= 512);
    fake_stage(FakeStageRead);
    if(fake.short_read) return 0;
    size_t remaining = fake.document_size - file->offset;
    if(size > remaining) size = remaining;
    memcpy(buffer, fake.document + file->offset, size);
    file->offset += size;
    return size;
}
bool furi_hal_speaker_acquire(uint32_t timeout) {
    assert(fake_worker && timeout == 0 && !fake.owned);
    assert(fake.file_count == 0 && fake.record_count == 0);
    fake_event(FakeAcquire, 0);
    fake.owned = !fake.busy;
    return fake.owned;
}
void furi_hal_speaker_start(float frequency, float volume) {
    assert(fake_worker && fake.owned && !fake.sounding);
    assert(volume > 0 && volume <= 1 && frequency > 0);
    fake.sounding = true;
    fake_event(FakeStart, frequency);
}
void furi_hal_speaker_stop(void) {
    assert(fake_worker && fake.owned);
    fake.sounding = false;
    fake_event(FakeStop, 0);
}
void furi_hal_speaker_release(void) {
    assert(fake_worker && fake.owned && !fake.sounding);
    fake.owned = false;
    fake_event(FakeRelease, 0);
}
void fake_log(const char* tag, const char* format, ...) {
    assert(fake_worker && !fake.owned && tag && format);
    fake.logs++;
}
