#include "../misrc_gui/input/gui_ddd_async.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_EVENT_ERROR (-99)

typedef struct {
    uint64_t now_ms;
    size_t submit_calls;
    size_t cancel_calls;
    size_t event_pump_calls;
} async_fault_state_t;

static uint64_t advancing_now_ms(void *context)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    state->now_ms += 400;
    return state->now_ms;
}

static int permanent_event_error(void *context, long timeout_us)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    (void)timeout_us;
    state->event_pump_calls++;
    return TEST_EVENT_ERROR;
}

static int accept_fake_submit(
    void *context,
    struct libusb_transfer *transfer)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    assert(transfer != NULL);
    state->submit_calls++;
    return 0;
}

static int accept_fake_cancel(
    void *context,
    struct libusb_transfer *transfer)
{
    async_fault_state_t *state = (async_fault_state_t *)context;
    assert(transfer != NULL);
    state->cancel_calls++;
    return 0;
}

static gui_ddd_async_consume_result_t reject_unexpected_consume(
    void *context,
    const uint8_t *data,
    size_t size)
{
    (void)context;
    (void)data;
    (void)size;
    assert(!"permanent event failure must not consume a block");
    return GUI_DDD_ASYNC_CONSUME_FAILED;
}

static gui_ddd_async_config_t make_fault_config(
    async_fault_state_t *state,
    atomic_bool *capture_running,
    atomic_bool *transfer_ready,
    atomic_bool *startup_failed,
    int *usb_context_marker,
    int *device_handle_marker)
{
    gui_ddd_async_config_t config;

    memset(&config, 0, sizeof(config));
    config.usb_context =
        (struct libusb_context *)(void *)usb_context_marker;
    config.device_handle =
        (struct libusb_device_handle *)(void *)device_handle_marker;
    config.endpoint = 0x82;
    config.capture_running = capture_running;
    config.transfer_ready = transfer_ready;
    config.startup_failed = startup_failed;
    config.consume = reject_unexpected_consume;
    config.consume_context = state;
    config.event_pump_override = permanent_event_error;
    config.event_pump_context = state;
    config.now_ms_override = advancing_now_ms;
    config.now_ms_context = state;
    config.submit_override = accept_fake_submit;
    config.submit_context = state;
    config.cancel_override = accept_fake_cancel;
    config.cancel_context = state;
    return config;
}

static void test_result_is_required_for_orphan_ownership(void)
{
    async_fault_state_t state = {0};
    atomic_bool capture_running = true;
    atomic_bool transfer_ready = true;
    atomic_bool startup_failed = false;
    int usb_context_marker = 1;
    int device_handle_marker = 2;
    gui_ddd_async_config_t config = make_fault_config(
        &state, &capture_running, &transfer_ready, &startup_failed,
        &usb_context_marker, &device_handle_marker);

    assert(gui_ddd_async_run(&config, NULL) == -1);
    assert(!atomic_load(&transfer_ready));
    assert(atomic_load(&startup_failed));
    assert(state.submit_calls == 0);
}

static void test_permanent_event_error_returns_bounded_orphan(void)
{
    async_fault_state_t state = {0};
    atomic_bool capture_running = true;
    atomic_bool transfer_ready = false;
    atomic_bool startup_failed = false;
    int usb_context_marker = 1;
    int device_handle_marker = 2;
    gui_ddd_async_config_t config = make_fault_config(
        &state, &capture_running, &transfer_ready, &startup_failed,
        &usb_context_marker, &device_handle_marker);
    gui_ddd_async_result_t result;

    memset(&result, 0, sizeof(result));
    assert(!gui_ddd_async_global_quarantine_active());
    assert(gui_ddd_async_run(&config, &result) == -1);

    assert(result.code == GUI_DDD_ASYNC_RESULT_EVENT_FAILURE);
    assert(result.libusb_error == TEST_EVENT_ERROR);
    assert(result.ready_signalled);
    assert(result.transfers_unreaped);
    assert(result.unreaped_transfers == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(result.active_callbacks == 0);
    assert(result.orphan != NULL);
    assert(state.submit_calls == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(state.cancel_calls == GUI_DDD_ASYNC_TRANSFER_COUNT);
    assert(state.event_pump_calls > 0);
    assert(state.event_pump_calls <= 4);
    assert(state.now_ms <=
           GUI_DDD_ASYNC_CANCEL_REAP_TIMEOUT_MS + 1200);
    assert(!atomic_load(&transfer_ready));
    assert(atomic_load(&startup_failed));

    assert(gui_ddd_async_orphan_has_unreaped(result.orphan));
    assert(!gui_ddd_async_orphan_try_reclaim(result.orphan));
    assert(!gui_ddd_async_policy_sync_control_allowed(true));
    assert(gui_ddd_async_orphan_abandon(result.orphan));
    assert(gui_ddd_async_global_quarantine_active());
    assert(gui_ddd_async_orphan_abandon(result.orphan));
}

int main(void)
{
    test_result_is_required_for_orphan_ownership();
    test_permanent_event_error_returns_bounded_orphan();
    puts("gui_ddd_async_fault_test: OK");
    return 0;
}
