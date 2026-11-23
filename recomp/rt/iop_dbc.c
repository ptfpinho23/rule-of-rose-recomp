/* iop_dbc.c — controller service: DBCMAN.IRX + PAD2/DS1O_D.IRX, native.
 *
 * Everything below was recovered from the game's EE libraries (libdbc 0x26bec8-
 * 0x26d0f0, libpad2 0x26d118-0x26db3c in SLES_542.18) and from the two IOP
 * modules on the disc (addresses "dbcman:0x..." / "ds1o:0x..." are offsets in
 * the .text of MODULES/DBCMAN.IRX and MODULES/PAD2/DS1O_D.IRX).
 *
 * ---- Layers ---------------------------------------------------------------
 * DBCMAN is a generic "device bus controller": drivers register devices
 * ("dep", 16 max), the EE creates sockets (16 max), and DBCMAN links a socket
 * to a connected device whose type/port/slot match. DS1O_D is the driver for
 * DualShock-1-compatible pads: it registers one device per port (type 1),
 * runs the usual config sequence over SIO2 (0x43, 0x45, 0x46, 0x47, 0x4C,
 * 0x44 analog+lock, 0x4D; it never sends 0x4F, so a DualShock 2 ends up in
 * plain analog mode, ID 0x73, no pressure bytes) and then polls (0x42) once
 * per vblank (ds1o:0x1e4 thread, 0x6a0 poll, 0xf30 publish).
 *
 * ---- RPC servers (dbcman:0x1d94-0x1f94) -----------------------------------
 * 0x80001300  main server, handler dbcman:0x1c88, dispatch on fno (table at
 *             dbcman .rodata 0x2560, index fno-0x80001301). The handler
 *             returns its own receive buffer, so the reply is the request
 *             with some words overwritten. EE: send = recv = 144 bytes at
 *             0x84be40. Offsets below are in that buffer.
 *   0x80001363 version            -> [0] = 0x0310 (dbcman .data 0x2714). EE
 *                                    0x26bfd0 requires (v >> 4) == 0x31.
 *   0x80001304 SetWorkAddr        [4] = EE address of a 128-byte status area
 *                                 (game: 0x84e140) -> [0] = 1. The area is
 *                                 rewritten by SIF DMA whenever it changes:
 *                                 u32 status[16], 1 = socket linked to a
 *                                 connected device (dbcman:0xa4, 0x8d0).
 *   0x80001301 CreateSocket       [0] option, [4] device type, [8] port,
 *                                 [12] slot, [16] sub-type, [20..35] name,
 *                                 [40],[44] EE addresses of two 128-byte
 *                                 data buffers -> [36] = socket number, or
 *                                 -21 if none free (dbcman:0x54c, 0xf48).
 *                                 option bits select what must match the
 *                                 device (dbcman:0xc00): 1 type, 2 port+slot,
 *                                 4 sub-type, 8 name.
 *   0x80001302 DeleteSocket       [0] socket -> [4] = 1, or -22 if not in use.
 *   0x80001303 GetDepNumber       [0] socket -> [4] = device number, or -12
 *                                 if the socket is not linked.
 *   0x80001315 InitSocket         [0] socket -> [4] = 1; -22 bad socket,
 *   0x80001316 ResetSocket        -23 not linked (driver hooks are empty).
 *   0x80001317 GetDeviceStatus    -> [4] = 0 ("status function is not
 *                                 support", dbcman:0x718).
 *   0x8000131D (find socket)      [16] port, [20] slot ([8] is forced to 2)
 *                                 -> [4] = socket linked to the device at
 *                                 that port/slot; -12 no such device, -22
 *                                 device not linked (dbcman:0x624).
 *   0x8000131A ReceiveData        [0] socket, [4] command, [8] length ->
 *                                 [8] length, [12..] data, [140] result.
 *   0x80001319 SendData           [0] socket, [4] command, [8] length,
 *                                 [12..] data -> [140] result.
 *   0x80001318 SRData             [0] socket, [4] command, [8] send length,
 *                                 [12] recv length, [16..] send data ->
 *                                 [12] recv length, [144..] recv data, [272]
 *                                 result. The EE only fetches 144 bytes, so
 *                                 data and result never arrive (the library
 *                                 reads stale memory at 0x84bf50).
 *   other fno (e.g. 0x80001305, issued by the unused EE function 0x26c208):
 *                                 the buffer is returned unchanged.
 * 0x8000131E / 0x8000131F  SendData2 (handler dbcman:0x1b70), any fno, 144
 *             bytes, same layout as SendData. The EE alternates between the
 *             two servers in no-wait mode (0x26ccc0).
 * 0x8000131C  SendData3 (handler dbcman:0x1bcc), any fno, up to 8336 bytes,
 *             same layout with the result at [8332] (EE 0x26ce70).
 * Results of the data calls: -22 bad socket, -23 not linked, otherwise the
 * driver's return value.
 *
 * ---- Driver commands (ds1o:0x1b20) ----------------------------------------
 * command = type << 24 | class << 16 | (flags, ignored) | number (14 bits).
 * type must be 1 (else -1); unknown class -2; unknown number -3; success 1.
 *   class 1 number 2   get: u32 pad ID (0x73)                (4 bytes)
 *   class 1 number 12  get: u8 connection state (1 = stable) (1 byte)
 *   class 2 number 2   get: button profile (ff ff 0f 00)     (4 bytes)
 *   class 3 number 2   get: actuator profile (03)            (1 byte)
 *   class 3 number 11  set actuators. data = { u32 n1, u32 n2, value[n2],
 *                      mask[n1] }: mask 1 = small motor (value bit 0), 2 =
 *                      big motor (value byte), 3 = both (bit 0, then 8 bits).
 *                      Built by libpad2 0x26da50, called by the game at
 *                      0x101b50.
 * ReceiveData passes only output pointers and SendData only input pointers
 * to the driver, so "get" needs ReceiveData or SRData and "set" needs one
 * of the SendData calls or SRData; the other combinations dereference NULL
 * on the IOP.
 *
 * ---- Per-frame data (ds1o:0xf30 -> dbcSignalData dbcman:0x31c) ------------
 * Once per vblank the driver DMAs a 128-byte record straight into EE memory,
 * alternating between the socket's two buffers (index = counter & 1):
 *   [0]   connection state (1)        [1] data + profile length (10)
 *   [2]   data length (6)             [3] profile length (4)
 *   [4]   u32, 1 = last SIO2 transfer succeeded
 *   [28]  data: the six pad bytes that follow "ID, 0x5A" on the wire:
 *         buttons low, buttons high (active low), RX, RY, LX, LY
 *   [34]  button profile ff ff 0f 00 (buttons 0-15 one bit each, 16-19 bytes)
 *   [124] u32 frame counter
 * libpad2 picks the buffer with the larger counter (0x26d7b8). The game
 * (0x101ccc, every frame per pad object) calls scePad2GetState (0x26d4c8;
 * returns byte [0] when the counter moved, otherwise asks with ReceiveData
 * class 1 number 12) and, if that is 1, scePad2Read (0x26d320; copies the 6
 * data bytes). It uses bytes 0-1 as an inverted little-endian u16 and bytes
 * 2-5 as the sticks; it never asks for pressure or the button profile.
 * The game creates one socket per pad object with option 2 (libpad2 adds 1
 * and type 1), port 0 or 1, slot 0 (0x101bcc).
 *
 * ---- Model ----------------------------------------------------------------
 * One DualShock 2 in port 0 / slot 0, connected and configured before the
 * game creates its first socket (on hardware this takes about 15 vblanks
 * after the module starts). Nothing in port 1: its device stays registered
 * but unconnected, so a socket for it is created and never linked. */
