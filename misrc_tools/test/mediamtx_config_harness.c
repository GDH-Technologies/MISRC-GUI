/*
 * Unit harness for gui_mediamtx's config rendering.
 *
 * The generated mediamtx.yml carries the design's hard requirement: this
 * instance must not interfere with capture-node's three on the same host.
 * Two ways to break that, both silent:
 *
 *   - drift onto one of capture-node's ports (8554/8000/8001, x10000 per tier);
 *   - forget to disable a listener, since mediamtx defaults rtmp/srt/moq to ON
 *     and would quietly claim 1935/8890/8892.
 *
 * Neither shows up as an error -- mediamtx would either fail to bind or bind a
 * port that belongs to someone else. So they are asserted here.
 */

#include "gui_mediamtx.h"

#include <stdio.h>
#include <string.h>

static int failures;

static int expect_contains(const char *yaml, const char *needle, const char *what)
{
    if (strstr(yaml, needle) == NULL) {
        fprintf(stderr, "ASSERTION FAILED: %s: missing %s\n", what, needle);
        failures++;
        return 1;
    }
    return 0;
}

static int expect_absent(const char *yaml, const char *needle, const char *what)
{
    if (strstr(yaml, needle) != NULL) {
        fprintf(stderr, "ASSERTION FAILED: %s: must not contain %s\n", what, needle);
        failures++;
        return 1;
    }
    return 0;
}

static int expect_eq_int(int got, int want, const char *what)
{
    if (got != want) {
        fprintf(stderr, "ASSERTION FAILED: %s: expected=%d got=%d\n", what, want, got);
        failures++;
        return 1;
    }
    return 0;
}

/* The ports the design settled on: mediamtx canonical + 100. */
static int test_default_ports_are_canonical_plus_100(void)
{
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();

    expect_eq_int(cfg.rtsp, 8654, "default rtsp port");
    expect_eq_int(cfg.rtp, 8100, "default rtp port");
    expect_eq_int(cfg.rtcp, 8101, "default rtcp port");
    expect_eq_int(cfg.hls, 8988, "default hls port");
    expect_eq_int(cfg.webrtc_http, 8989, "default webrtc http port");
    expect_eq_int(cfg.webrtc_ice, 8289, "default webrtc ice port");
    expect_eq_int(cfg.lan ? 1 : 0, 0, "default bind is loopback");

    if (failures == 0) puts("PASS: default ports are canonical + 100");
    return failures;
}

/* The hard requirement, stated as a test: nothing capture-node owns, at any
 * tier, may appear in a config we generate. */
static int test_never_emits_a_capture_node_port(void)
{
    char yaml[8192];
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
    int before = failures;

    if (expect_eq_int(gui_mediamtx_render_config(&cfg, yaml, sizeof yaml) > 0, 1,
                      "render loopback") == 0) {
        /* prod / preview / dev tiers */
        expect_absent(yaml, ":8554", "capture-node prod rtsp");
        expect_absent(yaml, ":8000", "capture-node prod rtp");
        expect_absent(yaml, ":8001", "capture-node prod rtcp");
        expect_absent(yaml, ":18554", "capture-node preview rtsp");
        expect_absent(yaml, ":28554", "capture-node dev rtsp");
    }

    cfg.lan = true;
    if (gui_mediamtx_render_config(&cfg, yaml, sizeof yaml) > 0) {
        expect_absent(yaml, ":8554", "capture-node prod rtsp (lan)");
        expect_absent(yaml, ":18554", "capture-node preview rtsp (lan)");
        expect_absent(yaml, ":28554", "capture-node dev rtsp (lan)");
    }

    if (failures == before) puts("PASS: never emits a capture-node port");
    return failures - before;
}

/* mediamtx defaults rtmp/srt/moq to ON. Omitting them would claim
 * 1935/8890/8892 -- ports capture-node deliberately leaves free. */
