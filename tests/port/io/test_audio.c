/* tests/port/io/test_audio.c - nnaudio.c on Linux: the SDL3 AudioOut path and the dlopen'ed libopus decoder.
 * The handlers are called as the game calls them (guest pointers are host pointers), with a quiet 440 Hz tone:
 *   1. Open (48 kHz stereo), 4 buffers of 512 frames appended, Start, then the game's loop: GetReleased -> refill ->
 *      Append, for 2 s of sound. Checks: buffers come back in append order, the device play position (put - queued)
 *      only grows and follows the wall clock, the run takes ~2 s, the guest event is signalled.
 *   2. Stop with buffers queued: every one is released at once, in order. Close.
 *   3. Opus: libopus.so.0 through opus_load; 10 packets encoded here (opus_encode, found in the same library) go through
 *      HardwareOpusDecoder Initialize / DecodeInterleaved (8-byte header) and must give back the tone.
 * SSBU_AUDIO_MUTE=1 runs part 1 clock-driven (no device). SSBU_VOLUME defaults to 15 here (the tone is audible on the device). */
#include "nnaudio.c"

extern volatile long io_events_signalled;
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
static unsigned long long g_phase;
static void fill_tone(int16_t *p, unsigned frames, int amp) {
    for (unsigned i = 0; i < frames; i++, g_phase++) { int16_t v = (int16_t)(amp * sin(6.283185307179586 * 440.0 * (double)g_phase / 48000.0)); p[i * 2] = p[i * 2 + 1] = v; }
}
static uint64_t P(const void *p) { return (uint64_t)(uintptr_t)p; }