#include "iop.h"

#define DBC_VERSION   0x0310u
#define PAD_ID        0x73u          /* analog mode, 3 data words */
#define MAX_SOCKETS   16
#define E_NOSOCKET    (-21)
#define E_BADSOCKET   (-22)
#define E_NOTLINKED   (-23)
#define E_NODEVICE    (-12)

typedef struct {
    int state;                       /* 0 free, 1 created, 2 linked (dbcman 0x2a40) */
    u32 option, type, port, slot, sub;
    u8  name[16];
    u32 addr[2];                     /* EE data buffers */
} Socket;

/* The one connected device (device number 0), as DS1O_D registers it
 * (ds1o:0x17f0: type 1, sub-type 0) and connects it (ds1o:0x18b4: port and
 * slot from SIO2D, empty name). */
enum { DEV_TYPE = 1, DEV_SUB = 0, DEV_PORT = 0, DEV_SLOT = 0, DEV_NUMBER = 0 };

static Socket sockets[MAX_SOCKETS];
static int dev_socket = -1;          /* socket linked to the device, or -1 */
static u32 status[MAX_SOCKETS];      /* copy published to the EE work area */
static u32 work_addr;
static u32 frame_counter;
static int ticking;

static u16 pad_buttons = 0xFFFF;
static u8  pad_sticks[4] = { 0x80, 0x80, 0x80, 0x80 };   /* RX, RY, LX, LY */
static u8  act_small, act_big;

