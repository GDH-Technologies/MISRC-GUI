#include "../common/ddd_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return false; \
    } \
} while (0)

typedef struct {
    uint8_t registers[256];
    uint8_t requests[16];
    uint16_t values[16];
    size_t request_count;
    int fail_at;
} mock_usb_t;

static void mock_usb_init(mock_usb_t *mock)
{
    memset(mock, 0, sizeof(*mock));
    mock->registers[DDD_REGISTER_IDENTITY] = DDD_IDENTITY_VALUE;
    mock->registers[DDD_REGISTER_MAP_VERSION] =
        DDD_SUPPORTED_REGISTER_MAP;
    mock->registers[DDD_REGISTER_IMAGE_ROLE] =
        DDD_APPLICATION_IMAGE_ROLE;
    mock->registers[DDD_REGISTER_BUILD_FLAGS] = DDD_BUILD_COMMIT_FLAG;
    memcpy(&mock->registers[DDD_REGISTER_COMMIT], "deadbeef", 8);
    mock->fail_at = -1;
}

static int mock_transfer(void *context,
                         uint8_t request_type,
                         uint8_t request,
                         uint16_t value,
                         uint16_t index,
                         uint8_t *data,
                         uint16_t length)
{
    mock_usb_t *mock = (mock_usb_t *)context;
    size_t call = mock->request_count;
    (void)index;
    if (call < sizeof(mock->requests)) {
        mock->requests[call] = request;
        mock->values[call] = value;
    }
    ++mock->request_count;
    if ((int)call == mock->fail_at) return -1;
    if (request_type == DDD_USB_REQUEST_VENDOR_IN &&
        request == DDD_REQUEST_REGISTER_READ) {
        memcpy(data, &mock->registers[value & 0xffu], length);
        return length;
    }
    if (request_type == DDD_USB_REQUEST_VENDOR_OUT &&
        request == DDD_REQUEST_REGISTER_WRITE) {
        mock->registers[(value >> 8) & 0xffu] = value & 0xffu;
        return 0;
    }
    if (request_type == DDD_USB_REQUEST_VENDOR_OUT &&
        request == DDD_REQUEST_COLLECTION) {
        return 0;
    }
    return -1;
}

static bool test_profiles_and_rates(void)
{
    ddd_profile_index_state_t indices;
    CHECK(ddd_classify_device(DDD_LEGACY_VENDOR_ID,
                              DDD_LEGACY_PRODUCT_ID, 0) ==
          DDD_DEVICE_LEGACY);
    CHECK(ddd_classify_device(DDD_CURRENT_VENDOR_ID,
                              DDD_CURRENT_PRODUCT_ID, 0x0100) ==
          DDD_DEVICE_PROTOCOL_V1);
    CHECK(ddd_classify_device(DDD_CURRENT_VENDOR_ID,
                              DDD_CURRENT_PRODUCT_ID, 0x0200) ==
          DDD_DEVICE_UNSUPPORTED);
    CHECK(ddd_classify_device(0xffff, 0xffff, 0) == DDD_DEVICE_NOT_DDD);
    CHECK(ddd_profile_supports_decimation(DDD_DEVICE_LEGACY, 1));
    CHECK(!ddd_profile_supports_decimation(DDD_DEVICE_LEGACY, 2));
    CHECK(ddd_profile_supports_decimation(DDD_DEVICE_PROTOCOL_V1, 1));
    CHECK(ddd_profile_supports_decimation(DDD_DEVICE_PROTOCOL_V1, 2));
    CHECK(ddd_sample_rate_hz(1) == 40000000u);
    CHECK(ddd_sample_rate_hz(2) == 20000000u);
    CHECK(ddd_sample_rate_hz(3) == 0);
    CHECK(ddd_v1_link_speed_allowed(false, false));
    CHECK(!ddd_v1_link_speed_allowed(true, false));
    CHECK(ddd_v1_link_speed_allowed(true, true));

    ddd_profile_index_state_init(&indices);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_PROTOCOL_V1) == 0);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_LEGACY) == 0);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_UNSUPPORTED) == 0);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_PROTOCOL_V1) == 1);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_LEGACY) == 1);
    CHECK(ddd_profile_index_take(&indices, DDD_DEVICE_NOT_DDD) == -1);
    return true;
}

static bool test_topology_and_endpoint(void)
{
    uint8_t ports[] = {3, 2, 7};
    char path[32];
    ddd_stream_selector_t selector;
    ddd_stream_path_t selected;
    ddd_stream_endpoint_candidate_t wrong = {
        .interface_number = 0, .alternate_setting = 0,
        .endpoint_address = 0x81, .max_packet_size = 512,
        .is_bulk = true, .is_in = true
    };
    ddd_stream_endpoint_candidate_t exact = {
        .interface_number = 0, .alternate_setting = 0,
        .endpoint_address = 0x81, .max_packet_size = 1024,
        .is_bulk = true, .is_in = true
    };
    CHECK(ddd_format_usb_topology_path(1, ports, 3, path, sizeof(path)));
    CHECK(strcmp(path, "usb:1-3.2.7") == 0);
    ddd_stream_selector_init(&selector, DDD_DEVICE_PROTOCOL_V1);
    ddd_stream_selector_consider(&selector, &wrong);
    CHECK(!ddd_stream_selector_get(&selector, &selected));
    ddd_stream_selector_consider(&selector, &exact);
    CHECK(ddd_stream_selector_get(&selector, &selected));
    ddd_stream_selector_consider(&selector, &exact);
    CHECK(!ddd_stream_selector_get(&selector, &selected));
    return true;
}

