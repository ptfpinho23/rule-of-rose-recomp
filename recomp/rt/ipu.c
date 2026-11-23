/* ipu.c — Image Processing Unit: the PS2's MPEG-2 macroblock decoder and
 * colour-space converter, used by the game's movie library (sceMpeg).
 *
 * The EE library parses pictures and does motion compensation itself; the
 * IPU provides: bitstream access (FDEC, VDEC for the macroblock-level VLCs),
 * macroblock decode (BDEC: VLC + inverse quantisation + IDCT), intra slice
 * decode (IDEC), quantiser/VQ tables (SETIQ/SETVQ) and CSC (YCbCr -> RGB).
 *
 * Flow control matters because the library derives its position in the
 * stream from the DMA and FIFO registers: input is pulled from DMA channel
 * 4 only as the decoder consumes it (8-quadword FIFO plus the 2-quadword
 * bit buffer), and a command that runs out of input suspends until more
 * arrives. The command processor is a coroutine so decoding code can simply
 * block in the bit reader. */
#define _GNU_SOURCE
#include "plat.h"
#include "gs.h"

int  dmac_ipu_in_pull(u8 *qw);        /* dmac.c: next input quadword, 0 if none */
void dmac_ipu_out_kick(void);         /* dmac.c: output became available */

/* ---- registers / state ---------------------------------------------------- */
static u32 ctrl_bits;                 /* IDP, AS, IVF, QST, MP1, PCT (bits 16..26) */
static u32 cbp, ecd, scd;
static u32 cmd_data; static int busy;
static u32 pending_cmd; static int have_cmd;
static u8 iq_intra[64], iq_non[64];
static u16 vqclut[16];
static u32 th0, th1;
static int dc_pred[3];

/* input: up to 10 quadwords (FIFO 8 + bit buffer 2), bit position in the first */
static u8 inbuf[16 * 10]; static u32 inq, bp;
/* output queue (bytes) */
static u8 *outbuf; static size_t out_r, out_w, out_cap;

static PlatCo *co_ctx, *caller_ctx; static int co_started, co_waiting_input;

static void yield_for_input(void) { co_waiting_input = 1; plat_co_switch(co_ctx, caller_ctx); co_waiting_input = 0; }

static void fill(void) { while (inq < 10 && dmac_ipu_in_pull(inbuf + inq * 16)) inq++; }
static void need(u32 bits) {
    for (;;) {
        if (inq * 128 >= bp + bits) return;
        fill();
        if (inq * 128 >= bp + bits) return;
        yield_for_input();
    }
}
static u32 peek(u32 bits) {            /* up to 32 bits, MSB first */
    need(bits);
    u32 byte = bp >> 3, off = bp & 7;
    u64 v = 0;
    for (int i = 0; i < 5; i++) v = (v << 8) | (byte + (u32)i < inq * 16 ? inbuf[byte + i] : 0);
    return (u32)((v >> (40 - off - bits)) & (bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1)));
}
static void skip(u32 bits) {
    need(bits);
    bp += bits;
    while (bp >= 128) { memmove(inbuf, inbuf + 16, (inq - 1) * 16); inq--; bp -= 128; }
}
static u32 get(u32 bits) { u32 v = peek(bits); skip(bits); return v; }

static void out_bytes(const void *p, size_t n) {
    if (out_w + n > out_cap) {
        if (out_r) { memmove(outbuf, outbuf + out_r, out_w - out_r); out_w -= out_r; out_r = 0; }
        if (out_w + n > out_cap) { out_cap = (out_w + n) * 2 + 4096; outbuf = realloc(outbuf, out_cap); }
    }
    memcpy(outbuf + out_w, p, n); out_w += n;
}
int ipu_out_pull(u8 *qw) {             /* used by dmac.c for channel 3 */
    if (out_w - out_r < 16) return 0;
    memcpy(qw, outbuf + out_r, 16); out_r += 16;
    return 1;
}

/* ---- MPEG-2 tables ---------------------------------------------------------- */
static const u8 zigzag[64] = { 0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,
    28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };
