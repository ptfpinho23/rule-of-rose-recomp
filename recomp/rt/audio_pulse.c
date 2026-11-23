/* audio_pulse.c — sound device for Linux (plat.h adev_*): PulseAudio.
 *
 * There is no libpulse on this system, so the PulseAudio "native" protocol
 * is spoken directly over the server's unix socket ($PULSE_SERVER, else
 * $XDG_RUNTIME_DIR/pulse/native, else one fixed path some set-ups use).
 * Only the four things needed are implemented:
 * AUTH, SET_CLIENT_NAME, CREATE_PLAYBACK_STREAM (protocol version 12
 * layouts) and stream data packets; from the server only REQUEST is read.
 * The socket is non-blocking after the handshake: audio never stalls the
 * emulation, at worst a frame of sound is dropped. Without a server there is
 * one log line and silence. */
#ifndef _WIN32
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>
#include "rt.h"
#include "plat.h"

/* The build has glibc headers but no kernel headers, so <errno.h> and
 * <sys/socket.h> cannot be included; the few Linux/x86-64 definitions needed
 * are declared here. */
extern int *__errno_location(void);
#define errno (*__errno_location())
#define EINTR 4
#define EAGAIN 11
#define AF_UNIX 1
#define SOCK_STREAM 1
#define SOL_SOCKET 1
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define MSG_NOSIGNAL 0x4000
struct sockaddr_un { unsigned short sun_family; char sun_path[108]; };
int socket(int domain, int type, int protocol);
int connect(int fd, const void *addr, unsigned len);
int setsockopt(int fd, int level, int name, const void *val, unsigned len);
ssize_t send(int fd, const void *buf, size_t n, int flags);
ssize_t recv(int fd, void *buf, size_t n, int flags);

#define RATE 48000
#define FRAMES_PER_CALL 960
#define FRAME_BYTES (FRAMES_PER_CALL * 4)

/* ---- PulseAudio native protocol --------------------------------------- */
enum { PA_ERROR = 0, PA_REPLY = 2, PA_CREATE_PLAYBACK_STREAM = 3, PA_AUTH = 8, PA_SET_CLIENT_NAME = 9, PA_REQUEST = 61 };
#define PA_VERSION 12
#define PA_TLENGTH (RATE * 4 / 10)          /* 100 ms target buffer */

static int pa_fd = -1;
static u32 pa_channel;
static s64 pa_credit;                        /* bytes the server has asked for and not yet received */
static u8  pa_out[1 << 16]; static u32 pa_out_n;        /* unsent bytes (whole packets) */
static u8  pa_in[4096]; static u32 pa_in_n;
static u32 pa_dropped, pa_sent, pa_requested;

static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

typedef struct { u8 b[1024]; u32 n; } Tag;
static void t_u32(Tag *t, u32 v) { t->b[t->n++] = 'L'; put32(t->b + t->n, v); t->n += 4; }
static void t_str(Tag *t, const char *s) { if (!s) { t->b[t->n++] = 'N'; return; } t->b[t->n++] = 't'; size_t l = strlen(s) + 1; memcpy(t->b + t->n, s, l); t->n += (u32)l; }
static void t_bool(Tag *t, int v) { t->b[t->n++] = v ? '1' : '0'; }
static void t_blob(Tag *t, const void *d, u32 len) { t->b[t->n++] = 'x'; put32(t->b + t->n, len); t->n += 4; memcpy(t->b + t->n, d, len); t->n += len; }

static void pa_close(const char *why) {
    if (pa_fd >= 0) close(pa_fd);
    pa_fd = -1;
    if (why) rt_log("[audio] %s; continuing without sound output\n", why);
}
static int write_all(const u8 *p, u32 n) {
    while (n) {
        ssize_t r = send(pa_fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return 0; }
        p += r; n -= (u32)r;
    }
    return 1;
}
static int read_all(u8 *p, u32 n) {
    while (n) {
        ssize_t r = recv(pa_fd, p, n, 0);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return 0; }
        p += r; n -= (u32)r;
    }
    return 1;
}
/* Send a control packet and wait for its reply (handshake only, blocking
 * with the socket timeouts). Returns the reply payload length, 0 on failure. */