/* Pad state as it would appear on the wire. `buttons` is active low (bit
 * clear = pressed), low byte = first button byte:
 *   0x0001 SELECT  0x0002 L3     0x0004 R3     0x0008 START
 *   0x0010 UP      0x0020 RIGHT  0x0040 DOWN   0x0080 LEFT
 *   0x0100 L2      0x0200 R2     0x0400 L1     0x0800 R1
 *   0x1000 TRIANGLE 0x2000 CIRCLE 0x4000 CROSS 0x8000 SQUARE
 * Sticks: 0 = left/up, 0x80 = centre, 0xFF = right/down. */
void pad_set_state(u16 buttons, u8 rx, u8 ry, u8 lx, u8 ly) {
    pad_buttons = buttons;
    pad_sticks[0] = rx; pad_sticks[1] = ry; pad_sticks[2] = lx; pad_sticks[3] = ly;
}
/* Last vibration request from the game: small motor 0/1, big motor 0..255. */
void pad_get_actuators(u8 *small_motor, u8 *big_motor) {
    *small_motor = act_small; *big_motor = act_big;
}

static void *ee_area(Ctx *c, u32 addr, const char *what) {
    if ((u64)(addr & 0x1FFFFFFFu) + 128 > RAM_SIZE) rt_fatal(c, "dbcman: %s 0x%08x outside RAM", what, addr);
    return gptr(addr);
}

/* dbcman:0xa4 — DMA the status table to the EE. The real transfer is 128
 * bytes of DBCMAN's .bss; the EE library reads the first 64 (0x26c4f8). The
 * rest (IOP-internal device table) is sent as zeros here. */
static void publish_status(Ctx *c) {
    if (!work_addr) return;
    u8 *p = ee_area(c, work_addr, "work area");
    memset(p, 0, 128);
    memcpy(p, status, sizeof status);
}

/* ds1o:0xf30 — one record per vblank into the linked socket's buffers. */
static void publish_data(void) {
    u32 n = frame_counter++;
    if (dev_socket < 0) return;
    u8 rec[128] = { 0 };
    rec[0] = 1; rec[1] = 6 + 4; rec[2] = 6; rec[3] = 4;
    wr32(rec + 4, 1);
    rec[28] = (u8)pad_buttons; rec[29] = (u8)(pad_buttons >> 8);
    memcpy(rec + 30, pad_sticks, 4);
    rec[34] = 0xFF; rec[35] = 0xFF; rec[36] = 0x0F; rec[37] = 0x00;
    wr32(rec + 124, n);
    memcpy(ee_area(0, sockets[dev_socket].addr[n & 1], "socket buffer"), rec, 128);
}
static void vblank_tick(void *arg) {
    (void)arg;
    publish_data();
    /* shortly after the next vblank start (the driver polls the pad first) */
    kernel_schedule((g_now / TICKS_PER_FRAME + 1) * TICKS_PER_FRAME + TICKS_PER_FRAME / 16, vblank_tick, 0);
}

/* dbcman:0xc00 — does the device satisfy the socket's match options? */
static int device_matches(const Socket *s) {
    if ((s->option & 1) && s->type != DEV_TYPE) return 0;
    if ((s->option & 2) && (s->port != DEV_PORT || s->slot != DEV_SLOT)) return 0;
    if ((s->option & 4) && s->sub != DEV_SUB) return 0;
    if ((s->option & 8) && s->name[0] != 0) return 0;    /* device name is "" */
    return 1;
}

