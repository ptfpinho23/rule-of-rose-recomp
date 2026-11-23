/* vu_ops.h — vector-unit operations, used by COP2 macro mode (VU0) and by
 * the VU micro-program interpreter. Included from cpu.h. */
#ifndef ROR_VU_OPS_H
#define ROR_VU_OPS_H

enum { VUOP_ADD, VUOP_SUB, VUOP_MUL, VUOP_MADD, VUOP_MSUB, VUOP_MAX, VUOP_MINI };

/* Result fixup + MAC flag bits for component i (0 = x .. 3 = w). */
static inline float vu_res(VU *v, int i, float x) {
    fpr_t r; r.f = x;
    int b = 3 - i;
    if ((r.u & 0x7F800000u) == 0x7F800000u) {
        r.u = (r.u & 0x80000000u) | 0x7F7FFFFFu;
        v->mac |= 0x1000u << b;
    } else if ((r.u & 0x7F800000u) == 0) {
        if (r.u & 0x007FFFFFu) v->mac |= 0x0100u << b;     /* underflow */
        r.u &= 0x80000000u;
        v->mac |= 0x0001u << b;
    }
    if (r.u & 0x80000000u) v->mac |= 0x0010u << b;
    return r.f;
}
static inline void vu_flags_end(VU *v) {
    u32 f = 0;
    if (v->mac & 0x000F) f |= 1;
    if (v->mac & 0x00F0) f |= 2;
    if (v->mac & 0x0F00) f |= 4;
    if (v->mac & 0xF000) f |= 8;
    v->status = (v->status & 0xFF0) | f | (f << 6);     /* I/D (bits 4,5) belong to the divider */
}
/* Destination pointer: writes to vf[0] are discarded. */
static inline r128 *vu_dst(VU *v, r128 *scratch, int fd) {
    return fd ? &v->vf[fd] : scratch;
}

static inline __attribute__((always_inline))
void vu_arith(VU *v, int op, r128 *dst, u32 dest, const r128 *pa, const r128 *pb) {
    r128 a = *pa, b = *pb, acc = v->acc, o = *dst;
    if (op < VUOP_MAX) v->mac = 0;                 /* MAX/MINI leave the flags alone */
    for (int i = 0; i < 4; i++) {
        if (!(dest & (8u >> i))) continue;
        float x = fclamp(a.f[i]), y = fclamp(b.f[i]), r;
        switch (op) {
        case VUOP_ADD:  r = x + y; break;
        case VUOP_SUB:  r = x - y; break;
        case VUOP_MUL:  r = x * y; break;
        case VUOP_MADD: r = fclamp(acc.f[i]) + fclamp(x * y); break;
        case VUOP_MSUB: r = fclamp(acc.f[i]) - fclamp(x * y); break;
        case VUOP_MAX:  o.f[i] = x > y ? x : y; continue;
        default:        o.f[i] = x < y ? x : y; continue;
        }
        o.f[i] = vu_res(v, i, r);
    }
    *dst = o;
    if (op < VUOP_MAX) vu_flags_end(v);
}
static inline r128 vu_splat(float s) { r128 t; t.f[0] = t.f[1] = t.f[2] = t.f[3] = s; return t; }

/* Shorthand used by generated code (c = Ctx*). */
#define V0 (&g_vu0)
#define VU_A3(op, dreg, dest, fs, ft) do { r128 s__; \
    vu_arith(V0, op, dreg, dest, &V0->vf[fs], &V0->vf[ft]); (void)s__; } while (0)
#define VU_ARITH(op, fd, dest, fs, ft) do { r128 s__; \
    vu_arith(V0, op, vu_dst(V0, &s__, fd), dest, &V0->vf[fs], &V0->vf[ft]); } while (0)
#define VU_ARITH_S(op, fd, dest, fs, scalar) do { r128 s__, b__ = vu_splat(scalar); \
    vu_arith(V0, op, vu_dst(V0, &s__, fd), dest, &V0->vf[fs], &b__); } while (0)
#define VU_ARITHA(op, dest, fs, ft) \
    vu_arith(V0, op, &V0->acc, dest, &V0->vf[fs], &V0->vf[ft])
#define VU_ARITHA_S(op, dest, fs, scalar) do { r128 b__ = vu_splat(scalar); \
    vu_arith(V0, op, &V0->acc, dest, &V0->vf[fs], &b__); } while (0)

