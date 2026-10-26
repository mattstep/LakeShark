/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original. */
#pragma once

/* USB CDC-NCM 1.0 transfer blocks, 16-bit flavour (NTB16). Pure code, no
   IDF: the bench builds it on the host.

   An NTB is one USB bulk transfer: an NTH16 header, one or more NDP16
   datagram pointer tables chained from it, and the Ethernet frames the
   tables point at, all at offsets inside the block. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_NCM_NTH16_SIG     0x484d434eu /* "NCMH" */
#define LS_NCM_NDP16_SIG     0x304d434eu /* "NCM0" */
#define LS_NCM_NDP16_CRC_SIG 0x314d434eu /* "NCM1": CRC-32 after each frame */
#define LS_NCM_NTH16_LEN     12
#define LS_NCM_NDP16_MIN     16 /* header and one entry plus the terminator */
#define LS_NCM_PARAMS_LEN    28
#define LS_NCM_MIN_DATAGRAM  14 /* an Ethernet header */

/* GET_NTB_PARAMETERS, decoded. */
typedef struct {
    uint16_t formats;          /* bit 0 NTB16, bit 1 NTB32 */
    uint32_t in_max;           /* dwNtbInMaxSize */
    uint16_t in_divisor, in_remainder, in_align;
    uint32_t out_max;          /* dwNtbOutMaxSize */
    uint16_t out_divisor, out_remainder, out_align;
    uint16_t out_max_datagrams; /* 0: no limit */
} ls_ncm_params_t;

bool ls_ncm_parse_params(const uint8_t *b, size_t n, ls_ncm_params_t *p);

/* One frame in one NTB: NTH16, then the NDP16 (at wNdpOutAlignment), then
   the frame at the first offset that satisfies wNdpOutDivisor and
   wNdpOutPayloadRemainder. When the block would be a whole number of
   mps-sized packets one zero byte is added (and counted in wBlockLength),
   so the transfer ends in a short packet and needs no zero-length one.
   Returns the transfer length, 0 if it does not fit cap or out_max. */
size_t ls_ncm_build(uint8_t *out, size_t cap, const ls_ncm_params_t *p, uint16_t mps,
                    uint16_t seq, const uint8_t *frame, size_t len);

typedef enum {
    LS_NCM_OK = 0,
    LS_NCM_E_SHORT,      /* shorter than an NTH16 */
    LS_NCM_E_SIG,        /* not "NCMH" */
    LS_NCM_E_HDRLEN,     /* wHeaderLength is not 12 */
    LS_NCM_E_BLOCKLEN,   /* wBlockLength beyond what arrived */
    LS_NCM_E_NDP_INDEX,  /* NDP outside the block, or misaligned */
    LS_NCM_E_NDP_SIG,    /* not "NCM0" (or "NCM1") */
    LS_NCM_E_NDP_LEN,    /* wLength too small, odd, or past the block */
    LS_NCM_E_ENTRY,      /* a datagram outside the block or too short */
    LS_NCM_E_CHAIN,      /* more chained NDPs than any sane device sends */
} ls_ncm_err_t;

const char *ls_ncm_err_name(ls_ncm_err_t e);

typedef void (*ls_ncm_datagram_fn)(void *ctx, const uint8_t *d, size_t len);

/* Walks every NDP16 of one received NTB and hands each valid datagram to
   fn. A bad entry is skipped and the rest still delivered; a bad header or
   table stops there. Returns the number delivered; *err is the first
   problem met, LS_NCM_OK when none. */
int ls_ncm_parse(const uint8_t *ntb, size_t len, ls_ncm_datagram_fn fn, void *ctx,
                 ls_ncm_err_t *err);

#ifdef __cplusplus
}
#endif
