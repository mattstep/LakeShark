/* LS_TEST_SOURCES: ${FW}/components/lslte/ls_lte_at.c */
#include "ls_test.h"
#include "ls_lte_at.h"

#include <stdio.h>
#include <string.h>

/* The Huawei stick's AT channel as recorded from an MS2372h-517: replies
   split across USB packets, URCs dropped in anywhere, and the replies the
   LTE driver decodes. */

typedef struct {
    int urcs;
    char urc[8][LS_AT_LINE_MAX];
    int dones;
    ls_at_result_t res;
    char resp[LS_AT_RESP_MAX];
} rec_t;

static void on_urc(void *ctx, const char *line)
{
    rec_t *r = ctx;
    if (r->urcs < 8) snprintf(r->urc[r->urcs], sizeof(r->urc[0]), "%s", line);
    r->urcs++;
}

static void on_done(void *ctx, ls_at_result_t res, const char *resp)
{
    rec_t *r = ctx;
    r->dones++;
    r->res = res;
    snprintf(r->resp, sizeof(r->resp), "%s", resp);
}

static void feed(ls_at_t *a, const char *s)
{
    ls_at_feed(a, (const uint8_t *)s, strlen(s));
}

/* Every split point of the same byte stream gives the same result. */
LS_CASE(lines_split_across_chunks)
{
    const char *stream = "\r\n^RSSI:30\r\n\r\n+CSQ: 30,99\r\n\r\nOK\r\n";
    const size_t n = strlen(stream);
    for (size_t cut = 0; cut <= n; cut++) {
        static ls_at_t a;
        rec_t r = {0};
        ls_at_init(&a, on_urc, on_done, &r);
        ls_at_begin(&a, "AT+CSQ");
        ls_at_feed(&a, (const uint8_t *)stream, cut);
        ls_at_feed(&a, (const uint8_t *)stream + cut, n - cut);
        LS_EQ_INT(r.dones, 1);
        LS_EQ_INT(r.res, LS_AT_RES_OK);
        LS_EQ_STR(r.resp, "+CSQ: 30,99\nOK\n");
        LS_EQ_INT(r.urcs, 1);
        LS_EQ_STR(r.urc[0], "^RSSI:30");
    }
    /* Byte by byte. */
    static ls_at_t a;
    rec_t r = {0};
    ls_at_init(&a, on_urc, on_done, &r);
    ls_at_begin(&a, "AT+CSQ");
    for (size_t i = 0; i < n; i++) ls_at_feed(&a, (const uint8_t *)stream + i, 1);
    LS_EQ_STR(r.resp, "+CSQ: 30,99\nOK\n");
}

LS_CASE(urcs_while_idle_and_mid_reply)
{
    static ls_at_t a;
    rec_t r = {0};
    ls_at_init(&a, on_urc, on_done, &r);
    feed(&a, "\r\n^HCSQ:\"LTE\",62,63,153,22\r\n");
    LS_EQ_INT(r.urcs, 1);
    ls_at_begin(&a, "AT+COPS?");
    feed(&a, "\r\n+COPS: 0,0,\"T-Mobile\",7\r\n\r\n^NDISSTAT:1,,,\"IPV4\"\r\n\r\nOK\r\n");
    LS_EQ_INT(r.urcs, 2);
    LS_EQ_STR(r.urc[1], "^NDISSTAT:1,,,\"IPV4\"");
    LS_EQ_STR(r.resp, "+COPS: 0,0,\"T-Mobile\",7\nOK\n");
    /* Unknown unsolicited text while idle is a URC too. */
    feed(&a, "\r\n^STIN: 0,0\r\n");
    LS_EQ_INT(r.urcs, 3);
}

