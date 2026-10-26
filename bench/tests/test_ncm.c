/* LS_TEST_SOURCES: ${FW}/components/lslte/ls_ncm.c */
#include "ls_test.h"
#include "ls_ncm.h"

#include <stdint.h>
#include <string.h>

/* NTB16 framing for the Huawei NCM stick: what we build must parse back,
   and what a device might send wrong must be counted, not delivered. */

static const ls_ncm_params_t P = {
    .formats = 1, .in_max = 16384, .in_divisor = 4, .in_remainder = 0, .in_align = 4,
    .out_max = 16384, .out_divisor = 4, .out_remainder = 0, .out_align = 4,
};

typedef struct { int n; size_t len[8]; const uint8_t *at[8]; } got_t;

static void take(void *ctx, const uint8_t *d, size_t len)
{
    got_t *g = ctx;
    if (g->n < 8) { g->len[g->n] = len; g->at[g->n] = d; }
    g->n++;
}

static void frame(uint8_t *f, size_t n, uint8_t seed)
{
    for (size_t i = 0; i < n; i++) f[i] = (uint8_t)(seed + i * 7);
}

static void wr16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }

LS_CASE(params_decode)
{
    /* As GET_NTB_PARAMETERS returns them, little-endian. */
    const uint8_t b[28] = { 28, 0, 1, 0, 0x00, 0x40, 0, 0, 4, 0, 0, 0, 4, 0, 0, 0,
                            0x00, 0x08, 0, 0, 4, 0, 0, 0, 4, 0, 1, 0 };
    ls_ncm_params_t p;
    LS_CHECK(ls_ncm_parse_params(b, sizeof(b), &p));
    LS_EQ_UINT(p.formats, 1);
    LS_EQ_UINT(p.in_max, 16384);
    LS_EQ_UINT(p.in_divisor, 4);
    LS_EQ_UINT(p.in_align, 4);
    LS_EQ_UINT(p.out_max, 2048);
    LS_EQ_UINT(p.out_divisor, 4);
    LS_EQ_UINT(p.out_align, 4);
    LS_EQ_UINT(p.out_max_datagrams, 1);
    LS_CHECK(!ls_ncm_parse_params(b, 27, &p));
}

LS_CASE(build_then_parse_round_trip)
{
    uint8_t f[1514], ntb[2048];
    frame(f, sizeof(f), 3);
    const size_t n = ls_ncm_build(ntb, sizeof(ntb), &P, 512, 7, f, sizeof(f));
    LS_CHECK(n > sizeof(f));
    LS_CHECK(!memcmp(ntb, "NCMH", 4));
    LS_EQ_UINT(ntb[4] | ntb[5] << 8, 12);
    LS_EQ_UINT(ntb[6] | ntb[7] << 8, 7);
    LS_EQ_UINT((size_t)(ntb[8] | ntb[9] << 8), n);
    const size_t ndp = ntb[10] | ntb[11] << 8;
    LS_EQ_UINT(ndp % 4, 0);
    LS_CHECK(!memcmp(ntb + ndp, "NCM0", 4));
    got_t g = {0};
    ls_ncm_err_t e;
    LS_EQ_INT(ls_ncm_parse(ntb, n, take, &g, &e), 1);
    LS_EQ_INT(e, LS_NCM_OK);
    LS_EQ_UINT(g.len[0], sizeof(f));
    LS_CHECK(!memcmp(g.at[0], f, sizeof(f)));
    LS_EQ_UINT((size_t)(g.at[0] - ntb) % 4, 0);
}

LS_CASE(build_honours_divisor_and_remainder)
{
    ls_ncm_params_t p = P;
    p.out_divisor = 32;
    p.out_remainder = 14; /* puts the IP header of the frame on a 32-byte boundary */
    p.out_align = 8;
    uint8_t f[60], ntb[512];
    frame(f, sizeof(f), 9);
    const size_t n = ls_ncm_build(ntb, sizeof(ntb), &p, 512, 1, f, sizeof(f));
    LS_CHECK(n);
    LS_EQ_UINT((size_t)(ntb[10] | ntb[11] << 8) % 8, 0);
    got_t g = {0};
    LS_EQ_INT(ls_ncm_parse(ntb, n, take, &g, NULL), 1);
    LS_EQ_UINT((size_t)(g.at[0] - ntb) % 32, 14);
    LS_CHECK(!memcmp(g.at[0], f, sizeof(f)));
}

