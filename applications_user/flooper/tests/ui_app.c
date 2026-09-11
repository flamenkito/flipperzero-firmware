#include "ui_fake.h"
#include "../flooper.c"

static unsigned step, selection, playback_step;
static void input_matrix(const char* root);
static void exit_routes(const char* root);
static void scripted_step(void) {
    assert(step < 100);
    fake_sync();
    ui_fake_draw();
    FlooperApp* app = ui_fake.app_context;
    switch(step++) {
    case 0:
        assert(ui_has_text("the dolphin compás looper"));
        for(InputType type = InputTypePress; type < InputTypeMAX; ++type)
            for(InputKey key = InputKeyUp; key < InputKeyMAX; ++key)
                if(type != InputTypeLong || key != InputKeyBack) ui_fake_input(type, key);
        fake_advance(999);
        break;
    case 1:
        assert(app->screen == FlooperSplash && ui_has_text("FLOOPER"));
        assert(fake_events(FakeAcquire) == 0);
        fake_advance(1000);
        break;
    case 2:
        assert(app->screen == FlooperSelector && ui_has_text("BULERIAS"));
        ui_fake_input(InputTypeShort, InputKeyUp);
        break;
    case 3:
        assert(ui_has_text("TANGOS"));
        ui_fake_input(InputTypeShort, InputKeyDown);
        break;
    case 4:
        assert(ui_has_text("BULERIAS"));
        if(selection) ui_fake_input(InputTypeShort, InputKeyDown);
        break;
    case 5:
        assert(app->selected == selection);
        ui_fake_input(InputTypeShort, InputKeyOk);
        break;
    default:
        assert(app->screen == FlooperPlayback);
        switch(playback_step) {
        case 0:
            if(app->pending_load || !ui_has_text("PAUSED")) {
                fake_wake();
                break;
            }
            assert(app->ui->model.snapshot.count_label == (selection ? 4 : 6));
            assert(fake_events(FakeAcquire) == 0);
            ui_fake_input(InputTypeShort, InputKeyOk);
            playback_step++;
            break;
        case 1:
            if(!ui_has_text("PLAYING")) break;
            fake_advance(1000 + (3ULL * app->ui->model.snapshot.pulse_us + 500) / 1000);
            playback_step++;
            break;
        case 2:
            assert(app->ui->model.snapshot.count_label == 3 && app->ui->model.contact);
            fake_advance(fake.tick + 79);
            playback_step++;
            break;
        case 3:
            assert(app->ui->model.contact);
            fake_advance(fake.tick + 1);
            playback_step++;
            break;
        default:
            assert(!app->ui->model.contact && app->ui->model.snapshot.count_label == 3);
            ui_fake_input(InputTypeLong, InputKeyBack);
            assert(flooper_player_exit_requested(app->player) && app->exit_requested);
            break;
        }
        break;
    }
}
void test_ui_app(const char* root) {
    /* Given each canonical document; When running the actual entry and callbacks;
     * Then silent splash timing, wrapped selection, paused load and clean exit. */
    for(selection = 0; selection < 2; ++selection) {
        fake_reset(1000, 0);
        fake.app_mode = true;
        ui_fake_reset();
        fake_read_document(
            root,
            selection ? "assets/flipper_tangos_pattern_v2_1.json" :
                        "assets/flipper_bulerias_pattern_v2_1.json");
        step = playback_step = 0;
        fake.app_step = scripted_step;
        assert(flooper_app(NULL) == 0);
        assert(fake.joined && fake.exited && !fake.gui_records);
        assert(ui_fake.removed == 1 && ui_fake.freed == 1 && ui_fake.timer_freed == 1);
        ui_cases++;
    }
    input_matrix(root);
    exit_routes(root);
}

static void input_matrix(const char* root) {
    fake_reset(1000, 0);
    fake.app_mode = true;
    ui_fake_reset();
    FlooperApp app = {.screen = FlooperPlayback};
    app.input = furi_message_queue_alloc(8, sizeof(FlooperInput));
    fake_read_document(root, "assets/flipper_tangos_pattern_v2_1.json");
    app.player = flooper_player_alloc(1);
    fake_sync();
    /* Given playback; When each ignored event crosses the real callback/router;
     * Then neither the worker position nor its audio event log changes. */
    for(InputType type = InputTypePress; type < InputTypeMAX; ++type) {
        for(InputKey key = InputKeyUp; key < InputKeyMAX; ++key) {
            if((key == InputKeyOk && (type == InputTypeShort || type == InputTypeLong)) ||
               (key == InputKeyBack && type == InputTypeLong))
                continue;
            InputEvent event = {.type = type, .key = key};
            input_callback(&event, &app);
            FlooperInput input;
            if(furi_message_queue_get(app.input, &input, 0) == FuriStatusOk)
                route_input(&app, &input);
            fake_wake();
            FlooperSnapshot snapshot;
            assert(flooper_player_snapshot(app.player, &snapshot));
            assert(snapshot.state == FlooperStatePaused && snapshot.count_label == 4);
            assert(fake_events(FakeAcquire) == 0);
        }
    }
    ui_cases++;
    FlooperInput toggle = {
        .screen = FlooperPlayback, .event = {.type = InputTypeShort, .key = InputKeyOk}};
    route_input(&app, &toggle);
    fake_sync();
    assert(fake.owned && fake_events(FakeStart) > 0);
    FlooperInput restart = {
        .screen = FlooperPlayback, .event = {.type = InputTypeLong, .key = InputKeyOk}};
    route_input(&app, &restart);
    fake_sync();
    route_input(&app, &toggle);
    fake_sync();
    assert(!fake.owned);
    close_app(&app);
    close_app(&app);
    ui_cases++;
}