LS_CASE(urc_that_is_the_reply)
{
    static ls_at_t a;
    rec_t r = {0};
    ls_at_init(&a, on_urc, on_done, &r);
    ls_at_begin(&a, "AT^HCSQ?");
    feed(&a, "\r\n^HCSQ:\"LTE\",62,63,153,22\r\n\r\nOK\r\n");
    LS_EQ_STR(r.resp, "^HCSQ:\"LTE\",62,63,153,22\nOK\n");
    LS_EQ_INT(r.urcs, 1);
}

LS_CASE(terminators)
{
    static const struct { const char *in; ls_at_result_t res; } t[] = {
        { "\r\nOK\r\n", LS_AT_RES_OK },
        { "\r\nERROR\r\n", LS_AT_RES_ERROR },
        { "\r\nCOMMAND NOT SUPPORT\r\n", LS_AT_RES_NOT_SUPPORTED },
        { "\r\n+CME ERROR: SIM not inserted\r\n", LS_AT_RES_CME },
        { "\r\n+CMS ERROR: 500\r\n", LS_AT_RES_CME },
        { "\r\nNO CARRIER\r\n", LS_AT_RES_NO_CARRIER },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        static ls_at_t a;
        rec_t r = {0};
        ls_at_init(&a, on_urc, on_done, &r);
        ls_at_begin(&a, "AT^SETPORT?");
        feed(&a, t[i].in);
        LS_EQ_INT(r.dones, 1);
        LS_EQ_INT(r.res, t[i].res);
    }
    /* Not finals: OK inside a line, "OKAY", an empty line. */
    LS_EQ_INT(ls_at_final("OKAY"), LS_AT_RES_NONE);
    LS_EQ_INT(ls_at_final("+CME ERRO"), LS_AT_RES_NONE);
    LS_EQ_INT(ls_at_final(""), LS_AT_RES_NONE);
    /* No command in flight: a stray OK completes nothing. */
    static ls_at_t a;
    rec_t r = {0};
    ls_at_init(&a, on_urc, on_done, &r);
    feed(&a, "\r\nOK\r\n");
    LS_EQ_INT(r.dones, 0);
}

LS_CASE(echo_dropped_and_abort)
{
    static ls_at_t a;
    rec_t r = {0};
    ls_at_init(&a, on_urc, on_done, &r);
    ls_at_begin(&a, "ATI");
    feed(&a, "ATI\r\r\nManufacturer: huawei\r\nModel: MS2372h-517\r\n"
             "Revision: 21.328.02.01.00\r\nIMEI: 861234567890123\r\n+GCAP: +CGSM\r\n\r\nOK\r\n");
    LS_EQ_INT(r.res, LS_AT_RES_OK);
    char model[32], rev[32], imei[20];
    ls_at_parse_ati(r.resp, model, sizeof(model), rev, sizeof(rev), imei, sizeof(imei));
    LS_EQ_STR(model, "MS2372h-517");
    LS_EQ_STR(rev, "21.328.02.01.00");
    LS_EQ_STR(imei, "861234567890123");
    LS_CHECK(!strstr(r.resp, "ATI\n"));

    ls_at_begin(&a, "AT+CEREG?");
    feed(&a, "\r\n+CEREG: 0,");
    ls_at_abort(&a, LS_AT_RES_TIMEOUT);
    LS_EQ_INT(r.dones, 2);
    LS_EQ_INT(r.res, LS_AT_RES_TIMEOUT);
    ls_at_abort(&a, LS_AT_RES_TIMEOUT); /* nothing in flight: no second call */
    LS_EQ_INT(r.dones, 2);
}

LS_CASE(overlong_line_dropped)
{
    static ls_at_t a;
    rec_t r = {0};
    ls_at_init(&a, on_urc, on_done, &r);
    ls_at_begin(&a, "AT+CGDCONT?");
    char big[600];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    feed(&a, "\r\n+CGDCONT: ");
    feed(&a, big);
    feed(&a, "\r\n+CGDCONT: 1,\"IP\",\"h2g2\",\"0.0.0.0\",0,0,0,0\r\n\r\nOK\r\n");
    LS_EQ_INT(r.res, LS_AT_RES_OK);
    LS_EQ_STR(r.resp, "+CGDCONT: 1,\"IP\",\"h2g2\",\"0.0.0.0\",0,0,0,0\nOK\n");
}