LS_CASE(pad_byte_when_block_is_whole_packets)
{
    /* Layout 12 + 16 = 28 header bytes: a 484-byte frame makes exactly 512. */
    uint8_t f[600], ntb[2048];
    frame(f, sizeof(f), 1);
    size_t n = ls_ncm_build(ntb, sizeof(ntb), &P, 512, 0, f, 484);
    LS_EQ_UINT(n, 513);
    LS_EQ_UINT((size_t)(ntb[8] | ntb[9] << 8), 513);
    LS_EQ_UINT(ntb[512], 0);
    got_t g = {0};
    LS_EQ_INT(ls_ncm_parse(ntb, n, take, &g, NULL), 1);
    LS_EQ_UINT(g.len[0], 484);
    /* One byte either side: no pad. */
    LS_EQ_UINT(ls_ncm_build(ntb, sizeof(ntb), &P, 512, 0, f, 483), 511);
    LS_EQ_UINT(ls_ncm_build(ntb, sizeof(ntb), &P, 512, 0, f, 485), 513);
    /* No room for the pad within out_max: sent as is. */
    ls_ncm_params_t tight = P;
    tight.out_max = 512;
    LS_EQ_UINT(ls_ncm_build(ntb, sizeof(ntb), &tight, 512, 0, f, 484), 512);
}

LS_CASE(build_refuses_what_does_not_fit)
{
    uint8_t f[1514], ntb[1024];
    frame(f, sizeof(f), 1);
    LS_EQ_UINT(ls_ncm_build(ntb, sizeof(ntb), &P, 512, 0, f, sizeof(f)), 0);
    ls_ncm_params_t small = P;
    small.out_max = 600;
    uint8_t big[2048];
    LS_EQ_UINT(ls_ncm_build(big, sizeof(big), &small, 512, 0, f, 1000), 0);
    LS_EQ_UINT(ls_ncm_build(big, sizeof(big), &P, 512, 0, f, 13), 0);
}

/* A hand-made NTB: header, then NDPs and datagrams placed by the test. */
static size_t nth(uint8_t *b, uint16_t block, uint16_t ndp)
{
    memcpy(b, "NCMH", 4);
    wr16(b + 4, 12);
    wr16(b + 6, 0);
    wr16(b + 8, block);
    wr16(b + 10, ndp);
    return 12;
}

static void ndp16(uint8_t *b, uint16_t len, uint16_t next, const uint16_t *ent, int n)
{
    memcpy(b, "NCM0", 4);
    wr16(b + 4, len);
    wr16(b + 6, next);
    for (int i = 0; i < n; i++) { wr16(b + 8 + 4 * i, ent[2 * i]); wr16(b + 10 + 4 * i, ent[2 * i + 1]); }
}

LS_CASE(multi_datagram_ntb_as_devices_send_them)
{
    /* Datagrams first, NDP at the end, as Linux's gadget and most modems do. */
    uint8_t b[1024] = {0};
    nth(b, 400, 300);
    frame(b + 12, 60, 1);
    frame(b + 72, 100, 2);
    frame(b + 172, 64, 3);
    const uint16_t e[] = { 12, 60, 72, 100, 172, 64, 0, 0 };
    ndp16(b + 300, 24, 0, e, 4);
    got_t g = {0};
    ls_ncm_err_t err;
    LS_EQ_INT(ls_ncm_parse(b, 400, take, &g, &err), 3);
    LS_EQ_INT(err, LS_NCM_OK);
    LS_EQ_UINT(g.len[1], 100);
    LS_CHECK(g.at[2] == b + 172);
}

LS_CASE(two_ndps_chained)
{
    uint8_t b[512] = {0};
    nth(b, 300, 200);
    const uint16_t e1[] = { 12, 60, 0, 0 };
    const uint16_t e2[] = { 72, 70, 0, 0 };
    ndp16(b + 200, 16, 240, e1, 2);
    ndp16(b + 240, 16, 0, e2, 2);
    got_t g = {0};
    ls_ncm_err_t err;
    LS_EQ_INT(ls_ncm_parse(b, 300, take, &g, &err), 2);
    LS_EQ_INT(err, LS_NCM_OK);
    LS_EQ_UINT(g.len[0], 60);
    LS_EQ_UINT(g.len[1], 70);
}

LS_CASE(ndp_chain_loop_is_cut)
{
    uint8_t b[512] = {0};
    nth(b, 300, 200);
    const uint16_t e[] = { 12, 60, 0, 0 };
    ndp16(b + 200, 16, 200, e, 2); /* points at itself */
    got_t g = {0};
    ls_ncm_err_t err;
    LS_EQ_INT(ls_ncm_parse(b, 300, take, &g, &err), 8);
    LS_EQ_INT(err, LS_NCM_E_CHAIN);
}