static const u8 altscan[64] = { 0,8,16,24,1,9,2,10,17,25,32,40,48,56,57,49,41,33,26,18,3,11,4,12,19,27,34,42,50,58,35,
    43,51,59,20,28,5,13,6,14,21,29,36,44,52,60,37,45,53,61,22,30,7,15,23,31,38,46,54,62,39,47,55,63 };
static const u8 nonlinear_q[32] = { 0,1,2,3,4,5,6,7,8,10,12,14,16,18,20,22,24,28,32,36,40,44,48,52,56,64,72,80,88,96,104,112 };

typedef struct { u32 code; u8 len; s16 a, b; } Vlc;
#define MAXVLC 128
static Vlc t_mbai[40], t_mbt_p[8], t_mbt_b[12], t_mc[20], t_cbp[70], t_dcl[12], t_dcc[12], t_b14[MAXVLC], t_b15[MAXVLC];
static int n_mbai, n_mbt_p, n_mbt_b, n_mc, n_cbp, n_dcl, n_dcc, n_b14, n_b15;

/* Table text: "bits:a[,b] bits:a[,b] ..." */
static int parse_vlc(Vlc *t, const char *s) {
    int n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        u32 code = 0; u8 len = 0;
        while (*s == '0' || *s == '1') { code = (code << 1) | (u32)(*s - '0'); len++; s++; }
        s++;                                    /* ':' */
        char *e; long a = strtol(s, &e, 10), b = 0;
        if (*e == ',') b = strtol(e + 1, &e, 10);
        t[n].code = code; t[n].len = len; t[n].a = (s16)a; t[n].b = (s16)b; n++;
        s = e;
    }
    return n;
}
static const Vlc *vlc_find(const Vlc *t, int n, u32 bits17) {
    for (int i = 0; i < n; i++) if ((bits17 >> (17 - t[i].len)) == t[i].code) return &t[i];
    return 0;
}
static const Vlc *vlc_get(const Vlc *t, int n) {
    const Vlc *v = vlc_find(t, n, peek(17));
    if (v) skip(v->len);
    return v;
}

