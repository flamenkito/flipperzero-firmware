#include "flooper_player.h"
#include "flooper_app.h"
#include <furi.h>
#include <furi_hal_speaker.h>
#include <storage/storage.h>
#include <stdio.h>

// allow: SIZE_OK — one private worker state machine; Todo 4 restricts production to this pair.
typedef struct {
    FlooperCommand command;
    unsigned generation;
} QueuedCommand;
typedef struct {
    FlooperPatternWorkspace parser;
    char input[FLOOPER_JSON_BYTES];
} LoadWorkspace;
struct FlooperPlayer {
    FuriThread* thread;
    FuriMessageQueue* commands;
    FuriMutex* mutex;
    bool exit_requested;
    unsigned command_generation;
    FlooperSnapshot shared, current;
    FlooperPattern* model;
    FlooperSchedule* schedule;
    FlooperPosition position;
    const FlooperInterval* sounding;
    uint64_t tick64, epoch_tick64, origin_count, elapsed_count;
    uint32_t raw_tick, tick_hz;
    bool owned;
};
_Static_assert(__GCC_ATOMIC_BOOL_LOCK_FREE == 2, "Sticky exit must be lock-free");

bool flooper_player_exit_requested(const FlooperPlayer* p) {
    return __atomic_load_n(&p->exit_requested, __ATOMIC_ACQUIRE);
}
void flooper_player_request_exit(FlooperPlayer* p) {
    __atomic_store_n(&p->exit_requested, true, __ATOMIC_RELEASE);
    const QueuedCommand wake = {.command.type = FlooperCommandQuit};
    furi_message_queue_put(p->commands, &wake, 0);
}
bool flooper_player_send(FlooperPlayer* p, FlooperCommand command) {
    switch(command.type) {
    case FlooperCommandQuit:
        flooper_player_request_exit(p);
        return true;
    case FlooperCommandToggle:
    case FlooperCommandRestart:
    case FlooperCommandLoadSelected:
        break;
    default:
        return false;
    }
    if(flooper_player_exit_requested(p)) return false;
    const QueuedCommand queued = {
        .command = command,
        .generation = __atomic_load_n(&p->command_generation, __ATOMIC_ACQUIRE)};
    return furi_message_queue_put(p->commands, &queued, 0) == FuriStatusOk;
}
bool flooper_player_snapshot(FlooperPlayer* p, FlooperSnapshot* output) {
    if(furi_mutex_acquire(p->mutex, 0) != FuriStatusOk) return false;
    *output = p->shared;
    furi_mutex_release(p->mutex);
    return true;
}
static void publish(FlooperPlayer* p) {
    if(furi_mutex_acquire(p->mutex, 1) == FuriStatusOk) {
        p->shared = p->current;
        furi_mutex_release(p->mutex);
    }
}
static uint64_t clock_now(FlooperPlayer* p) {
    uint32_t raw = furi_get_tick();
    p->tick64 += (uint32_t)(raw - p->raw_tick);
    p->raw_tick = raw;
    return p->tick64;
}
static void silence(FlooperPlayer* p) {
    if(p->sounding) furi_hal_speaker_stop();
    p->sounding = NULL;
}
static void release(FlooperPlayer* p) {
    if(p->owned) {
        furi_hal_speaker_stop();
        furi_hal_speaker_release();
        p->owned = false;
        p->sounding = NULL;
    }
}
static void discard(FlooperPlayer* p) {
    free(p->model);
    free(p->schedule);
    p->model = NULL;
    p->schedule = NULL;
    p->position = (FlooperPosition){0};
    FlooperSnapshot clean = {
        .generation = p->current.generation,
        .state = p->current.state,
        .error = p->current.error,
        .pattern_error = p->current.pattern_error,
        .schedule_error = p->current.schedule_error,
        .selected_index = p->current.selected_index};
    p->current = clean;
}
static void fail(FlooperPlayer* p, FlooperPlayerError error) {
    release(p);
    p->current.state = FlooperStatePatternError;
    p->current.error = error;
    discard(p);
}
static void* bounded_alloc(size_t size) {
    /* Local malloc aborts on OOM. Suspend task scheduling across admission and
     * allocation; 32 bytes covers this checkout's heap header/alignment/split.
     * ISR heap allocation is forbidden by the local allocator. */
    int32_t lock = furi_kernel_lock();
    void* result = NULL;
    if(memmgr_heap_get_max_free_block() >= size + 32) result = malloc(size);
    furi_kernel_restore_lock(lock);
    return result;
}
static void position_labels(FlooperPlayer* p) {
    p->current.section_index = p->position.section_index;
    p->current.cycle_index = p->position.cycle_index;
    p->current.count_index = p->position.count_index;
    p->current.count_label = p->position.label;
    memcpy(
        p->current.section_name,
        p->model->sections[p->position.section_index].name,
        sizeof(p->current.section_name));
}
static void reset_position(FlooperPlayer* p) {
    p->position.elapsed_count = p->schedule->sections[p->schedule->start_section].first_count;
    flooper_schedule_position(p->schedule, &p->position);
    position_labels(p);
}
static FlooperPlayerError read_document(FlooperPlayer* p, LoadWorkspace* work) {
    if(!furi_record_exists(RECORD_STORAGE)) return FlooperPlayerStorage;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    int32_t lock = furi_kernel_lock();
    File* file = memmgr_heap_get_max_free_block() >= 1024 ? storage_file_alloc(storage) : NULL;
    furi_kernel_restore_lock(lock);
    FlooperPlayerError error = FlooperPlayerMemory;
    if(file && !flooper_player_exit_requested(p)) {
        char path[sizeof(APP_ASSETS_PATH("")) + FLOOPER_CATALOG_FILENAME_MAX - 1];
        int length = snprintf(
            path,
            sizeof(path),
            "%s%s",
            APP_ASSETS_PATH(""),
            flooper_catalog[p->current.selected_index].filename);
        if(length < 0 || (size_t)length >= sizeof(path)) {
            storage_file_free(file);
            furi_record_close(RECORD_STORAGE);
            return FlooperPlayerStorage;
        }
        error = FlooperPlayerStorage;
        bool opened = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
        if(opened && !flooper_player_exit_requested(p)) {
            uint64_t size = storage_file_size(file);
            if(size == 0 || size > sizeof(work->input))
                error = FlooperPlayerSize;
            else {
                size_t used = 0;
                while(used < size && !flooper_player_exit_requested(p)) {
                    size_t chunk = size - used > 512 ? 512 : (size_t)size - used;
                    size_t got = storage_file_read(file, work->input + used, chunk);
                    if(got != chunk) break;
                    used += got;
                }
                if(used == size && !flooper_player_exit_requested(p)) {
                    p->current.pattern_error =
                        flooper_pattern_parse(work->input, used, &work->parser, p->model);
                    error = p->current.pattern_error == FlooperPatternOk ? FlooperPlayerOk :
                                                                           FlooperPlayerParse;
                }
            }
        }
        if(!storage_file_close(file)) error = FlooperPlayerStorage;
    }
    if(file) storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return error;
}
static void load_selected(FlooperPlayer* p, uint8_t selected) {
    __atomic_fetch_add(&p->command_generation, 1, __ATOMIC_RELEASE);
    release(p);
    p->current = (FlooperSnapshot){
        .generation = p->current.generation + 1,
        .state = FlooperStateLoading,
        .selected_index = selected};
    discard(p);
    publish(p);
    FlooperPlayerError error = FlooperPlayerSelection;
    if(selected < FLOOPER_CATALOG_COUNT && !flooper_player_exit_requested(p)) {
        error = FlooperPlayerMemory;
        p->model = bounded_alloc(sizeof(*p->model));
        LoadWorkspace* work = NULL;
        if(p->model && !flooper_player_exit_requested(p)) work = bounded_alloc(sizeof(*work));
        if(work && !flooper_player_exit_requested(p)) error = read_document(p, work);
        free(work);
        if(error == FlooperPlayerOk && !flooper_player_exit_requested(p)) {
            p->schedule = bounded_alloc(sizeof(*p->schedule));
            if(p->schedule && !flooper_player_exit_requested(p)) {
                p->current.schedule_error = flooper_schedule_compile(p->model, p->schedule);
                if(p->current.schedule_error != FlooperScheduleOk) error = FlooperPlayerCompile;
            } else
                error = FlooperPlayerMemory;
        }
    }
    if(error == FlooperPlayerOk && !flooper_player_exit_requested(p)) {
        const FlooperPattern* model = p->model;
        if(model->warning_cycle_mismatch)
            FURI_LOG_W(
                "Flooper",
                "Legacy cycle mismatch: reported=%lu derived=%llu",
                (unsigned long)model->reported_cycle_us,
                (unsigned long long)model->cycle_us);
        p->current.state = FlooperStatePaused;
        p->current.pulse_us = model->pulse_us;
        p->current.count_count = model->count_count;
        memcpy(p->current.display_name, model->display_name, sizeof(p->current.display_name));
        memcpy(p->current.count_labels, model->labels, sizeof(p->current.count_labels));
        for(uint8_t i = 0; i < model->count_count; ++i)
            if(model->accent_mask & (1U << i))
                p->current.accent_labels[p->current.accent_count++] = model->labels[i];
        reset_position(p);
    } else
        fail(p, error);
    furi_message_queue_reset(p->commands);
    __atomic_fetch_add(&p->command_generation, 1, __ATOMIC_RELEASE);
}
static uint64_t deadline(FlooperPlayer* p, uint64_t us) {
    uint64_t seconds = us / 1000000;
    uint64_t fraction = ((us % 1000000) * p->tick_hz + 500000) / 1000000;
    if(fraction > UINT64_MAX - p->epoch_tick64) return UINT64_MAX;
    if(seconds > (UINT64_MAX - p->epoch_tick64 - fraction) / p->tick_hz) return UINT64_MAX;
    return p->epoch_tick64 + seconds * p->tick_hz + fraction;
}
static void play(FlooperPlayer* p) {
    silence(p);
    if(!p->owned) p->owned = furi_hal_speaker_acquire(0);
    if(!p->owned) {
        p->current.state = FlooperStateSpeakerBusy;
        return;
    }
    p->epoch_tick64 = clock_now(p);
    p->origin_count = p->position.elapsed_count;
    p->elapsed_count = 0;
    p->current.state = FlooperStatePlaying;
}
static bool advance_position(FlooperPlayer* p, uint64_t now) {
    uint64_t ticks = now - p->epoch_tick64;
    if(ticks / p->tick_hz > (UINT64_MAX - UINT32_MAX - 2000000) / 1000000) {
        fail(p, FlooperPlayerClock);
        return false;
    }
    uint64_t us = (ticks / p->tick_hz) * 1000000 + ((ticks % p->tick_hz) * 1000000) / p->tick_hz;
    uint64_t count = us / p->schedule->pulse_us;
    /* Rounding may make the next ideal boundary due up to half a tick early.
     * Binary search also handles sub-tick pulses without catch-up iteration. */
    uint64_t low = count, high = (us + 1000000 / p->tick_hz + 1) / p->schedule->pulse_us + 1;
    while(low + 1 < high) {
        uint64_t mid = low + (high - low) / 2;
        if(deadline(p, mid * p->schedule->pulse_us) <= now)
            low = mid;
        else
            high = mid;
    }
    count = low;
    if(count > UINT64_MAX - p->origin_count) {
        fail(p, FlooperPlayerClock);
        return false;
    }
    bool changed = count != p->elapsed_count;
    p->elapsed_count = count;
    p->position.elapsed_count = p->origin_count + count;
    FlooperScheduleError status = flooper_schedule_position(p->schedule, &p->position);
    if(status != FlooperScheduleOk) {
        release(p);
        if(status == FlooperSchedulePositionEnd) {
            p->current.state = FlooperStatePaused;
            reset_position(p);
        } else
            fail(p, FlooperPlayerClock);
        return false;
    }
    if(changed) {
        silence(p);
        position_labels(p);
        publish(p);
    }
    return true;
}
static uint64_t playback(FlooperPlayer* p, uint64_t now) {
    if(!advance_position(p, now)) return now;
    uint64_t start_us = p->position.count_start_us - p->origin_count * p->schedule->pulse_us;
    uint64_t next = deadline(p, start_us + p->schedule->pulse_us);
    uint64_t audio_now = clock_now(p);
    if(next <= audio_now) return audio_now;
    const FlooperCountRange* range = &p->schedule->ranges[p->position.range_index];
    const FlooperInterval* active = NULL;
    for(uint16_t i = 0; i < range->interval_count; ++i) {
        const FlooperInterval* interval = &p->schedule->intervals[range->first_interval + i];
        uint64_t end = deadline(p, start_us + interval->end_us);
        if(end <= audio_now) continue;
        uint64_t start = deadline(p, start_us + interval->start_us);
        if(start <= audio_now) {
            active = interval;
            next = end;
        } else
            next = start;
        break;
    }
    if(active != p->sounding) {
        silence(p);
        if(active && !flooper_player_exit_requested(p) && clock_now(p) < next) {
            furi_hal_speaker_start(active->frequency_hz, active->volume);
            p->sounding = active;
        }
    }
    return next;
}
static void command(FlooperPlayer* p, FlooperCommand cmd) {
    switch(cmd.type) {
    case FlooperCommandQuit:
        flooper_player_request_exit(p);
        break;
    case FlooperCommandLoadSelected:
        if(p->current.state == FlooperStatePaused || p->current.state == FlooperStatePatternError)
            load_selected(p, cmd.selected_index);
        break;
    case FlooperCommandRestart:
        if(p->schedule) {
            reset_position(p);
            if(p->current.state == FlooperStatePlaying) play(p);
        }
        break;
    case FlooperCommandToggle:
        switch(p->current.state) {
        case FlooperStatePlaying:
            if(!advance_position(p, clock_now(p))) break;
            release(p);
            p->current.state = FlooperStatePaused;
            break;
        case FlooperStatePaused:
        case FlooperStateSpeakerBusy:
            play(p);
            break;
        case FlooperStateLoading:
        case FlooperStatePatternError:
        case FlooperStateQuitting:
            break;
        }
        break;
    }
}
static bool wait_until_or_command(FlooperPlayer* p, uint64_t target, QueuedCommand* cmd) {
    if(flooper_player_exit_requested(p)) return false;
    uint64_t now = clock_now(p);
    uint64_t remaining = target > now ? target - now : 0;
    uint32_t chunk = p->tick_hz / 10;
    uint32_t timeout = remaining < chunk ? (uint32_t)remaining : chunk;
    bool got = furi_message_queue_get(p->commands, cmd, timeout) == FuriStatusOk;
    clock_now(p);
    return got && !flooper_player_exit_requested(p);
}
static int32_t worker(void* context) {
    FlooperPlayer* p = context;
    p->tick_hz = furi_kernel_get_tick_frequency();
    p->raw_tick = furi_get_tick();
    p->tick64 = p->raw_tick;
    if(!flooper_player_exit_requested(p)) load_selected(p, p->current.selected_index);
    while(!flooper_player_exit_requested(p)) {
        uint64_t now = clock_now(p);
        uint64_t target = now + p->tick_hz / 10;
        if(p->current.state == FlooperStatePlaying) target = playback(p, now);
        publish(p);
        QueuedCommand cmd;
        if(wait_until_or_command(p, target, &cmd) &&
           cmd.generation == __atomic_load_n(&p->command_generation, __ATOMIC_ACQUIRE))
            command(p, cmd.command);
    }
    release(p);
    p->current.state = FlooperStateQuitting;
    publish(p);
    return 0;
}
FlooperPlayer* flooper_player_alloc(uint8_t selected_index) {
    /* Conservative contiguous admission for local thread/stack, queue, mutex,
     * strings and player (well below 16KiB together). No task can consume it
     * between this check and those allocations; no blocking call in this span. */
    int32_t lock = furi_kernel_lock();
    FlooperPlayer* p = NULL;
    if(memmgr_heap_get_max_free_block() >= 16384) {
        p = malloc(sizeof(*p));
        if(p) {
            memset(p, 0, sizeof(*p));
            p->current.state = FlooperStateLoading;
            p->current.selected_index = selected_index;
            p->shared = p->current;
            p->commands =
                furi_message_queue_alloc(FLOOPER_COMMAND_CAPACITY, sizeof(QueuedCommand));
            p->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
            p->thread =
                furi_thread_alloc_ex("FlooperPlayer", FLOOPER_WORKER_STACK_SIZE, worker, p);
        }
    }
    furi_kernel_restore_lock(lock);
    if(p) furi_thread_start(p->thread);
    return p;
}
void flooper_player_free(FlooperPlayer* p) {
    flooper_player_request_exit(p);
    furi_thread_join(p->thread);
    free(p->model);
    free(p->schedule);
    furi_thread_free(p->thread);
    furi_message_queue_free(p->commands);
    furi_mutex_free(p->mutex);
    free(p);
}