LS_CASE(cmd_prefix)
{
    char p[32];
    ls_at_cmd_prefix("AT+CSQ", p, sizeof(p));            LS_EQ_STR(p, "+CSQ:");
    ls_at_cmd_prefix("AT^NDISSTATQRY?", p, sizeof(p));   LS_EQ_STR(p, "^NDISSTATQRY:");
    ls_at_cmd_prefix("AT^NDISDUP=1,1,\"h2g2\"", p, sizeof(p)); LS_EQ_STR(p, "^NDISDUP:");
    ls_at_cmd_prefix("ATI", p, sizeof(p));               LS_EQ_STR(p, "");
    ls_at_cmd_prefix("ATE0", p, sizeof(p));              LS_EQ_STR(p, "");
    ls_at_cmd_prefix("at+cops?", p, sizeof(p));          LS_EQ_STR(p, "+cops:");
}

static void dotted(const uint8_t a[4], char *out)
{
    snprintf(out, 20, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

LS_CASE(dhcp_decode)
{
    ls_at_dhcp_t d;
    const char *resp = "^DHCP: 68702130,F0FFFFFF,61702130,61702130,2200B10A,D200B10A,"
                       "150000000,150000000\nOK\n";
    LS_CHECK(ls_at_parse_dhcp(resp, &d));
    char s[20];
    dotted(d.ip, s);     LS_EQ_STR(s, "48.33.112.104");
    dotted(d.mask, s);   LS_EQ_STR(s, "255.255.255.240");
    dotted(d.gw, s);     LS_EQ_STR(s, "48.33.112.97");
    dotted(d.server, s); LS_EQ_STR(s, "48.33.112.97");
    dotted(d.dns1, s);   LS_EQ_STR(s, "10.177.0.34");
    dotted(d.dns2, s);   LS_EQ_STR(s, "10.177.0.210");
    LS_EQ_UINT(d.rx_rate, 150000000);
    LS_EQ_UINT(d.tx_rate, 150000000);
    LS_CHECK(!ls_at_parse_dhcp("^DHCP: 6870213,F0FFFFFF,61702130\nOK\n", &d));
    LS_CHECK(!ls_at_parse_dhcp("OK\n", &d));
}

LS_CASE(hcsq_parse)
{
    ls_at_signal_t s;
    LS_CHECK(ls_at_parse_hcsq("^HCSQ:\"LTE\",62,63,153,22", &s));
    LS_EQ_STR(s.mode, "LTE");
    LS_EQ_INT(s.rssi_dbm, -59);
    LS_EQ_INT(s.rsrp_dbm, -78);
    LS_EQ_INT(s.sinr_db10, 104);  /* 10.4 dB */
    LS_EQ_INT(s.rsrq_db10, -90);  /* -9.0 dB */
    LS_CHECK(ls_at_parse_hcsq("^HCSQ:\"NOSERVICE\"", &s));
    LS_EQ_STR(s.mode, "NOSERVICE");
    LS_EQ_INT(s.rssi_dbm, LS_AT_SIG_UNKNOWN);
    LS_CHECK(ls_at_parse_hcsq("^HCSQ:\"WCDMA\",30,30,58", &s));
    LS_EQ_INT(s.rssi_dbm, -91);
    LS_EQ_INT(s.rsrp_dbm, LS_AT_SIG_UNKNOWN);
    LS_CHECK(ls_at_parse_hcsq("^HCSQ:\"LTE\",255,255,255,255", &s));
    LS_EQ_INT(s.rssi_dbm, LS_AT_SIG_UNKNOWN);
    LS_CHECK(!ls_at_parse_hcsq("^RSSI:30", &s));

    int dbm;
    LS_CHECK(ls_at_parse_csq("+CSQ: 30,99", &dbm)); LS_EQ_INT(dbm, -53);
    LS_CHECK(ls_at_parse_csq("^RSSI:99", &dbm));    LS_EQ_INT(dbm, LS_AT_SIG_UNKNOWN);
}

LS_CASE(cgdcont_apn)
{
    const char *resp = "+CGDCONT: 1,\"IP\",\"h2g2\",\"0.0.0.0\",0,0,0,0\n"
                       "+CGDCONT: 2,\"IPV4V6\",\"ims\",\"0.0.0.0\",0,0,0,0\n"
                       "+CGDCONT: 3,\"IP\",\"\",\"0.0.0.0\",0,0\nOK\n";
    char apn[64];
    LS_CHECK(ls_at_parse_cgdcont_apn(resp, 1, apn, sizeof(apn))); LS_EQ_STR(apn, "h2g2");
    LS_CHECK(ls_at_parse_cgdcont_apn(resp, 2, apn, sizeof(apn))); LS_EQ_STR(apn, "ims");
    LS_CHECK(ls_at_parse_cgdcont_apn(resp, 3, apn, sizeof(apn))); LS_EQ_STR(apn, "");
    LS_CHECK(!ls_at_parse_cgdcont_apn(resp, 4, apn, sizeof(apn)));
    /* A comma inside quotes is not a field break. */
    LS_CHECK(ls_at_parse_cgdcont_apn("+CGDCONT: 1,\"IP\",\"a,b\"\nOK\n", 1, apn, sizeof(apn)));
    LS_EQ_STR(apn, "a,b");
}

LS_CASE(registration_session_operator)
{
    int st;
    LS_CHECK(ls_at_parse_reg("+CEREG: 0,1\nOK\n", "+CEREG:", &st)); LS_EQ_INT(st, 1);
    LS_CHECK(ls_at_parse_reg("+CEREG: 2,5,\"2F1A\",\"01A2B301\",7\nOK\n", "+CEREG:", &st));
    LS_EQ_INT(st, 5);
    LS_CHECK(ls_at_parse_reg("+CEREG: 2", "+CEREG:", &st)); LS_EQ_INT(st, 2);  /* URC */
    LS_CHECK(ls_at_parse_reg("+CEREG: 1,\"0123\",\"01A2B301\",7", "+CEREG:", &st));
    LS_EQ_INT(st, 1);
    LS_CHECK(!ls_at_parse_reg("OK\n", "+CEREG:", &st));

    LS_CHECK(ls_at_parse_ndisstat("^NDISSTAT:1,,,\"IPV4\"", &st)); LS_EQ_INT(st, 1);
    LS_CHECK(ls_at_parse_ndisstat("^NDISSTAT:0,36,,\"IPV4\"", &st)); LS_EQ_INT(st, 0);
    LS_CHECK(ls_at_parse_ndisstat("^NDISSTATQRY: 1,,,\"IPV4\"\nOK\n", &st)); LS_EQ_INT(st, 1);

    char op[32], mode[16];
    int act;
    LS_CHECK(ls_at_parse_cops("+COPS: 0,0,\"T-Mobile\",7\nOK\n", op, sizeof(op), &act));
    LS_EQ_STR(op, "T-Mobile");
    LS_EQ_INT(act, 7);
    LS_CHECK(ls_at_parse_cops("+COPS: 0\nOK\n", op, sizeof(op), &act));
    LS_EQ_STR(op, "");
    LS_EQ_INT(act, -1);
    LS_CHECK(ls_at_parse_sysinfoex("^SYSINFOEX:2,3,0,1,,6,\"LTE\",101,\"LTE\"\nOK\n", &st,
                                   mode, sizeof(mode)));
    LS_EQ_INT(st, 2);
    LS_EQ_STR(mode, "LTE");
}