static s32 create_socket(Ctx *c, const u8 *a) {
    int n = -1;
    for (int i = 0; i < MAX_SOCKETS; i++) if (sockets[i].state <= 0) { n = i; break; }
    if (n < 0) return E_NOSOCKET;
    Socket *s = &sockets[n];
    s->state = 1;
    s->option = rd32(a); s->type = rd32(a + 4); s->port = rd32(a + 8); s->slot = rd32(a + 12);
    s->sub = rd32(a + 16);
    memcpy(s->name, a + 20, 16);
    s->addr[0] = rd32(a + 40); s->addr[1] = rd32(a + 44);
    if (dev_socket < 0 && device_matches(s)) {           /* dbcman:0x154c */
        ee_area(c, s->addr[0], "socket buffer"); ee_area(c, s->addr[1], "socket buffer");
        s->state = 2; dev_socket = n;
        status[n] = 1;
        publish_status(c);
    }
    rt_log("[dbc] socket %d: option=%u type=%u port=%u slot=%u -> %s\n", n, s->option, s->type,
           s->port, s->slot, s->state == 2 ? "linked to the pad" : "no device");
    return n;
}
static s32 delete_socket(Ctx *c, u32 n) {
    /* dbcman:0x10e0 indexes its table without a range check */
    if (n >= MAX_SOCKETS) rt_fatal(c, "dbcman: DeleteSocket(%d) outside the socket table", (s32)n);
    if (sockets[n].state <= 0) return E_BADSOCKET;
    if ((int)n == dev_socket) dev_socket = -1;
    sockets[n].state = 0;
    status[n] = 0;
    publish_status(c);
    return 1;
}
/* dbcman:0x12f4 + 0x1698 — 1 if the socket can reach the driver */
static s32 socket_device(u32 n) {
    if (n >= MAX_SOCKETS || sockets[n].state <= 0) return E_BADSOCKET;
    if ((int)n != dev_socket) return E_NOTLINKED;
    return 1;
}

/* ds1o:0x14c4 — set actuators. `d` = { n1, n2, value[n2], mask[n1] }. */
static s32 set_actuators(Ctx *c, const u8 *d, u32 avail) {
    u32 n2 = rd32(d + 4);
    if (n2 > avail || 8 + n2 >= avail) rt_fatal(c, "dbcman: actuator request with value length %u", n2);
    switch (d[8 + n2]) {
    case 1: act_small = d[8] & 1; break;
    case 2: act_big = d[8]; break;
    case 3: act_small = d[8] & 1; act_big = (u8)((d[8] >> 1) | (d[9] << 7)); break;
    default: return -1;
    }
    return 1;
}
/* ds1o:0x1b20 — driver command. sdata/rdata are NULL when the calling RPC
 * does not provide that direction. */
static s32 driver_command(Ctx *c, u32 cmd, const u8 *sdata, u32 savail, u8 *rlen, u8 *rdata) {
    u32 type = cmd >> 24, cls = (cmd >> 16) & 0xFF, num = cmd & 0x3FFF;
    const u8 *out = 0; u32 n = 0;
    static const u8 id[4] = { PAD_ID, 0, 0, 0 }, state[1] = { 1 };
    static const u8 profile[4] = { 0xFF, 0xFF, 0x0F, 0x00 }, act_profile[1] = { 0x03 };
    if (type != 1) return -1;
    switch (cls) {
    case 1:
        if (num == 2) { out = id; n = 4; }
        else if (num == 12) { out = state; n = 1; }
        else return -3;
        break;
    case 2:
        if (num != 2) return -3;
        out = profile; n = 4;
        break;
    case 3:
        if (num == 2) { out = act_profile; n = 1; }
        else if (num == 11) {
            if (!sdata) rt_fatal(c, "dbcman: set-actuators command 0x%08x through ReceiveData (NULL data on the IOP)", cmd);
            return set_actuators(c, sdata, savail);
        } else return -3;
        break;
    default:
        return -2;
    }
    if (!rdata) rt_fatal(c, "dbcman: query command 0x%08x through SendData (NULL result pointer on the IOP)", cmd);
    wr32(rlen, n);
    memcpy(rdata, out, n);
    return 1;
}

/* SendData and its variants: { socket, command, length, data... } */
static s32 send_data(RpcCall *rc, u8 *b, u32 size) {
    s32 r = socket_device(rd32(b));
    if (r < 0) return r;
    return driver_command(rc->ctx, rd32(b + 4), b + 12, size - 12, 0, 0);
}