static void init_tables(void) {
    n_mbai = parse_vlc(t_mbai,
        "1:1 011:2 010:3 0011:4 0010:5 00011:6 00010:7 0000111:8 0000110:9 00001011:10 00001010:11 00001001:12 "
        "00001000:13 00000111:14 00000110:15 0000010111:16 0000010110:17 0000010101:18 0000010100:19 0000010011:20 "
        "0000010010:21 00000100011:22 00000100010:23 00000100001:24 00000100000:25 00000011111:26 00000011110:27 "
        "00000011101:28 00000011100:29 00000011011:30 00000011010:31 00000011001:32 00000011000:33 "
        "00000001111:34 00000001000:35");                         /* 34 = stuffing, 35 = escape */
    /* macroblock type flags: 1 intra, 2 pattern, 4 backward, 8 forward, 16 quant */
    n_mbt_p = parse_vlc(t_mbt_p, "1:10 01:2 001:8 00011:1 00010:26 00001:18 000001:17");
    n_mbt_b = parse_vlc(t_mbt_b, "10:12 11:14 010:4 011:6 0010:8 0011:10 00011:1 00010:30 000011:26 000010:22 000001:17");
    n_mc = parse_vlc(t_mc, "1:0 01:1 001:2 0001:3 000011:4 0000101:5 0000100:6 0000011:7 000001011:8 000001010:9 "
        "000001001:10 0000010001:11 0000010000:12 0000001111:13 0000001110:14 0000001101:15 0000001100:16");
    n_cbp = parse_vlc(t_cbp,
        "111:60 1101:4 1100:8 1011:16 1010:32 10011:12 10010:48 10001:20 10000:40 01111:28 01110:44 01101:52 01100:56 "
        "01011:1 01010:61 01001:2 01000:62 001111:24 001110:36 001101:3 001100:63 0010111:5 0010110:9 0010101:17 "
        "0010100:33 0010011:6 0010010:10 0010001:18 0010000:34 00011111:7 00011110:11 00011101:19 00011100:35 "
        "00011011:13 00011010:49 00011001:21 00011000:41 00010111:14 00010110:50 00010101:22 00010100:42 00010011:15 "
        "00010010:51 00010001:23 00010000:43 00001111:25 00001110:37 00001101:26 00001100:38 00001011:29 00001010:45 "
        "00001001:53 00001000:57 00000111:30 00000110:46 00000101:54 00000100:58 000000111:31 000000110:47 "
        "000000101:55 000000100:59 000000011:27 000000010:39 000000001:0");
    n_dcl = parse_vlc(t_dcl, "100:0 00:1 01:2 101:3 110:4 1110:5 11110:6 111110:7 1111110:8 11111110:9 111111110:10 111111111:11");
    n_dcc = parse_vlc(t_dcc, "00:0 01:1 10:2 110:3 1110:4 11110:5 111110:6 1111110:7 11111110:8 111111110:9 1111111110:10 1111111111:11");
    /* DCT coefficients, "code:run,level" without the sign bit. The codes shared
     * by both tables (13 bits and longer, and the 12-bit group) come first. */
    static const char common[] =
        "000000011100:3,3 000000010010:4,3 000000011110:6,2 000000010101:7,2 000000010001:8,2 000000011111:17,1 "
        "000000011010:18,1 000000011001:19,1 000000010111:20,1 000000010110:21,1 "
        "0000000010110:1,6 0000000010101:1,7 0000000010100:2,5 0000000010011:3,4 0000000010010:5,3 0000000010001:9,2 "
        "0000000010000:10,2 0000000011111:22,1 0000000011110:23,1 0000000011101:24,1 0000000011100:25,1 0000000011011:26,1 "
        "00000000011111:0,16 00000000011110:0,17 00000000011101:0,18 00000000011100:0,19 00000000011011:0,20 "
        "00000000011010:0,21 00000000011001:0,22 00000000011000:0,23 00000000010111:0,24 00000000010110:0,25 "
        "00000000010101:0,26 00000000010100:0,27 00000000010011:0,28 00000000010010:0,29 00000000010001:0,30 "
        "00000000010000:0,31 000000000011000:0,32 000000000010111:0,33 000000000010110:0,34 000000000010101:0,35 "
        "000000000010100:0,36 000000000010011:0,37 000000000010010:0,38 000000000010001:0,39 000000000010000:0,40 "
        "000000000011111:1,8 000000000011110:1,9 000000000011101:1,10 000000000011100:1,11 000000000011011:1,12 "
        "000000000011010:1,13 000000000011001:1,14 0000000000010011:1,15 0000000000010010:1,16 0000000000010001:1,17 "
        "0000000000010000:1,18 0000000000010100:6,3 0000000000011010:11,2 0000000000011001:12,2 0000000000011000:13,2 "
        "0000000000010111:14,2 0000000000010110:15,2 0000000000010101:16,2 0000000000011111:27,1 0000000000011110:28,1 "
        "0000000000011101:29,1 0000000000011100:30,1 0000000000011011:31,1 ";
    static const char b14[] =
        "11:0,1 011:1,1 0100:0,2 0101:2,1 00101:0,3 00111:3,1 00110:4,1 000110:1,2 000111:5,1 000101:6,1 000100:7,1 "
        "0000110:0,4 0000100:2,2 0000111:8,1 0000101:9,1 00100110:0,5 00100001:0,6 00100101:1,3 00100100:3,2 "
        "00100111:10,1 00100011:11,1 00100010:12,1 00100000:13,1 0000001010:0,7 0000001100:1,4 0000001011:2,3 "
        "0000001111:4,2 0000001001:5,2 0000001110:14,1 0000001101:15,1 0000001000:16,1 000000011101:0,8 "
        "000000011000:0,9 000000010011:0,10 000000010000:0,11 000000011011:1,5 000000010100:2,4 "
        "0000000011010:0,12 0000000011001:0,13 0000000011000:0,14 0000000010111:0,15 ";
    static const char b15[] =
        "10:0,1 010:1,1 110:0,2 00101:2,1 0111:0,3 00111:3,1 000110:4,1 00110:1,2 000111:5,1 0000110:6,1 0000100:7,1 "
        "11100:0,4 0000111:2,2 0000101:8,1 1111000:9,1 11101:0,5 000101:0,6 1111001:1,3 00100110:3,2 1111010:10,1 "
        "00100001:11,1 00100101:12,1 00100100:13,1 000100:0,7 00100111:1,4 11111100:2,3 11111101:4,2 000000100:5,2 "
        "000000101:14,1 000000111:15,1 0000001101:16,1 1111011:0,8 1111100:0,9 00100011:0,10 00100010:0,11 "
        "00100000:1,5 0000001100:2,4 11111010:0,12 11111011:0,13 11111110:0,14 11111111:0,15 ";
    char buf[4096];
    snprintf(buf, sizeof buf, "%s%s", b14, common); n_b14 = parse_vlc(t_b14, buf);
    snprintf(buf, sizeof buf, "%s%s", b15, common); n_b15 = parse_vlc(t_b15, buf);
    if (n_b14 > MAXVLC || n_b15 > MAXVLC) rt_fatal(0, "IPU: VLC table overflow");
}