#define NBUF 4
#define FRAMES 512
static int audio_test(void) {
    Ctx c;
    static uint8_t audio_out[64], event[64], bufobj[NBUF][0x28];
    static int16_t data[NBUF][FRAMES * 2];
    struct { uint32_t rate; uint16_t ch, r; } par = {48000, 2, 0};
    int mute = getenv("SSBU_AUDIO_MUTE") != NULL;
    memset(&c, 0, sizeof c);
    c.x[0] = P(audio_out); c.x[1] = P(event); c.x[2] = P(&par);
    audio_OpenDefaultAudioOut(&c);
    CHECK(c.x[0] == 0, "OpenDefaultAudioOut -> 0");
    if (!mute) CHECK(g_wave != NULL, "SDL stream open (driver %s)", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none");
    if (!mute && !g_wave) return 1;
    int fifo[256], fh = 0, ft = 0, appended = 0, released = 0, order_bad = 0, pos_back = 0;
    const int total = (2 * 48000 + FRAMES - 1) / FRAMES;          /* 2 s */
    for (int i = 0; i < NBUF; i++) {
        fill_tone(data[i], FRAMES, 4000);
        c.x[0] = P(bufobj[i]); c.x[1] = P(data[i]); c.x[2] = sizeof data[i]; c.x[3] = sizeof data[i];
        audio_SetAudioOutBufferInfo(&c);
        c.x[0] = P(audio_out); c.x[1] = P(bufobj[i]);
        audio_AppendAudioOutBuffer(&c);
        if (c.x[0] == 1) { fifo[ft++ % 256] = i; appended++; }
    }
    CHECK(appended == NBUF, "4 buffers appended before Start");
    long ev0 = io_events_signalled;
    c.x[0] = P(audio_out); audio_StartAudioOut(&c);
    double t0 = now_s(), t_last = 0, off_min = 1e9, off_max = -1e9;
    unsigned long long last_pos = 0;
    while (released < total && now_s() - t0 < 6.0) {
        c.x[0] = P(audio_out);
        audio_GetReleasedAudioOutBuffer(&c);
        uint64_t r = c.x[0];
        if (!mute) {                                                /* play position = put - queued, frames */
            EnterCriticalSection(&g_lock);
            unsigned long long pos = sdl_consumed() / 4;
            LeaveCriticalSection(&g_lock);
            if (pos < last_pos) pos_back++;
            last_pos = pos;
            double el = now_s() - t0;
            if (el > 0.2 && appended < total) { double off = (double)pos - el * 48000.0; if (off < off_min) off_min = off; if (off > off_max) off_max = off; }
        }
        if (!r) { usleep(1000); continue; }
        int want = fifo[fh++ % 256];
        if (r != P(bufobj[want])) order_bad++;
        released++;
        t_last = now_s() - t0;
        if (appended < total) {
            int i = (int)((uint8_t (*)[0x28])(uintptr_t)r - bufobj);
            fill_tone(data[i], FRAMES, 4000);
            c.x[0] = P(audio_out); c.x[1] = r;
            audio_AppendAudioOutBuffer(&c);
            if (c.x[0] == 1) { fifo[ft++ % 256] = i; appended++; }
        }
    }
    printf("%s: %d buffers (%.3f s of sound) appended, %d released in %.3f s after Start\n", mute ? "clock-driven" : "SDL", appended, appended * (double)FRAMES / 48000, released, t_last);
    CHECK(released == total, "every buffer released");
    CHECK(order_bad == 0, "released in append order (%d out of order)", order_bad);
    CHECK(t_last > 1.70 && t_last < 2.40, "2 s of buffers took %.3f s of real time", t_last);
    CHECK(io_events_signalled > ev0, "guest event signalled (%ld times)", io_events_signalled - ev0);
    if (!mute) {
        printf("play position - wall clock * 48000 while streaming: %.0f .. %.0f frames (spread %.1f ms)\n", off_min, off_max, (off_max - off_min) / 48.0);
        CHECK(pos_back == 0, "play position never goes back");
        CHECK(off_max - off_min < 0.060 * 48000, "play position follows the wall clock (spread under 60 ms)");
        EnterCriticalSection(&g_lock);
        unsigned long long pos = sdl_consumed() / 4, put = g_bytes_put / 4;
        LeaveCriticalSection(&g_lock);
        CHECK(pos == put && put == (unsigned long long)total * FRAMES, "position at the end = frames put = %llu", put);
    }
    /* Stop with buffers queued */
    for (int i = 0; i < NBUF; i++) {
        fill_tone(data[i], FRAMES, 4000);
        c.x[0] = P(audio_out); c.x[1] = P(bufobj[i]);
        audio_AppendAudioOutBuffer(&c);
        if (c.x[0] == 1) fifo[ft++ % 256] = i;
    }
    c.x[0] = P(audio_out); audio_StopAudioOut(&c);
    int got = 0; order_bad = 0;
    for (;;) {
        c.x[0] = P(audio_out); audio_GetReleasedAudioOutBuffer(&c);
        if (!c.x[0]) break;
        if (c.x[0] != P(bufobj[fifo[fh++ % 256]])) order_bad++;
        got++;
    }
    CHECK(got == NBUF && order_bad == 0, "StopAudioOut released the %d queued buffers at once, in order (%d)", NBUF, got);
    c.x[0] = P(audio_out); audio_CloseAudioOut(&c);
    audio_report(stdout);
    return 0;
}

static int opus_test(void) {
    Ctx c;
    static uint8_t obj[0x20];
    memset(&c, 0, sizeof c);
    c.x[0] = P(obj); opus_constructor(&c);
    CHECK(rd64(P(obj)) == P(g_opus_vtable), "HardwareOpusDecoder constructor installs the vtable");
    c.x[0] = P(obj); c.x[1] = 48000; c.x[2] = 2; opus_GetWorkBufferSize(&c);
    size_t size = (size_t)c.x[0];
    CHECK(g_opus_dll != NULL && size > 0, "libopus loaded by dlopen, work buffer %zu bytes", size);
    if (!g_opus_dll) return 1;
    void *(*enc_create)(int32_t, int, int, int *) = (void *(*)(int32_t, int, int, int *))(void *)GetProcAddress(g_opus_dll, "opus_encoder_create");
    int32_t (*enc)(void *, const int16_t *, int, unsigned char *, int32_t) = (int32_t (*)(void *, const int16_t *, int, unsigned char *, int32_t))(void *)GetProcAddress(g_opus_dll, "opus_encode");
    const char *(*ver)(void) = (const char *(*)(void))(void *)GetProcAddress(g_opus_dll, "opus_get_version_string");
    CHECK(enc_create && enc, "opus_encoder_create / opus_encode found (%s)", ver ? ver() : "?");
    if (!enc_create || !enc) return 1;
    void *buf = malloc(size);
    c.x[0] = P(obj); c.x[1] = 48000; c.x[2] = 2; c.x[3] = P(buf); c.x[4] = size; opus_Initialize(&c);
    CHECK(c.x[0] == 0 && rd64(P(obj) + OBJ_STATE) == P(buf), "Initialize(48000, 2) -> decoder in the work buffer");
    int err = 0;
    void *e = enc_create(48000, 2, 2049 /* OPUS_APPLICATION_AUDIO */, &err);
    enum { PK = 10, PF = 960 };
    static int16_t src[PK * PF * 2], out[PK * PF * 2];
    g_phase = 0;
    fill_tone(src, PK * PF, 8000);
    int bad = 0, frames_total = 0;
    for (int k = 0; k < PK; k++) {
        unsigned char in[1500 + 8];
        int32_t len = enc(e, src + k * PF * 2, PF, in + 8, 1500);
        if (len <= 0) { bad++; continue; }
        in[0] = (unsigned char)(len >> 24); in[1] = (unsigned char)(len >> 16); in[2] = (unsigned char)(len >> 8); in[3] = (unsigned char)len;
        memset(in + 4, 0, 4);
        uint64_t consumed = 0; int32_t count = 0;
        c.x[0] = P(obj); c.x[1] = P(&consumed); c.x[2] = P(&count); c.x[3] = P(out + frames_total * 2); c.x[4] = sizeof out - (size_t)frames_total * 4;
        c.x[5] = P(in); c.x[6] = (uint64_t)len + 8;
        opus_DecodeInterleaved(&c);
        if (c.x[0] != 0 || consumed != (uint64_t)len + 8 || count != PF) bad++;
        frames_total += count;
    }
    CHECK(bad == 0 && frames_total == PK * PF, "10 packets of 20 ms: DecodeInterleaved -> 0, consumed = packet + 8, 960 frames each");
    /* the tone comes back: Goertzel at 440 Hz over the last 160 ms (left channel) vs the total power */
    double s1 = 0, s2 = 0, pw = 0, ps = 0, w = 6.283185307179586 * 440.0 / 48000.0;
    int n0 = 2 * PF, n = PK * PF - n0;
    for (int i = n0; i < PK * PF; i++) { double x = out[i * 2], s0 = x + 2 * cos(w) * s1 - s2; s2 = s1; s1 = s0; pw += x * x; ps += (double)src[i * 2] * src[i * 2]; }
    double tone = (s1 * s1 + s2 * s2 - 2 * cos(w) * s1 * s2) * 2.0 / n;
    printf("opus: decoded power %.3g (source %.3g), 440 Hz share %.3f\n", pw / n, ps / n, pw ? tone / pw : 0.0);
    CHECK(pw > 0.5 * ps && pw < 1.5 * ps && tone / pw > 0.9, "decoded signal is the 440 Hz tone at the right level");
    c.x[0] = P(obj); opus_Reset(&c);
    c.x[0] = P(obj); opus_Finalize(&c);
    free(buf);
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!getenv("SSBU_VOLUME")) setenv("SSBU_VOLUME", "15", 1);
    register_audio_imports();
    audio_test();
    opus_test();
    printf("%s\n", g_fail ? "AUDIO TEST FAILED" : "audio test ok");
    return g_fail != 0;
}
