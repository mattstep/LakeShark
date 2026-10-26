/* SPDX-License-Identifier: GPL-3.0-or-later
   LakeShark original, written from the USB CDC-NCM 1.0 specification. */

#include "ls_ncm.h"

#include <string.h>

#define MAX_NDPS 8

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t rd32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static void wr16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}

bool ls_ncm_parse_params(const uint8_t *b, size_t n, ls_ncm_params_t *p)
{
    if (n < LS_NCM_PARAMS_LEN || rd16(b) < LS_NCM_PARAMS_LEN) return false;
    p->formats = rd16(b + 2);
    p->in_max = rd32(b + 4);
    p->in_divisor = rd16(b + 8);
    p->in_remainder = rd16(b + 10);
    p->in_align = rd16(b + 12);
    p->out_max = rd32(b + 16);
    p->out_divisor = rd16(b + 20);
    p->out_remainder = rd16(b + 22);
    p->out_align = rd16(b + 24);
    p->out_max_datagrams = rd16(b + 26);
    return true;
}

static size_t align_up(size_t v, size_t a)
{
    return a > 1 ? (v + a - 1) / a * a : v;
}

size_t ls_ncm_build(uint8_t *out, size_t cap, const ls_ncm_params_t *p, uint16_t mps,
                    uint16_t seq, const uint8_t *frame, size_t len)
{
    /* The spec makes both powers of two of at least 4; a device that
       reports less gets 4, which every NCM host uses anyway. */
    const size_t ndp_align = p->out_align >= 4 ? p->out_align : 4;
    const size_t ndp = align_up(LS_NCM_NTH16_LEN, ndp_align);
    size_t dg = ndp + LS_NCM_NDP16_MIN;
    if (p->out_divisor > 1) {
        const size_t d = p->out_divisor, r = p->out_remainder % d;
        dg += (r + d - dg % d) % d;
    }
    size_t total = dg + len;
    const size_t limit = p->out_max && p->out_max < cap ? p->out_max : cap;
    if (len < LS_NCM_MIN_DATAGRAM || total > limit || total > 0xffff) return 0;
    if (mps && total % mps == 0 && total + 1 <= limit) {
        out[total] = 0;
        total++;
    }
    memset(out, 0, dg);
    wr32(out, LS_NCM_NTH16_SIG);
    wr16(out + 4, LS_NCM_NTH16_LEN);
    wr16(out + 6, seq);
    wr16(out + 8, (uint16_t)total);
    wr16(out + 10, (uint16_t)ndp);
    wr32(out + ndp, LS_NCM_NDP16_SIG);
    wr16(out + ndp + 4, LS_NCM_NDP16_MIN);
    wr16(out + ndp + 6, 0);
    wr16(out + ndp + 8, (uint16_t)dg);
    wr16(out + ndp + 10, (uint16_t)len);
    /* + 12: the zero terminator entry, already zero. */
    memcpy(out + dg, frame, len);
    return total;
}

const char *ls_ncm_err_name(ls_ncm_err_t e)
{
    switch (e) {
    case LS_NCM_OK:          return "ok";
    case LS_NCM_E_SHORT:     return "short";
    case LS_NCM_E_SIG:       return "bad NTH signature";
    case LS_NCM_E_HDRLEN:    return "bad header length";
    case LS_NCM_E_BLOCKLEN:  return "block length past the transfer";
    case LS_NCM_E_NDP_INDEX: return "NDP index out of range";
    case LS_NCM_E_NDP_SIG:   return "bad NDP signature";
    case LS_NCM_E_NDP_LEN:   return "bad NDP length";
    case LS_NCM_E_ENTRY:     return "datagram entry out of range";
    case LS_NCM_E_CHAIN:     return "NDP chain too long";
    }
    return "?";
}

int ls_ncm_parse(const uint8_t *ntb, size_t len, ls_ncm_datagram_fn fn, void *ctx,
                 ls_ncm_err_t *err)
{
    ls_ncm_err_t first = LS_NCM_OK;
#define FAIL(e) do { if (first == LS_NCM_OK) first = (e); } while (0)
    int delivered = 0;
    if (len < LS_NCM_NTH16_LEN) { FAIL(LS_NCM_E_SHORT); goto out; }
    if (rd32(ntb) != LS_NCM_NTH16_SIG) { FAIL(LS_NCM_E_SIG); goto out; }
    if (rd16(ntb + 4) != LS_NCM_NTH16_LEN) { FAIL(LS_NCM_E_HDRLEN); goto out; }
    size_t block = rd16(ntb + 8);
    /* 0: the block runs to the end of the transfer (a short packet ends it). */
    if (block == 0) block = len;
    if (block > len || block < LS_NCM_NTH16_LEN) { FAIL(LS_NCM_E_BLOCKLEN); goto out; }

    size_t ndp = rd16(ntb + 10);
    for (int hops = 0; ndp; hops++) {
        if (hops == MAX_NDPS) { FAIL(LS_NCM_E_CHAIN); break; }
        if (ndp < LS_NCM_NTH16_LEN || ndp % 4 || ndp + 8 > block) {
            FAIL(LS_NCM_E_NDP_INDEX);
            break;
        }
        const uint32_t sig = rd32(ntb + ndp);
        if (sig != LS_NCM_NDP16_SIG && sig != LS_NCM_NDP16_CRC_SIG) {
            FAIL(LS_NCM_E_NDP_SIG);
            break;
        }
        const size_t ndp_len = rd16(ntb + ndp + 4);
        if (ndp_len < LS_NCM_NDP16_MIN || ndp_len % 4 || ndp + ndp_len > block) {
            FAIL(LS_NCM_E_NDP_LEN);
            break;
        }
        /* With CRC the frame carries 4 trailing bytes that are not Ethernet. */
        const size_t crc = sig == LS_NCM_NDP16_CRC_SIG ? 4 : 0;
        for (size_t e = ndp + 8; e + 4 <= ndp + ndp_len; e += 4) {
            const size_t idx = rd16(ntb + e), dlen = rd16(ntb + e + 2);
            if (idx == 0 || dlen == 0) break; /* the terminator */
            if (idx < LS_NCM_NTH16_LEN || idx + dlen > block ||
                dlen < LS_NCM_MIN_DATAGRAM + crc) {
                FAIL(LS_NCM_E_ENTRY);
                continue;
            }
            if (fn) fn(ctx, ntb + idx, dlen - crc);
            delivered++;
        }
        ndp = rd16(ntb + ndp + 6);
    }
out:
    if (err) *err = first;
    return delivered;
#undef FAIL
}