/* ---- block decode ------------------------------------------------------------ */
static float idct_cos[8][8];
static void idct(const int *in, int *out) {
    float tmp[64];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        float s = 0;
        for (int u = 0; u < 8; u++) s += idct_cos[x][u] * (float)in[y * 8 + u];
        tmp[y * 8 + x] = s;
    }
    for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
        float s = 0;
        for (int v = 0; v < 8; v++) s += idct_cos[y][v] * tmp[v * 8 + x];
        out[y * 8 + x] = (int)floorf(s + 0.5f);
    }
}

/* Decode one 8x8 block into `out` (pixels for intra, residual otherwise). */
static const char *fail_why = "";
static int decode_block(int comp, int intra, int qscale, int *out) {
    int coef[64] = { 0 }, i = 0, sum = 0;
    const u8 *scan = (ctrl_bits & (1u << 20)) ? altscan : zigzag;
    int idp = (ctrl_bits >> 16) & 3;
    if (intra) {
        const Vlc *v = comp ? vlc_get(t_dcc, n_dcc) : vlc_get(t_dcl, n_dcl);
        if (!v) { fail_why = "dc size"; return 0; }
        int size = v->a, diff = 0;
        if (size) { diff = (int)get((u32)size); if (diff < (1 << (size - 1))) diff -= (1 << size) - 1; }
        dc_pred[comp] += diff;
        coef[0] = dc_pred[comp] << (3 - idp);
        sum = coef[0];
        i = 1;
    }
    const Vlc *tab = (intra && (ctrl_bits & (1u << 21))) ? t_b15 : t_b14;
    int ntab = (intra && (ctrl_bits & (1u << 21))) ? n_b15 : n_b14;
    int eob_len = (tab == t_b15) ? 4 : 2; u32 eob_code = (tab == t_b15) ? 6 : 2;
    const u8 *W = intra ? iq_intra : iq_non;
    for (int first = !intra;; first = 0) {
        int run, level;
        u32 b = peek(17);
        if (first && (b >> 16)) { skip(1); run = 0; level = 1; if (get(1)) level = -1; }   /* "1s" */
        else if (!first && (b >> (17 - eob_len)) == eob_code) { skip((u32)eob_len); break; }
        else if ((b >> 11) == 1) {                                                       /* escape */
            skip(6); run = (int)get(6); level = (int)get(12);
            if (level & 0x800) level -= 4096;
            if (level == 0 || level == -2048) { fail_why = "escape level"; return 0; }
        } else {
            const Vlc *v = vlc_find(tab, ntab, b);
            if (!v) { fail_why = "coefficient code"; return 0; }
            skip(v->len); run = v->a; level = v->b;
            if (get(1)) level = -level;
        }
        i += run;
        if (i > 63) { fail_why = "run past 63"; return 0; }
        int pos = scan[i], val;
        if (intra) val = (level * qscale * W[pos]) / 16;
        else val = (((level < 0 ? -level : level) * 2 + 1) * qscale * W[pos]) / 32 * (level < 0 ? -1 : 1);
        if (val > 2047) val = 2047; else if (val < -2048) val = -2048;
        coef[pos] = val; sum += val;
        i++;
    }
    if (!(sum & 1)) coef[63] ^= 1;                    /* mismatch control */
    idct(coef, out);
    for (int k = 0; k < 64; k++) {
        if (intra) out[k] = out[k] < 0 ? 0 : out[k] > 255 ? 255 : out[k];
        else out[k] = out[k] < -256 ? -256 : out[k] > 255 ? 255 : out[k];
    }
    return 1;
}
static int qscale_of(u32 qsc) { return (ctrl_bits & (1u << 22)) ? nonlinear_q[qsc & 31] : (int)(qsc & 31) * 2; }
static void reset_dc(void) { int v = 1 << (((ctrl_bits >> 16) & 3) + 7); dc_pred[0] = dc_pred[1] = dc_pred[2] = v; }

