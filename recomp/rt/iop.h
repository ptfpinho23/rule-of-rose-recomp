/* iop.h — native I/O-processor services reached over SIF RPC. */
#ifndef ROR_IOP_H
#define ROR_IOP_H
#include "rt.h"

#define IOP_RAM_SIZE 0x00200000u
extern u8 g_iop_ram[IOP_RAM_SIZE];

struct IopServer;
typedef struct RpcCall {
    u32 fno;                 /* function number */
    u8 *send; u32 send_size; /* arguments (in the server's IOP buffer) */
    u8 *recv; u32 recv_size; /* result, copied to the EE on completion */
    u32 ee_recv;
    u64 delay;               /* extra ticks before the call completes */
    int no_reply;            /* the call never completes (waits for an event that does not occur) */
    Ctx *ctx;
    struct IopServer *server;
} RpcCall;

typedef struct IopServer {
    u32 sid;
    const char *name;
    void (*call)(RpcCall *rc);
    u32 buf_size;
    u32 buf, handle;         /* assigned at first bind */
} IopServer;

IopServer *iop_find_server(u32 sid);
IopServer *iop_server_by_handle(u32 handle);
int  iop_custom_cmd(Ctx *c, u32 cid, const u8 *pkt, u32 size);
void iop_reset(void);
void iop_init(const char *iso_path);
u32  iop_alloc(u32 size);

/* service modules */
void rxsnd_call(RpcCall *rc);
void dbc_call(RpcCall *rc);
void mc_call(RpcCall *rc);

/* iso.c */
int iso_open(const char *path);
int iso_read(u32 lsn, u32 count, void *dst);
int iso_find(const char *path, u32 *lsn, u32 *size);

/* sif.c */
void sif_send_cmd(u32 cid, u32 opt, const void *payload, u32 payload_size,
                  const void *data, u32 dsize, u32 ddest, u64 delay);
u32  sif_set_dma(Ctx *c, u32 sdd, u32 count);
u32  sif_get_reg(u32 r);
void sif_set_reg(u32 r, u32 v);

static inline u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline void wr32(u8 *p, u32 v) { memcpy(p, &v, 4); }
#endif