static inline void vu_opmula(VU *v, int fs, int ft) {
    r128 a = v->vf[fs], b = v->vf[ft];
    v->mac = 0;
    v->acc.f[0] = vu_res(v, 0, fclamp(a.f[1]) * fclamp(b.f[2]));
    v->acc.f[1] = vu_res(v, 1, fclamp(a.f[2]) * fclamp(b.f[0]));
    v->acc.f[2] = vu_res(v, 2, fclamp(a.f[0]) * fclamp(b.f[1]));
    vu_flags_end(v);
}
static inline void vu_opmsub(VU *v, int fd, int fs, int ft) {
    r128 a = v->vf[fs], b = v->vf[ft], o;
    v->mac = 0;
    o.f[0] = vu_res(v, 0, fclamp(v->acc.f[0]) - fclamp(fclamp(a.f[1]) * fclamp(b.f[2])));
    o.f[1] = vu_res(v, 1, fclamp(v->acc.f[1]) - fclamp(fclamp(a.f[2]) * fclamp(b.f[0])));
    o.f[2] = vu_res(v, 2, fclamp(v->acc.f[2]) - fclamp(fclamp(a.f[0]) * fclamp(b.f[1])));
    if (fd) { v->vf[fd].f[0] = o.f[0]; v->vf[fd].f[1] = o.f[1]; v->vf[fd].f[2] = o.f[2]; }
    vu_flags_end(v);
}
static inline void vu_move(VU *v, u32 dest, int ft, int fs) {
    if (!ft) return;
    r128 a = v->vf[fs];
    for (int i = 0; i < 4; i++) if (dest & (8u >> i)) v->vf[ft].u32[i] = a.u32[i];
}
static inline void vu_mr32(VU *v, u32 dest, int ft, int fs) {
    if (!ft) return;
    r128 a = v->vf[fs];
    for (int i = 0; i < 4; i++) if (dest & (8u >> i)) v->vf[ft].u32[i] = a.u32[(i + 1) & 3];
}
static inline void vu_abs(VU *v, u32 dest, int ft, int fs) {
    if (!ft) return;
    r128 a = v->vf[fs];
    for (int i = 0; i < 4; i++) if (dest & (8u >> i)) v->vf[ft].u32[i] = a.u32[i] & 0x7FFFFFFFu;
}
static inline s32 vu_ftoi(float x, float scale) {
    fpr_t a; a.f = x * scale;
    if ((a.u & 0x7F800000u) <= 0x4E800000u) return (s32)a.f;
    return (a.u & 0x80000000u) ? INT32_MIN : INT32_MAX;
}
static inline void vu_ftoin(VU *v, u32 dest, int ft, int fs, float scale) {
    if (!ft) return;
    r128 a = v->vf[fs];
    for (int i = 0; i < 4; i++) if (dest & (8u >> i))
        v->vf[ft].s32[i] = vu_ftoi(fclamp(a.f[i]), scale);
}
static inline void vu_itofn(VU *v, u32 dest, int ft, int fs, float scale) {
    if (!ft) return;
    r128 a = v->vf[fs];
    for (int i = 0; i < 4; i++) if (dest & (8u >> i))
        v->vf[ft].f[i] = (float)a.s32[i] * scale;
}
static inline void vu_clip(VU *v, int fs, int ft) {
    float w = fabsf(fclamp(v->vf[ft].f[3]));
    u32 f = 0;
    for (int i = 0; i < 3; i++) {
        float x = fclamp(v->vf[fs].f[i]);
        if (x > w)  f |= 1u << (i * 2);
        if (x < -w) f |= 2u << (i * 2);
    }
    v->clip = ((v->clip << 6) | f) & 0xFFFFFFu;
}
static inline void vu_div(VU *v, int fs, int fsf, int ft, int ftf) {
    fpr_t a, b; a.u = v->vf[fs].u32[fsf]; b.u = v->vf[ft].u32[ftf];
    v->status &= ~0x30u;
    if ((b.u & 0x7F800000u) == 0) {
        if ((a.u & 0x7F800000u) == 0) v->status |= 0x10 | 0x400;
        else v->status |= 0x20 | 0x800;
        v->q.u = ((a.u ^ b.u) & 0x80000000u) | 0x7F7FFFFFu;
    } else v->q.f = fclamp(fclamp(a.f) / fclamp(b.f));
}
static inline void vu_sqrt(VU *v, int ft, int ftf) {
    fpr_t b; b.u = v->vf[ft].u32[ftf];
    v->status &= ~0x30u;
    if (b.u & 0x80000000u) v->status |= 0x10 | 0x400;
    v->q.f = sqrtf(fabsf(fclamp(b.f)));
}
static inline void vu_rsqrt(VU *v, int fs, int fsf, int ft, int ftf) {
    fpr_t a, b; a.u = v->vf[fs].u32[fsf]; b.u = v->vf[ft].u32[ftf];
    v->status &= ~0x30u;
    if ((b.u & 0x7F800000u) == 0) {
        if ((a.u & 0x7F800000u) == 0) v->status |= 0x10 | 0x400;
        else v->status |= 0x20 | 0x800;
        v->q.u = ((a.u ^ b.u) & 0x80000000u) | 0x7F7FFFFFu;
        return;
    }
    if (b.u & 0x80000000u) v->status |= 0x10 | 0x400;
    v->q.f = fclamp(fclamp(a.f) / sqrtf(fabsf(fclamp(b.f))));
}
/* VU data memory access (addr in 16-byte units). */
static inline void vu_lqi(VU *v, u32 dest, int ft, int is) {
    u32 a = (v->vi[is] * 16) & v->mem_mask;
    if (ft) { r128 m; memcpy(&m, v->mem + a, 16);
        for (int i = 0; i < 4; i++) if (dest & (8u >> i)) v->vf[ft].u32[i] = m.u32[i]; }
    if (is) v->vi[is] = (v->vi[is] + 1) & 0xFFFF;
}
static inline void vu_sqi(VU *v, u32 dest, int fs, int it) {
    u32 a = (v->vi[it] * 16) & v->mem_mask;
    for (int i = 0; i < 4; i++) if (dest & (8u >> i)) memcpy(v->mem + a + i * 4, &v->vf[fs].u32[i], 4);
    if (it) v->vi[it] = (v->vi[it] + 1) & 0xFFFF;
}
static inline void vu_lqd(VU *v, u32 dest, int ft, int is) {
    if (is) v->vi[is] = (v->vi[is] - 1) & 0xFFFF;
    u32 a = (v->vi[is] * 16) & v->mem_mask;
    if (ft) { r128 m; memcpy(&m, v->mem + a, 16);
        for (int i = 0; i < 4; i++) if (dest & (8u >> i)) v->vf[ft].u32[i] = m.u32[i]; }
}
static inline void vu_sqd(VU *v, u32 dest, int fs, int it) {
    if (it) v->vi[it] = (v->vi[it] - 1) & 0xFFFF;
    u32 a = (v->vi[it] * 16) & v->mem_mask;
    for (int i = 0; i < 4; i++) if (dest & (8u >> i)) memcpy(v->mem + a + i * 4, &v->vf[fs].u32[i], 4);
}
#define VI_SET(v, n, val) do { if (n) (v)->vi[n] = (u32)(val) & 0xFFFF; } while (0)

/* COP2 control registers (cfc2 / ctc2). */
static inline u32 vu0_cfc2(VU *v, int reg) {
    if (reg < 16) return v->vi[reg];
    switch (reg) {
    case 16: return v->status; case 17: return v->mac; case 18: return v->clip;
    case 20: return v->r & 0x7FFFFF; case 21: return v->i.u; case 22: return v->q.u;
    case 26: return v->tpc; case 27: return v->cmsar0; case 28: return v->fbrst;
    case 29: return v->vpu_stat; case 31: return v->cmsar1;
    }
    return 0;
}
static inline void vu0_ctc2(VU *v, int reg, u32 val) {
    if (reg < 16) { VI_SET(v, reg, val); return; }
    switch (reg) {
    case 16: v->status = (v->status & 0x3F) | (val & 0xFC0); break;
    case 18: v->clip = val & 0xFFFFFF; break;
    case 20: v->r = val & 0x7FFFFF; break;
    case 21: v->i.u = val; break;
    case 22: v->q.u = val; break;
    case 27: v->cmsar0 = val & 0xFFFF; break;
    case 28: v->fbrst = val & 0x0C0C; break;   /* reset bits handled by runtime */
    case 31: v->cmsar1 = val & 0xFFFF; break;
    }
}
#endif