static int test_disables_every_listener_we_do_not_serve(void)
{
    char yaml[8192];
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
    int before = failures;

    gui_mediamtx_render_config(&cfg, yaml, sizeof yaml);

    expect_contains(yaml, "rtmp: no", "rtmp disabled");
    expect_contains(yaml, "srt: no", "srt disabled");
    expect_contains(yaml, "moq: no", "moq disabled");
    /* udpMulticast would bind multicastRTPPort 8002 / multicastRTCPPort 8003. */
    expect_contains(yaml, "rtspTransports: [udp, tcp]", "multicast transport excluded");
    expect_absent(yaml, "multicast", "no multicast transport");
    /* RTSPS would bind 8322. */
    expect_contains(yaml, "rtspEncryption: \"no\"", "rtsps disabled");

    if (failures == before) puts("PASS: disables every listener we do not serve");
    return failures - before;
}

static int test_loopback_binds_localhost_on_all_six(void)
{
    char yaml[8192];
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
    int before = failures;

    gui_mediamtx_render_config(&cfg, yaml, sizeof yaml);

    expect_contains(yaml, "rtspAddress: 127.0.0.1:8654", "rtsp address");
    expect_contains(yaml, "rtpAddress: 127.0.0.1:8100", "rtp address");
    expect_contains(yaml, "rtcpAddress: 127.0.0.1:8101", "rtcp address");
    expect_contains(yaml, "hlsAddress: 127.0.0.1:8988", "hls address");
    expect_contains(yaml, "webrtcAddress: 127.0.0.1:8989", "webrtc http address");
    expect_contains(yaml, "webrtcLocalUDPAddress: 127.0.0.1:8289", "webrtc ice address");

    /* Loopback must not advertise LAN ICE candidates for a server nothing
     * off-box can reach. */
    expect_contains(yaml, "webrtcIPsFromInterfaces: no", "ice gathering off in loopback");
    expect_contains(yaml, "webrtcAdditionalHosts: [127.0.0.1]", "loopback ice host");

    if (failures == before) puts("PASS: loopback binds localhost on all six");
    return failures - before;
}

static int test_lan_binds_all_interfaces_and_gathers_ice(void)
{
    char yaml[8192];
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
    cfg.lan = true;
    int before = failures;

    gui_mediamtx_render_config(&cfg, yaml, sizeof yaml);

    expect_contains(yaml, "rtspAddress: :8654", "rtsp address");
    expect_contains(yaml, "rtpAddress: :8100", "rtp address");
    expect_contains(yaml, "rtcpAddress: :8101", "rtcp address");
    expect_contains(yaml, "hlsAddress: :8988", "hls address");
    expect_contains(yaml, "webrtcAddress: :8989", "webrtc http address");
    expect_contains(yaml, "webrtcLocalUDPAddress: :8289", "webrtc ice address");

    /* Remote browsers need a reachable candidate. */
    expect_contains(yaml, "webrtcIPsFromInterfaces: yes", "ice gathering on in lan");
    expect_absent(yaml, "webrtcAdditionalHosts", "no pinned loopback host in lan");
    /* Assert the BIND directives specifically. A bare "127.0.0.1" search also
     * matches the publish allow-list, which is loopback-only by design in both
     * modes -- this test is about what mediamtx listens on, not who may reach
     * it. */
    expect_absent(yaml, "rtspAddress: 127.0.0.1", "no loopback rtsp bind in lan mode");
    expect_absent(yaml, "rtpAddress: 127.0.0.1", "no loopback rtp bind in lan mode");
    expect_absent(yaml, "rtcpAddress: 127.0.0.1", "no loopback rtcp bind in lan mode");
    expect_absent(yaml, "hlsAddress: 127.0.0.1", "no loopback hls bind in lan mode");
    expect_absent(yaml, "webrtcAddress: 127.0.0.1", "no loopback webrtc bind in lan mode");
    expect_absent(yaml, "webrtcLocalUDPAddress: 127.0.0.1",
                  "no loopback webrtc ice bind in lan mode");

    if (failures == before) puts("PASS: lan binds all interfaces and gathers ice");
    return failures - before;
}

