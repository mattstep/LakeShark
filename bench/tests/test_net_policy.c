/* LS_TEST_SOURCES: ${FW}/components/lsnet/ls_net_policy.c */
#include "ls_test.h"
#include "ls_net_policy.h"

/* Default route and resolver choice across Wi-Fi, Ethernet and LTE. The
   field case: Ethernet (prio 120) on the LAN, LTE (50) holding the
   carrier's 10.177.x resolvers, Wi-Fi (100) on the same LAN. Whatever is
   chosen, lwIP must get the chosen interface's own servers. */

#define A(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static ls_net_if_t lte(void)
{
    return (ls_net_if_t){ "LTE", true, 50, A(48, 64, 76, 10), { A(10, 177, 0, 34), A(10, 177, 0, 210) } };
}
static ls_net_if_t eth(void)
{
    return (ls_net_if_t){ "ETH", true, 120, A(172, 18, 0, 1), { A(1, 1, 1, 1), A(8, 8, 8, 8) } };
}
static ls_net_if_t wifi(void)
{
    return (ls_net_if_t){ "WIFI_STA_DEF", true, 100, A(172, 18, 0, 1), { A(1, 1, 1, 1), A(8, 8, 8, 8) } };
}

LS_CASE(highest_priority_up_carries_its_own_dns)
{
    const ls_net_if_t ifs[] = { lte(), eth(), wifi() };
    ls_net_choice_t c;
    ls_net_choose(ifs, 3, "auto", &c);
    LS_EQ_INT(c.index, 1);
    LS_CHECK(!c.preferred);
    LS_EQ_UINT(c.dns[0], A(1, 1, 1, 1));
    LS_EQ_UINT(c.dns[1], A(8, 8, 8, 8));
    LS_CHECK(!c.dns_from_gw);
}

LS_CASE(order_of_the_list_does_not_matter)
{
    const ls_net_if_t a[] = { eth(), lte(), wifi() };
    const ls_net_if_t b[] = { wifi(), lte(), eth() };
    ls_net_choice_t c;
    ls_net_choose(a, 3, NULL, &c);
    LS_EQ_STR(a[c.index].key, "ETH");
    ls_net_choose(b, 3, "", &c);
    LS_EQ_STR(b[c.index].key, "ETH");
    LS_EQ_UINT(c.dns[0], A(1, 1, 1, 1));
}

LS_CASE(preference_wins_when_up)
{
    const ls_net_if_t ifs[] = { lte(), eth(), wifi() };
    ls_net_choice_t c;
    ls_net_choose(ifs, 3, "LTE", &c);
    LS_EQ_INT(c.index, 0);
    LS_CHECK(c.preferred);
    LS_EQ_UINT(c.dns[0], A(10, 177, 0, 34));
    LS_EQ_UINT(c.dns[1], A(10, 177, 0, 210));
}

LS_CASE(preference_down_falls_back_to_priority)
{
    ls_net_if_t ifs[] = { lte(), eth(), wifi() };
    ifs[0].up = false;
    ls_net_choice_t c;
    ls_net_choose(ifs, 3, "LTE", &c);
    LS_EQ_INT(c.index, 1);
    LS_CHECK(!c.preferred);
    ls_net_choose(ifs, 3, "NOSUCH", &c);
    LS_EQ_INT(c.index, 1);
}

LS_CASE(down_interfaces_never_chosen)
{
    ls_net_if_t ifs[] = { lte(), eth(), wifi() };
    ifs[1].up = false;
    ls_net_choice_t c;
    ls_net_choose(ifs, 3, "auto", &c);
    LS_EQ_STR(ifs[c.index].key, "WIFI_STA_DEF");
    ifs[2].up = false;
    ls_net_choose(ifs, 3, "auto", &c);
    LS_EQ_STR(ifs[c.index].key, "LTE");
    LS_EQ_UINT(c.dns[0], A(10, 177, 0, 34));
    ifs[0].up = false;
    ls_net_choose(ifs, 3, "auto", &c);
    LS_EQ_INT(c.index, -1);
    LS_EQ_UINT(c.dns[0], 0);
    ls_net_choose(ifs, 0, "auto", &c);
    LS_EQ_INT(c.index, -1);
}

LS_CASE(lan_without_dns_uses_its_gateway_not_another_interfaces_servers)
{
    ls_net_if_t ifs[] = { lte(), eth() };
    ifs[1].dns[0] = ifs[1].dns[1] = 0;
    ls_net_choice_t c;
    ls_net_choose(ifs, 2, "auto", &c);
    LS_EQ_INT(c.index, 1);
    LS_CHECK(c.dns_from_gw);
    LS_EQ_UINT(c.dns[0], A(172, 18, 0, 1));
    LS_EQ_UINT(c.dns[1], 0);
    /* Only a backup server: it moves up to main. */
    ifs[1].dns[1] = A(9, 9, 9, 9);
    ls_net_choose(ifs, 2, "auto", &c);
    LS_EQ_UINT(c.dns[0], A(9, 9, 9, 9));
    LS_CHECK(!c.dns_from_gw);
    /* No DNS and no gateway: nothing, rather than LTE's. */
    ifs[1].dns[1] = 0;
    ifs[1].gw = 0;
    ls_net_choose(ifs, 2, "auto", &c);
    LS_EQ_INT(c.index, 1);
    LS_EQ_UINT(c.dns[0], 0);
    LS_EQ_UINT(c.dns[1], 0);
}

LS_CASE(equal_priority_first_wins)
{
    ls_net_if_t ifs[] = { wifi(), eth() };
    ifs[1].prio = 100;
    ls_net_choice_t c;
    ls_net_choose(ifs, 2, "auto", &c);
    LS_EQ_INT(c.index, 0);
}