LS_CASE(bad_signatures)
{
    uint8_t f[100], b[512];
    frame(f, sizeof(f), 5);
    size_t n = ls_ncm_build(b, sizeof(b), &P, 512, 0, f, sizeof(f));
    ls_ncm_err_t err;
    b[0] = 'X';
    LS_EQ_INT(ls_ncm_parse(b, n, take, &(got_t){0}, &err), 0);
    LS_EQ_INT(err, LS_NCM_E_SIG);
    n = ls_ncm_build(b, sizeof(b), &P, 512, 0, f, sizeof(f));
    b[12] = 'X';
    LS_EQ_INT(ls_ncm_parse(b, n, take, &(got_t){0}, &err), 0);
    LS_EQ_INT(err, LS_NCM_E_NDP_SIG);
    n = ls_ncm_build(b, sizeof(b), &P, 512, 0, f, sizeof(f));
    b[4] = 16;
    LS_EQ_INT(ls_ncm_parse(b, n, take, &(got_t){0}, &err), 0);
    LS_EQ_INT(err, LS_NCM_E_HDRLEN);
    LS_EQ_INT(ls_ncm_parse(b, 8, take, &(got_t){0}, &err), 0);
    LS_EQ_INT(err, LS_NCM_E_SHORT);
}

LS_CASE(ndp_index_out_of_range)
{
    uint8_t b[256] = {0};
    ls_ncm_err_t err;
    nth(b, 200, 196); /* NDP header would run past the block */
    LS_EQ_INT(ls_ncm_parse(b, 200, take, &(got_t){0}, &err), 0);
    LS_EQ_INT(err, LS_NCM_E_NDP_INDEX);
    nth(b, 200, 4); /* inside the NTH */
    LS_EQ_INT(ls_ncm_parse(b, 200, take, &(got_t){0}, &err), 0);
    LS_EQ_INT(err, LS_NCM_E_NDP_INDEX);
    nth(b, 200, 102); /* not 4-aligned */
    LS_EQ_INT(ls_ncm_parse(b, 200, take, &(got_t){0}, &err), 0);
    LS_EQ_INT(err, LS_NCM_E_NDP_INDEX);
    /* wLength running past the block */
    nth(b, 200, 180);
    const uint16_t e[] = { 12, 60, 0, 0 };
    ndp16(b + 180, 32, 0, e, 2);
    LS_EQ_INT(ls_ncm_parse(b, 200, take, &(got_t){0}, &err), 0);
    LS_EQ_INT(err, LS_NCM_E_NDP_LEN);
}

LS_CASE(block_length_past_transfer)
{
    uint8_t f[100], b[512];
    frame(f, sizeof(f), 5);
    const size_t n = ls_ncm_build(b, sizeof(b), &P, 512, 0, f, sizeof(f));
    ls_ncm_err_t err;
    LS_EQ_INT(ls_ncm_parse(b, n - 1, take, &(got_t){0}, &err), 0);
    LS_EQ_INT(err, LS_NCM_E_BLOCKLEN);
    /* wBlockLength 0: the transfer's length is the block's. */
    wr16(b + 8, 0);
    got_t g = {0};
    LS_EQ_INT(ls_ncm_parse(b, n, take, &g, &err), 1);
    LS_EQ_INT(err, LS_NCM_OK);
}

LS_CASE(entry_overrun_skipped_rest_delivered)
{
    uint8_t b[512] = {0};
    nth(b, 300, 200);
    /* second entry runs past wBlockLength, third is too short for Ethernet */
    const uint16_t e[] = { 12, 60, 100, 250, 72, 10, 80, 64, 0, 0 };
    ndp16(b + 200, 28, 0, e, 5);
    got_t g = {0};
    ls_ncm_err_t err;
    LS_EQ_INT(ls_ncm_parse(b, 300, take, &g, &err), 2);
    LS_EQ_INT(err, LS_NCM_E_ENTRY);
    LS_EQ_UINT(g.len[0], 60);
    LS_EQ_UINT(g.len[1], 64);
}

LS_CASE(zero_length_chain)
{
    /* An NDP whose first entry is the terminator carries nothing, and is
       not an error (devices send these while idle). wNdpIndex 0 means no
       NDP at all. */
    uint8_t b[256] = {0};
    nth(b, 100, 12);
    ndp16(b + 12, 16, 0, (const uint16_t[]){ 0, 0 }, 1);
    got_t g = {0};
    ls_ncm_err_t err;
    LS_EQ_INT(ls_ncm_parse(b, 100, take, &g, &err), 0);
    LS_EQ_INT(err, LS_NCM_OK);
    nth(b, 100, 0);
    LS_EQ_INT(ls_ncm_parse(b, 100, take, &g, &err), 0);
    LS_EQ_INT(err, LS_NCM_OK);
}

LS_CASE(crc_ndp_strips_trailer)
{
    uint8_t b[256] = {0};
    nth(b, 120, 100);
    const uint16_t e[] = { 12, 64, 0, 0 };
    ndp16(b + 100, 16, 0, e, 2);
    memcpy(b + 100, "NCM1", 4);
    got_t g = {0};
    LS_EQ_INT(ls_ncm_parse(b, 120, take, &g, NULL), 1);
    LS_EQ_UINT(g.len[0], 60);
}