/* Ports must come from the config, not be baked into the template -- otherwise
 * the "change the RTSP port" setting would silently do nothing. */
static int test_ports_come_from_config(void)
{
    char yaml[8192];
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
    cfg.rtsp = 9001; cfg.rtp = 9002; cfg.rtcp = 9003;
    cfg.hls = 9004; cfg.webrtc_http = 9005; cfg.webrtc_ice = 9006;
    int before = failures;

    gui_mediamtx_render_config(&cfg, yaml, sizeof yaml);

    expect_contains(yaml, "rtspAddress: 127.0.0.1:9001", "custom rtsp");
    expect_contains(yaml, "rtpAddress: 127.0.0.1:9002", "custom rtp");
    expect_contains(yaml, "rtcpAddress: 127.0.0.1:9003", "custom rtcp");
    expect_contains(yaml, "hlsAddress: 127.0.0.1:9004", "custom hls");
    expect_contains(yaml, "webrtcAddress: 127.0.0.1:9005", "custom webrtc http");
    expect_contains(yaml, "webrtcLocalUDPAddress: 127.0.0.1:9006", "custom webrtc ice");
    expect_absent(yaml, "8654", "no baked-in default rtsp port");

    if (failures == before) puts("PASS: ports come from config");
    return failures - before;
}

static int test_publishes_the_expected_path(void)
{
    char yaml[8192];
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
    int before = failures;

    gui_mediamtx_render_config(&cfg, yaml, sizeof yaml);

    expect_contains(yaml, "paths:", "paths block");
    expect_contains(yaml, "misrc-preview:", "the stream path");
    expect_contains(yaml, "source: publisher", "publisher source");
    /* Outside capture-node's VCR0/CAM0 naming, so bookmarks never collide. */
    expect_absent(yaml, "VCR0", "no capture-node path name");
    expect_absent(yaml, "CAM0", "no capture-node path name");

    if (failures == before) puts("PASS: publishes the expected path");
    return failures - before;
}

/* The hole this file exists to keep shut.
 *
 * mediamtx's shipped default is `user: any` with publish, read AND playback on
 * every path. With no authInternalUsers block, LAN mode let any machine on the
 * network publish to misrc-preview and displace our ffmpeg -- replacing what
 * viewers see, and what the operator sees while monitoring a customer's tape,
 * with arbitrary video.
 *
 * Our publisher always connects over loopback (gui_rtsp_stream.c hard-codes
 * rtsp://127.0.0.1 regardless of bind mode), so an IP restriction is enough and
 * costs no credential -- which matters, because a credential in ffmpeg's argv
 * would be readable from /proc/<pid>/cmdline by any local user. */
static int test_publishing_is_restricted_to_loopback(void)
{
    int before = failures;

    for (int lan = 0; lan <= 1; lan++) {
        char yaml[8192];
        gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
        cfg.lan = (lan != 0);

        gui_mediamtx_render_config(&cfg, yaml, sizeof yaml);

        expect_contains(yaml, "authInternalUsers:", "an auth block at all");
        expect_contains(yaml, "action: publish", "an explicit publish permission");
        expect_contains(yaml, "ips: [\"127.0.0.1\", \"::1\"]",
                        "publish restricted to loopback, quoted so it parses");
        /* Readers are deliberately unrestricted: passwordless by default is the
         * documented behaviour, and LAN mode exists to let people watch. */
        expect_contains(yaml, "action: read", "an explicit read permission");
        expect_contains(yaml, "ips: []", "readers not restricted by address");
    }

    if (failures == before) puts("PASS: publishing is restricted to loopback");
    return failures - before;
}

/* Even from loopback, nothing should be able to take the path out from under
 * the publisher that owns it. */