static FlooperApp* exiting_app;
static void observe_exit(void) {
    assert(exiting_app && !exiting_app->exit_requested);
    assert(flooper_player_exit_requested(exiting_app->player));
}
static void exit_routes(const char* root) {
    /* Given every screen/player state with saturated input; When Long Back is
     * delivered; Then worker sticky exit precedes app exit and join is safe. */
    for(unsigned scenario = 0; scenario < 8; ++scenario) {
        fake_reset(1000, 0);
        fake.app_mode = true;
        ui_fake_reset();
        FlooperApp app = {
            .screen = scenario == 0 ? FlooperSplash :
                      scenario == 1 ? FlooperSelector :
                                      FlooperPlayback};
        app.input = furi_message_queue_alloc(8, sizeof(FlooperInput));
        fake_read_document(root, "assets/flipper_tangos_pattern_v2_1.json");
        if(scenario == 2) fake.block_stage = FakeStageOpen;
        if(scenario == 3) fake.open_fail = true;
        if(scenario == 4) fake.busy = true;
        app.player = flooper_player_alloc(1);
        if(scenario == 2)
            fake_wait_stage(FakeStageOpen);
        else
            fake_sync();
        if(scenario == 4 || scenario == 6) {
            assert(
                flooper_player_send(app.player, (FlooperCommand){.type = FlooperCommandToggle}));
            fake_sync();
        }
        if(scenario != 2) {
            FlooperSnapshot snapshot;
            assert(flooper_player_snapshot(app.player, &snapshot));
            assert(
                snapshot.state == (scenario == 3 ? FlooperStatePatternError :
                                   scenario == 4 ? FlooperStateSpeakerBusy :
                                   scenario == 6 ? FlooperStatePlaying :
                                                   FlooperStatePaused));
        }
        FlooperInput fill = {.event.type = InputTypeMAX};
        for(unsigned i = 0; i < 8; ++i)
            assert(furi_message_queue_put(app.input, &fill, 0) == FuriStatusOk);
        InputEvent exit = {.type = InputTypeLong, .key = InputKeyBack};
        exiting_app = &app;
        fake.exit_observer = observe_exit;
        input_callback(&exit, &app);
        fake.exit_observer = NULL;
        assert(app.exit_requested && flooper_player_exit_requested(app.player));
        if(scenario == 2) fake_unblock();
        close_app(&app);
        close_app(&app);
        assert(fake.joined && fake.exited && !fake.owned);
        ui_cases++;
    }
}

void test_adapters_app(const char* root) {
    input_matrix(root);
    exit_routes(root);
    test_ui_inflight(root);
    /* Given either admission failure; When entry closes; Then records and partial
     * UI resources are freed without joining a worker that never existed. */
    for(int fail = 0; fail <= 1; ++fail) {
        fake_reset(1000, 0);
        fake.app_mode = true;
        ui_fake_reset();
        fake.heap_fail_after = fail;
        assert(flooper_app(NULL) == -1);
        assert(!fake.worker_started && !fake.joined && fake.gui_records == 0);
        assert(ui_fake.freed == (unsigned)fail);
        ui_cases++;
    }
    /* Given a current generation; When an old snapshot arrives; Then it cannot
     * replace text/position or trigger the visual label-3 pose. */
    FlooperUiModel model = {.screen = FlooperPlayback, .minimum_generation = 8};
    FlooperSnapshot current = {.generation = 8, .count_count = 4, .count_label = 4};
    flooper_ui_tick(&model, &current, 100);
    FlooperSnapshot stale = {.generation = 7, .count_count = 4, .count_label = 3};
    flooper_ui_tick(&model, &stale, 101);
    assert(model.snapshot.generation == 8 && model.snapshot.count_label == 4 && !model.contact);
    ui_cases++;
}