static u32 pa_call(Tag *t, u32 tag, u8 *reply, u32 cap) {
    u8 d[20];
    put32(d, t->n); put32(d + 4, 0xFFFFFFFFu); put32(d + 8, 0); put32(d + 12, 0); put32(d + 16, 0);
    if (!write_all(d, 20) || !write_all(t->b, t->n)) return 0;
    for (;;) {
        if (!read_all(d, 20)) return 0;
        u32 len = get32(d);
        if (len > cap) return 0;
        if (!read_all(reply, len)) return 0;
        if (get32(d + 4) != 0xFFFFFFFFu || len < 10 || reply[0] != 'L' || reply[5] != 'L') continue;
        u32 cmd = get32(reply + 1), rtag = get32(reply + 6);
        if (cmd == PA_REQUEST) continue;
        if (rtag != tag) continue;
        if (cmd != PA_REPLY) { rt_log("[audio] PulseAudio error %u for request %u\n", len >= 15 ? get32(reply + 11) : 0, tag); return 0; }
        return len;
    }
}
void adev_open(void) {
    const char *srv = getenv("PULSE_SERVER");
    char path[108] = "";
    if (srv && *srv) {
        if (!strncmp(srv, "unix:", 5)) srv += 5;
        if (*srv == '/') snprintf(path, sizeof path, "%s", srv);
    }
    if (!path[0]) {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (xdg && *xdg) { snprintf(path, sizeof path, "%s/pulse/native", xdg); if (access(path, F_OK)) path[0] = 0; }
    }
    if (!path[0] && !access("/mnt/wslg/PulseServer", F_OK)) snprintf(path, sizeof path, "/mnt/wslg/PulseServer");
    if (!path[0]) { rt_log("[audio] no PulseAudio server found ($PULSE_SERVER unset); no sound output\n"); return; }

    pa_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (pa_fd < 0) { pa_close("cannot create a socket"); return; }
    struct timeval tv = { 1, 0 };
    setsockopt(pa_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(pa_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (connect(pa_fd, &sa, sizeof sa) < 0) {
        rt_log("[audio] cannot connect to PulseAudio at %s (%s)\n", path, strerror(errno));
        pa_close(0); return;
    }
    u8 cookie[256] = { 0 }, reply[2048];
    char cpath[512] = "";
    const char *ck = getenv("PULSE_COOKIE"), *home = getenv("HOME");
    if (ck && *ck) snprintf(cpath, sizeof cpath, "%s", ck);
    else if (home) snprintf(cpath, sizeof cpath, "%s/.config/pulse/cookie", home);
    FILE *cf = cpath[0] ? fopen(cpath, "rb") : 0;
    if (cf) { if (fread(cookie, 1, 256, cf) != 256) memset(cookie, 0, 256); fclose(cf); }

    Tag t = { .n = 0 };
    t_u32(&t, PA_AUTH); t_u32(&t, 1); t_u32(&t, PA_VERSION); t_blob(&t, cookie, 256);
    u32 n = pa_call(&t, 1, reply, sizeof reply);
    if (!n) { pa_close("PulseAudio refused the connection (AUTH)"); return; }
    u32 server_version = n >= 15 ? get32(reply + 11) & 0xFFFF : 0;

    t.n = 0;
    t_u32(&t, PA_SET_CLIENT_NAME); t_u32(&t, 2); t_str(&t, "Rule of Rose");
    if (!pa_call(&t, 2, reply, sizeof reply)) { pa_close("PulseAudio SET_CLIENT_NAME failed"); return; }

    t.n = 0;
    t_u32(&t, PA_CREATE_PLAYBACK_STREAM); t_u32(&t, 3);
    t_str(&t, "game audio");
    t.b[t.n++] = 'a'; t.b[t.n++] = 3 /* S16LE */; t.b[t.n++] = 2; put32(t.b + t.n, RATE); t.n += 4;
    t.b[t.n++] = 'm'; t.b[t.n++] = 2; t.b[t.n++] = 1 /* front left */; t.b[t.n++] = 2 /* front right */;
    t_u32(&t, 0xFFFFFFFFu);                 /* sink index: default */
    t_str(&t, 0);                           /* sink name: default */
    t_u32(&t, 0xFFFFFFFFu);                 /* maxlength: server default */
    t_bool(&t, 0);                          /* not corked */
    t_u32(&t, PA_TLENGTH);                  /* tlength */
    t_u32(&t, 0xFFFFFFFFu);                 /* prebuf: default */
    t_u32(&t, 0xFFFFFFFFu);                 /* minreq: default */
    t_u32(&t, 0);                           /* sync id */
    t.b[t.n++] = 'v'; t.b[t.n++] = 2; put32(t.b + t.n, 0x10000); t.n += 4; put32(t.b + t.n, 0x10000); t.n += 4;
    for (int i = 0; i < 7; i++) t_bool(&t, 0);      /* no_remap, no_remix, fix_format, fix_rate, fix_channels, no_move, variable_rate */
    n = pa_call(&t, 3, reply, sizeof reply);
    if (n < 25) { pa_close("PulseAudio could not create a playback stream"); return; }
    pa_channel = get32(reply + 11);
    pa_credit = get32(reply + 21);
    fcntl(pa_fd, F_SETFL, fcntl(pa_fd, F_GETFL) | O_NONBLOCK);
    rt_log("[audio] PulseAudio output at %s (server protocol %u), 48 kHz stereo\n", path, server_version);
}
/* Read whatever the server sent; only REQUEST matters. */
static void pa_poll(void) {
    for (;;) {
        ssize_t r = recv(pa_fd, pa_in + pa_in_n, sizeof pa_in - pa_in_n, 0);
        if (r == 0) { pa_close("PulseAudio closed the connection"); return; }
        if (r < 0) { if (errno == EAGAIN || errno == EINTR) break; pa_close("PulseAudio connection error"); return; }
        pa_in_n += (u32)r;
        for (;;) {
            if (pa_in_n < 20) break;
            u32 len = get32(pa_in);
            if (len > sizeof pa_in - 20) { pa_close("unexpected large packet from PulseAudio"); return; }
            if (pa_in_n < 20 + len) break;
            const u8 *p = pa_in + 20;
            if (get32(pa_in + 4) == 0xFFFFFFFFu && len >= 20 && p[0] == 'L' && get32(p + 1) == PA_REQUEST && p[10] == 'L' && p[15] == 'L')
                { pa_credit += get32(p + 16); pa_requested += get32(p + 16); }
            memmove(pa_in, pa_in + 20 + len, pa_in_n - 20 - len); pa_in_n -= 20 + len;
        }
    }
}
static void pa_flush(void) {
    while (pa_out_n) {
        ssize_t r = send(pa_fd, pa_out, pa_out_n, MSG_NOSIGNAL);
        if (r < 0) { if (errno == EAGAIN || errno == EINTR) return; pa_close("PulseAudio connection lost"); return; }
        memmove(pa_out, pa_out + r, pa_out_n - (u32)r); pa_out_n -= (u32)r;
    }
}
void adev_play(const s16 *pcm) {
    if (pa_fd < 0) return;
    pa_poll();
    if (pa_fd < 0) return;
    pa_flush();
    if (pa_fd < 0) return;
    /* Drop the frame if the server already holds much more than it asked for
     * (the game is running ahead of the sound card) or the socket is backed up. */
    if (pa_credit < -(s64)(2 * PA_TLENGTH) || pa_out_n + 20 + FRAME_BYTES > sizeof pa_out) { pa_dropped++; return; }
    u8 *d = pa_out + pa_out_n;
    put32(d, FRAME_BYTES); put32(d + 4, pa_channel); put32(d + 8, 0); put32(d + 12, 0); put32(d + 16, 0);
    memcpy(d + 20, pcm, FRAME_BYTES);
    pa_out_n += 20 + FRAME_BYTES;
    pa_credit -= FRAME_BYTES; pa_sent++;
    pa_flush();
}

void adev_close(void) {
    if (pa_fd < 0) return;
    if (getenv("ROR_AUDIODBG")) rt_log("[audio] sent %u frames (20 ms each), dropped %u, server requested %u bytes after start, credit %lld\n", pa_sent, pa_dropped, pa_requested, (long long)pa_credit);
    /* let queued sound leave the process; the server plays out what it has */
    fcntl(pa_fd, F_SETFL, fcntl(pa_fd, F_GETFL) & ~O_NONBLOCK);
    if (pa_out_n) write_all(pa_out, pa_out_n);
    pa_close(0);
}
#endif