/* Decode the 6 blocks of a macroblock into s16 planes (Y 16x16, Cb, Cr 8x8). */
static int decode_mb(int intra, u32 pattern, int qscale, int field_dct, s16 *mb) {
    memset(mb, 0, 384 * sizeof(s16));
    for (int b = 0; b < 6; b++) {
        if (!(pattern & (0x20u >> b))) continue;
        int blk[64];
        if (!decode_block(b < 4 ? 0 : b - 3, intra, qscale, blk)) return 0;
        if (b < 4) {
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
                int row = field_dct ? (b >> 1) + y * 2 : (b >> 1) * 8 + y;
                mb[row * 16 + (b & 1) * 8 + x] = (s16)blk[y * 8 + x];
            }
        } else for (int k = 0; k < 64; k++) mb[256 + (b - 4) * 64 + k] = (s16)blk[k];
    }
    return 1;
}

/* ---- colour conversion --------------------------------------------------------- */
static void csc_mb(const u8 *y, const u8 *cb, const u8 *cr, int ofm16, int dither) {
    (void)dither;
    u32 rgb[256];
    for (int j = 0; j < 16; j++) for (int i = 0; i < 16; i++) {
        int Y = (y[j * 16 + i] - 16) * 298, U = cb[(j >> 1) * 8 + (i >> 1)] - 128, V = cr[(j >> 1) * 8 + (i >> 1)] - 128;
        int r = (Y + 409 * V + 128) >> 8, g = (Y - 100 * U - 208 * V + 128) >> 8, b = (Y + 516 * U + 128) >> 8;
        r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
        u32 a = 0x80;
        if ((u32)r < th0 && (u32)g < th0 && (u32)b < th0) a = 0;
        else if ((u32)r < th1 && (u32)g < th1 && (u32)b < th1) a = 0x40;
        rgb[j * 16 + i] = (u32)r | ((u32)g << 8) | ((u32)b << 16) | (a << 24);
    }
    if (!ofm16) { out_bytes(rgb, sizeof rgb); return; }
    u16 p16[256];
    for (int k = 0; k < 256; k++) {
        u32 c = rgb[k];
        p16[k] = (u16)(((c >> 3) & 0x1F) | ((c >> 6) & 0x3E0) | ((c >> 9) & 0x7C00) | ((c >> 24) == 0x80 ? 0x8000 : 0));
    }
    out_bytes(p16, sizeof p16);
}