static int test_never_lets_a_publisher_be_displaced(void)
{
    char yaml[8192];
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
    int before = failures;

    gui_mediamtx_render_config(&cfg, yaml, sizeof yaml);

    expect_contains(yaml, "overridePublisher: no", "publisher cannot be displaced");
    expect_absent(yaml, "overridePublisher: yes", "publisher displacement disabled");

    if (failures == before) puts("PASS: never lets a publisher be displaced");
    return failures - before;
}

/* Every reader costs bandwidth and CPU on the machine whose RF ingest we go out
 * of our way to protect with nice(+5). mediamtx reads 0 as "no limit". */
static int test_caps_the_number_of_readers(void)
{
    char yaml[8192];
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
    int before = failures;

    gui_mediamtx_render_config(&cfg, yaml, sizeof yaml);

    expect_contains(yaml, "maxReaders: 8", "a finite reader cap");
    expect_absent(yaml, "maxReaders: 0", "no unlimited readers");

    if (failures == before) puts("PASS: caps the number of readers");
    return failures - before;
}

/* These four default to "no" today. So did rtmp/srt/moq -- except those default
 * to YES, which is exactly why this file exists. A default is not a guarantee,
 * and pprof reachable on a LAN interface would be a genuine leak. */
static int test_admin_endpoints_are_explicitly_disabled(void)
{
    int before = failures;

    for (int lan = 0; lan <= 1; lan++) {
        char yaml[8192];
        gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
        cfg.lan = (lan != 0);

        gui_mediamtx_render_config(&cfg, yaml, sizeof yaml);

        expect_contains(yaml, "api: no", "control api disabled");
        expect_contains(yaml, "metrics: no", "metrics endpoint disabled");
        expect_contains(yaml, "pprof: no", "pprof endpoint disabled");
        expect_contains(yaml, "playback: no", "recording playback disabled");
    }

    if (failures == before) puts("PASS: admin endpoints are explicitly disabled");
    return failures - before;
}

/* A truncated config would be a mediamtx that silently starts with defaults --
 * including every listener we meant to disable. */
static int test_rejects_a_buffer_that_cannot_hold_the_config(void)
{
    char tiny[64];
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
    int before = failures;

    expect_eq_int(gui_mediamtx_render_config(&cfg, tiny, sizeof tiny), -1,
                  "render into a too-small buffer");

    if (failures == before) puts("PASS: rejects a buffer that cannot hold the config");
    return failures - before;
}

/* Renders a config to stdout so ci_guard_tests.py can hand it to a real YAML
 * parser. The assertions below match substrings, which cannot tell a valid
 * document from an invalid one -- the unquoted "::1" that broke this file's
 * first draft passed every strstr check while making mediamtx unable to read
 * the config at all. */
static int dump_config(int lan)
{
    char yaml[8192];
    gui_mediamtx_config_t cfg = gui_mediamtx_default_config();
    cfg.lan = (lan != 0);
    if (gui_mediamtx_render_config(&cfg, yaml, sizeof yaml) < 0) {
        fprintf(stderr, "render failed\n");
        return 1;
    }
    fputs(yaml, stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--dump") == 0) {
        return dump_config(argc > 2 && strcmp(argv[2], "lan") == 0);
    }

    test_default_ports_are_canonical_plus_100();
    test_never_emits_a_capture_node_port();
    test_disables_every_listener_we_do_not_serve();
    test_loopback_binds_localhost_on_all_six();
    test_lan_binds_all_interfaces_and_gathers_ice();
    test_ports_come_from_config();
    test_publishes_the_expected_path();
    test_publishing_is_restricted_to_loopback();
    test_never_lets_a_publisher_be_displaced();
    test_caps_the_number_of_readers();
    test_admin_endpoints_are_explicitly_disabled();
    test_rejects_a_buffer_that_cannot_hold_the_config();

    if (failures != 0) {
        fprintf(stderr, "FAILED: %d mediamtx config assertion(s)\n", failures);
        return 1;
    }
    puts("PASS: mediamtx config harness");
    return 0;
}
