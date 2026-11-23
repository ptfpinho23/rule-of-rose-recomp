/* vu.h — vector units (micro mode) and VIF. */
#ifndef ROR_VU_H
#define ROR_VU_H
#include "rt.h"

extern VU g_vu1;
void vu_init(void);
void vu_exec(int n, u32 addr);        /* run micro program; addr 0xFFFFFFFF = continue */
void vu_micro_changed(int n);

void vif_reset(int n);
void vif_write(int n, const u8 *data, u32 bytes);
u32  vif_read_reg(int n, u32 off);
void vif_write_reg(int n, u32 off, u32 val);
#endif