/* ---- commands ------------------------------------------------------------------- */
static void cmd_bdec(u32 c) {
    int intra = (c >> 27) & 1, field_dct = (c >> 25) & 1;
    skip(c & 0x3F);
    if ((c >> 26) & 1) reset_dc();
    u32 pattern = 0x3F;
    if (!intra) {
        const Vlc *v = vlc_get(t_cbp, n_cbp);
        if (!v) { ecd = 1; return; }
        pattern = (u32)v->a;
    }
    cbp = pattern;
    s16 mb[384];
    static u32 bdec_n; bdec_n++;
    if (!decode_mb(intra, pattern, qscale_of((c >> 16) & 31), field_dct, mb)) {
        ecd = 1;
        rt_log("[ipu] BDEC #%u: bitstream error (intra=%d cbp=%02x ctrl=%08x fail=%s next17=%05x)\n",
               bdec_n, intra, pattern, ctrl_bits, fail_why, peek(17));
    }
    out_bytes(mb, sizeof mb);
}
static void cmd_idec(u32 c) {
    int ofm = (c >> 27) & 1, dtd = (c >> 24) & 1;
    int qscale = qscale_of((c >> 16) & 31);
    skip(c & 0x3F);
    reset_dc();
    for (int first = 1;; first = 0) {
        if (!first) {                                    /* macroblock address increment (must be 1) */
            const Vlc *v = vlc_get(t_mbai, n_mbai);
            if (!v) { ecd = 1; break; }
        }
        u32 t = peek(2);
        int quant;
        if (t & 2) { skip(1); quant = 0; } else if (t == 1) { skip(2); quant = 1; } else { ecd = 1; break; }
        int field_dct = dtd ? (int)get(1) : 0;
        if (quant) qscale = qscale_of(get(5));
        s16 mb[384];
        if (!decode_mb(1, 0x3F, qscale, field_dct, mb)) { ecd = 1; rt_log("[ipu] IDEC: bitstream error\n"); break; }
        u8 px[384];
        for (int k = 0; k < 384; k++) px[k] = (u8)mb[k];
        csc_mb(px, px + 256, px + 320, ofm, (c >> 26) & 1);
        dmac_ipu_out_kick();
        if (peek(23) == 0) {                             /* next start code */
            while (bp & 7) skip(1);
            while (peek(24) != 1) skip(8);
            u32 code = peek(32) & 0xFF;
            if (code >= 1 && code <= 0xAF) {             /* another slice: header, then continue */
                skip(32);
                qscale = qscale_of(get(5));
                while (get(1)) skip(8);                  /* extra_bit_slice / extra_information */
                reset_dc();
                first = 1;
                { int f = 1; (void)f; }
                /* the first macroblock of a slice still carries an address increment */
                const Vlc *v = vlc_get(t_mbai, n_mbai);
                if (!v) { ecd = 1; break; }
                continue;
            }
            scd = 1;
            break;
        }
    }
}
static void cmd_vdec(u32 c) {
    skip(c & 0x3F);
    const Vlc *v = 0; int val = 0; u32 len = 0;
    switch ((c >> 26) & 3) {
    case 0: v = vlc_get(t_mbai, n_mbai); if (v) { val = v->a; len = v->len; } break;
    case 1: {
        int pct = (ctrl_bits >> 24) & 7;
        if (pct == 1) {                                   /* I picture */
            u32 t = peek(2);
            if (t & 2) { skip(1); val = 1; len = 1; } else if (t == 1) { skip(2); val = 17; len = 2; }
        } else {
            v = (pct == 3) ? vlc_get(t_mbt_b, n_mbt_b) : vlc_get(t_mbt_p, n_mbt_p);
            if (v) { val = v->a; len = v->len; }
        }
        break;
    }
    case 2:
        v = vlc_get(t_mc, n_mc);
        if (v) { val = v->a; len = v->len; if (val) { if (get(1)) val = -val; len++; } }
        break;
    default: {
        u32 t = peek(2);
        if (!(t & 2)) { skip(1); val = 0; len = 1; } else { skip(2); val = (t & 1) ? -1 : 1; len = 2; }
        break;
    }
    }
    if (!len) ecd = 1;
    cmd_data = (len << 16) | ((u32)val & 0xFFFF);
}
static void cmd_csc(u32 c) {
    u32 count = c & 0x7FF; int ofm = (c >> 27) & 1;
    for (u32 n = 0; n < count; n++) {
        u8 raw[384];
        for (int k = 0; k < 384; k++) raw[k] = (u8)get(8);
        csc_mb(raw, raw + 256, raw + 320, ofm, (c >> 26) & 1);
        dmac_ipu_out_kick();
    }
}

static void run_command(u32 c) {
    ecd = 0; scd = 0;
    switch (c >> 28) {
    case 0: inq = 0; bp = c & 0x7F; break;                                  /* BCLR */
    case 1: cmd_idec(c); break;
    case 2: cmd_bdec(c); break;
    case 3: cmd_vdec(c); break;
    case 4: skip(c & 0x3F); cmd_data = peek(32); break;                     /* FDEC */
    case 5: {                                                                /* SETIQ */
        u8 *m = ((c >> 27) & 1) ? iq_non : iq_intra;
        skip(c & 0x3F);
        for (int i = 0; i < 64; i++) m[zigzag[i]] = (u8)get(8);
        break;
    }
    case 6: for (int i = 0; i < 16; i++) { u32 lo = get(8), hi = get(8); vqclut[i] = (u16)(lo | (hi << 8)); } break;   /* SETVQ */
    case 7: cmd_csc(c); break;
    case 9: th0 = c & 0x1FF; th1 = (c >> 16) & 0x1FF; break;                 /* SETTH */
    default: rt_fatal(0, "IPU: command %u (0x%08x) not implemented", c >> 28, c);
    }
}