static bool test_lifecycle(void)
{
    mock_usb_t mock;
    ddd_collection_state_t state;
    ddd_control_ops_t ops = {.transfer = mock_transfer, .context = &mock};

    mock_usb_init(&mock);
    CHECK(ddd_collection_start_v1(&ops, true, 2, &state) ==
          DDD_PROTOCOL_OK);
    CHECK(state.collection_active);
    CHECK(state.sample_rate_hz == 20000000u);
    CHECK(mock.request_count == 6);
    CHECK(mock.requests[0] == DDD_REQUEST_REGISTER_READ);
    CHECK(mock.requests[1] == DDD_REQUEST_REGISTER_WRITE);
    CHECK(mock.values[1] == ddd_make_register_write(
        DDD_REGISTER_TEST_MODE, 1));
    CHECK(mock.requests[2] == DDD_REQUEST_REGISTER_WRITE);
    CHECK(mock.values[2] == ddd_make_register_write(
        DDD_REGISTER_DECIMATION, 2));
    CHECK(mock.requests[5] == DDD_REQUEST_COLLECTION);
    CHECK(mock.values[5] == 1);
    CHECK(ddd_collection_stop_v1(&ops, &state) == DDD_PROTOCOL_OK);
    CHECK(!state.collection_active);

    mock_usb_init(&mock);
    mock.fail_at = 2;
    CHECK(ddd_collection_start_v1(&ops, true, 2, &state) ==
          DDD_PROTOCOL_CONTROL_FAILURE);
    CHECK(state.rollback_attempted);
    CHECK(state.rollback_succeeded);
    CHECK(mock.registers[DDD_REGISTER_TEST_MODE] == 0);
    CHECK(mock.registers[DDD_REGISTER_DECIMATION] == 1);
    return true;
}

static bool test_validators(void)
{
    ddd_sequence_validator_t sequence;
    ddd_test_ramp_validator_t ramp;
    uint16_t *words = calloc(DDD_SEQUENCE_SAMPLES_PER_MARKER,
                             sizeof(*words));
    CHECK(words != NULL);
    ddd_sequence_validator_init(&sequence);
    words[0] = (uint16_t)(10u << 10);
    words[1] = (uint16_t)(11u << 10);
    CHECK(ddd_sequence_validator_feed(&sequence, words, 2) ==
          DDD_VALIDATION_OK);
    CHECK(sequence.phase == DDD_SEQUENCE_RUNNING);
    for (size_t i = 0; i < DDD_SEQUENCE_SAMPLES_PER_MARKER; ++i) {
        words[i] = (uint16_t)(11u << 10);
    }
    /* One sample for marker 11 was already consumed above. */
    CHECK(ddd_sequence_validator_feed(
              &sequence, words, DDD_SEQUENCE_SAMPLES_PER_MARKER - 1) ==
          DDD_VALIDATION_OK);
    words[0] = (uint16_t)(12u << 10);
    CHECK(ddd_sequence_validator_feed(&sequence, words, 1) ==
          DDD_VALIDATION_OK);
    words[0] = (uint16_t)(14u << 10);
    CHECK(ddd_sequence_validator_feed(&sequence, words, 1) ==
          DDD_VALIDATION_MISMATCH);

    ddd_test_ramp_validator_init(&ramp);
    for (size_t i = 0; i < DDD_TEST_RAMP_NEW_WRAP; ++i) words[i] = (uint16_t)i;
    words[DDD_TEST_RAMP_NEW_WRAP] = 0;
    CHECK(ddd_test_ramp_validator_feed(
              &ramp, words, DDD_TEST_RAMP_NEW_WRAP + 1u) == DDD_VALIDATION_OK);
    words[0] = 2;
    CHECK(ddd_test_ramp_validator_feed(&ramp, words, 1) ==
          DDD_VALIDATION_MISMATCH);

    ddd_test_ramp_validator_init(&ramp);
    for (size_t i = 0; i < DDD_TEST_RAMP_LEGACY_WRAP; ++i) {
        words[i] = (uint16_t)i;
    }
    words[DDD_TEST_RAMP_LEGACY_WRAP] = 0;
    CHECK(ddd_test_ramp_validator_feed(
              &ramp, words, DDD_TEST_RAMP_LEGACY_WRAP + 1u) ==
          DDD_VALIDATION_OK);
    free(words);
    return true;
}

int main(void)
{
    if (!test_profiles_and_rates() ||
        !test_topology_and_endpoint() ||
        !test_lifecycle() ||
        !test_validators()) {
        return 1;
    }
    puts("DDD protocol tests passed");
    return 0;
}