void dbc_call(RpcCall *rc) {
    if (!ticking) {
        ticking = 1;
        kernel_schedule((kernel_now(rc->ctx) / TICKS_PER_FRAME + 1) * TICKS_PER_FRAME + TICKS_PER_FRAME / 16, vblank_tick, 0);
    }
    u32 sid = rc->server->sid, fno = rc->fno;
    /* Every handler returns its request buffer; work on a full-size copy. */
    static u8 buf[8336];
    u32 size = rc->send_size;
    memset(buf, 0, sizeof buf);
    if (size > sizeof buf) rt_fatal(rc->ctx, "dbcman: request of %u bytes on server 0x%08x", size, sid);
    memcpy(buf, rc->send, size);
    if (g_verbose) rt_log("[dbc] server 0x%08x fn=0x%08x size=%u: %08x %08x %08x %08x\n", sid, fno, size,
                          rd32(buf), rd32(buf + 4), rd32(buf + 8), rd32(buf + 12));

    if (sid == 0x8000131Eu || sid == 0x8000131Fu) {          /* SendData2 */
        if (size != 144) rt_fatal(rc->ctx, "dbcman: SendData2 with %u bytes (the EE library always sends 144)", size);
        wr32(buf + 140, (u32)send_data(rc, buf, 140));
    } else if (sid == 0x8000131Cu) {                         /* SendData3 */
        if (size != 8336) rt_fatal(rc->ctx, "dbcman: SendData3 with %u bytes (the EE library always sends 8336)", size);
        wr32(buf + 8332, (u32)send_data(rc, buf, 8332));
    } else if (sid == 0x80001300u) {
        if (size != 144) rt_fatal(rc->ctx, "dbcman: fn 0x%08x with %u bytes (the EE library always sends 144)", fno, size);
        switch (fno) {
        case 0x80001363u: wr32(buf, DBC_VERSION); break;
        case 0x80001304u:
            work_addr = rd32(buf + 4);
            publish_status(rc->ctx);
            wr32(buf, 1);
            break;
        case 0x80001301u: wr32(buf + 36, (u32)create_socket(rc->ctx, buf)); break;
        case 0x80001302u: wr32(buf + 4, (u32)delete_socket(rc->ctx, rd32(buf))); break;
        case 0x80001303u:
            wr32(buf + 4, dev_socket >= 0 && rd32(buf) == (u32)dev_socket ? (u32)DEV_NUMBER : (u32)E_NODEVICE);
            break;
        case 0x80001315u: case 0x80001316u:
            wr32(buf + 4, (u32)socket_device(rd32(buf)));
            break;
        case 0x80001317u: wr32(buf + 4, 0); break;
        case 0x8000131Du:
            wr32(buf + 8, 2);
            if (rd32(buf + 16) != DEV_PORT || rd32(buf + 20) != DEV_SLOT) wr32(buf + 4, (u32)E_NODEVICE);
            else wr32(buf + 4, dev_socket >= 0 ? (u32)dev_socket : (u32)E_BADSOCKET);
            break;
        case 0x8000131Au: {                                  /* ReceiveData */
            s32 r = socket_device(rd32(buf));
            if (r >= 0) r = driver_command(rc->ctx, rd32(buf + 4), 0, 0, buf + 8, buf + 12);
            wr32(buf + 140, (u32)r);
            break;
        }
        case 0x80001319u: wr32(buf + 140, (u32)send_data(rc, buf, 140)); break;
        case 0x80001318u: {                                  /* SRData */
            /* Result (+272) and received data (+144) lie beyond the 144
             * bytes the EE fetches; only the received length is visible. */
            s32 r = socket_device(rd32(buf));
            u8 rdata[128];
            if (r >= 0) driver_command(rc->ctx, rd32(buf + 4), buf + 16, 128, buf + 12, rdata);
            break;
        }
        case 0x80001305u: break;                             /* not in the table: echoed */
        default:
            rt_fatal(rc->ctx, "dbcman: function 0x%08x is not issued by the game's libdbc", fno);
        }
    } else rt_fatal(rc->ctx, "dbc_call on unexpected server 0x%08x", sid);

    memcpy(rc->recv, buf, rc->recv_size < sizeof buf ? rc->recv_size : sizeof buf);
}