static void co_main(void) {
    for (;;) {
        while (!have_cmd) plat_co_switch(co_ctx, caller_ctx);
        have_cmd = 0;
        run_command(pending_cmd);
        busy = 0;
        dmac_ipu_out_kick();
        if ((pending_cmd >> 28) != 0) intc_raise(INTC_IPU);
    }
}
static void resume(void) {
    if (!co_started) {
        if (!caller_ctx) caller_ctx = plat_co_new(0);
        co_ctx = plat_co_new(1 << 20);
        plat_co_start(co_ctx, co_main);
        co_started = 1;
    }
    plat_co_switch(caller_ctx, co_ctx);
}
/* More input may be available (DMA channel 4 started, or FIFO written). */
void ipu_input_ready(void) { if (co_waiting_input) resume(); }

/* ---- register interface ------------------------------------------------------------ */
u64 ipu_read(u32 a) {
    u32 fp = inq < 2 ? inq : 2, ifc = inq - fp;
    switch (a) {
    case 0x10002000: return cmd_data | (busy ? 1ull << 63 : 0);
    case 0x10002010: {
        size_t o = (out_w - out_r) / 16;
        return ifc | ((u32)(o > 8 ? 8 : o) << 4) | (cbp << 8) | (ecd << 14) | (scd << 15) | ctrl_bits | ((u32)busy << 31);
    }
    case 0x10002020: return bp | (ifc << 8) | (fp << 16);
    case 0x10002030:
        if (busy) return 0x80000000u;
        fill();
        if (inq * 128 < bp + 32) return 0x80000000u;
        { u32 v = peek(32); return v; }
    }
    return 0;
}
void ipu_write(u32 a, u64 v) {
    switch (a) {
    case 0x10002000:
        { static u32 hist[16], hn; hist[hn++ & 15] = (u32)v; if (g_verbose > 2) rt_log("[ipu] cmd %u opt=%07x (inq=%u bp=%u)\n", (u32)v >> 28, (u32)v & 0x0FFFFFFF, inq, bp); }
        pending_cmd = (u32)v; have_cmd = 1; busy = 1;
        resume();
        break;
    case 0x10002010:
        if (v & 0x40000000u) {                       /* reset */
            inq = 0; bp = 0; out_r = out_w = 0; busy = 0; have_cmd = 0; ecd = scd = cbp = 0; ctrl_bits = 0;
            if (co_started) { plat_co_free(co_ctx); co_ctx = 0; co_started = 0; co_waiting_input = 0; }
        }
        ctrl_bits = (u32)v & 0x07F30000u;
        break;
    }
}
void ipu_fifo_write(const u8 *qw) {
    if (inq < 10) { memcpy(inbuf + inq * 16, qw, 16); inq++; }
    ipu_input_ready();
}
void ipu_init(void) {
    init_tables();
    for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++)
        idct_cos[x][u] = (u == 0 ? 0.35355339f : 0.5f) * cosf((2 * x + 1) * u * 3.14159265358979f / 16.0f);
    static const u8 def[64] = { 8,16,19,22,26,27,29,34, 16,16,22,24,27,29,34,37, 19,22,26,27,29,34,34,38, 22,22,26,27,29,34,37,40,
        22,26,27,29,32,35,40,48, 26,27,29,32,35,40,48,58, 26,27,29,34,38,46,56,69, 27,29,35,38,46,56,69,83 };
    memcpy(iq_intra, def, 64); memset(iq_non, 16, 64);
}

void ipu_dump(void) {
    fprintf(stderr, "  ipu: busy=%d cmd=%08x waiting_input=%d inq=%u bp=%u out=%zu ecd=%u scd=%u ctrl=%08x\n",
            busy, pending_cmd, co_waiting_input, inq, bp, out_w - out_r, ecd, scd, ctrl_bits);
}
