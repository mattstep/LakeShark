/* components/lsssh/ls_ed25519_adapter.c: libssh's crypto_sign_ed25519* API
   on MeshCore's orlp/ed25519. RFC 8032 section 7.1 vectors through libssh's
   seed || pk key layout, NaCl-style signed messages, and keypair round trip.

   Registered in bench/CMakeLists.txt. Plain cc build, from the repo root
   (ref10's fe.c left-shifts negative limbs, so that one UBSan check is off for
   the vendored ed25519 files, as bench/CMakeLists.txt does under LS_SANITIZE):
     E=components/meshcore/upstream/ed25519
     F="-std=c11 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
     for f in fe ge sc keypair sign verify sha512; do
       cc $F -fno-sanitize=shift-base -w -I$E -c $E/$f.c -o /tmp/ed_$f.o; done
     cc $F -Wall -Wextra -Ibench/harness -Ibench/shims \
        -Ibench/tests/ed25519_shims -I$E bench/tests/test_ed25519_adapter.c \
        bench/harness/ls_test.c bench/harness/shim_impl.c \
        components/lsssh/ls_ed25519_adapter.c /tmp/ed_*.o \
        -o /tmp/test_ed25519_adapter && /tmp/test_ed25519_adapter */

#include "ls_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libssh/ed25519.h"
#include "ed_25519.h"

/* libssh's randomness hook, which the adapter's keypair call uses.
   Deterministic here; returns 1 on success like libssh's. */
static ls_rng_t g_rng;
static int g_random_fail;
int ssh_get_random(void *where, int len, int strong)
{
    (void)strong;
    if (g_random_fail) return 0;
    unsigned char *p = where;
    for (int i = 0; i < len; i++) p[i] = (unsigned char)(ls_rng_u32(&g_rng) >> 24);
    return 1;
}

static void unhex(const char *h, unsigned char *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

typedef struct {
    const char *sk, *pk, *msg, *sig;
} vec_t;

/* RFC 8032, 7.1, TEST 1..3. */
static const vec_t VECS[] = {
    { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
      "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
      "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
      "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
      "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    { "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
      "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
      "af82",
      "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
};

LS_CASE(ed25519_rfc8032_vectors)
{
    for (size_t v = 0; v < sizeof VECS / sizeof VECS[0]; v++) {
        const vec_t *t = &VECS[v];
        ed25519_privkey sk;
        ed25519_pubkey pk;
        unsigned char want_sig[64];
        size_t mlen = strlen(t->msg) / 2;
        unsigned char *msg = malloc(mlen + 1);
        unhex(t->sk, sk, 32);
        unhex(t->pk, sk + 32, 32);
        unhex(t->pk, pk, 32);
        unhex(t->msg, msg, mlen);
        unhex(t->sig, want_sig, 64);

        /* The seed derives the vector's public key (what 'ssh pubkey' relies on). */
        unsigned char dpk[32], dsk[64];
        ed25519_create_keypair(dpk, dsk, sk);
        LS_CHECK_MSG(memcmp(dpk, pk, 32) == 0, "TEST %zu: derived pk differs", v + 1);

        unsigned char *sm = malloc(mlen + 64);
        uint64_t smlen = 0;
        LS_EQ_INT(crypto_sign_ed25519(sm, &smlen, msg, mlen, sk), 0);
        LS_EQ_UINT(smlen, mlen + 64);
        LS_CHECK_MSG(memcmp(sm, want_sig, 64) == 0, "TEST %zu: signature differs", v + 1);
        LS_CHECK(memcmp(sm + 64, msg, mlen) == 0);

        unsigned char *out = malloc(mlen + 64);
        uint64_t olen = 0;
        LS_EQ_INT(crypto_sign_ed25519_open(out, &olen, sm, smlen, pk), 0);
        LS_EQ_UINT(olen, mlen);
        LS_CHECK(memcmp(out, msg, mlen) == 0);

        /* Any flipped bit in signature or message must fail, output zeroed. */
        for (size_t bit = 0; bit < (size_t)smlen * 8; bit += 7) {
            sm[bit / 8] ^= (unsigned char)(1u << (bit % 8));
            memset(out, 0xAA, mlen + 64);
            olen = 0;
            LS_CHECK_MSG(crypto_sign_ed25519_open(out, &olen, sm, smlen, pk) == -1,
                         "TEST %zu: bit %zu flip verified", v + 1, bit);
            LS_EQ_UINT(olen, (uint64_t)-1);
            for (size_t i = 0; i < mlen; i++) LS_EQ_INT(out[i], 0);
            sm[bit / 8] ^= (unsigned char)(1u << (bit % 8));
        }

        /* Wrong public key fails. */
        ed25519_pubkey bad;
        memcpy(bad, pk, 32);
        bad[0] ^= 1;
        LS_EQ_INT(crypto_sign_ed25519_open(out, &olen, sm, smlen, bad), -1);

        /* Signing in place (sm == m) gives the same bytes. */
        unsigned char *inplace = malloc(mlen + 64);
        memcpy(inplace, msg, mlen);
        LS_EQ_INT(crypto_sign_ed25519(inplace, &smlen, inplace, mlen, sk), 0);
        LS_CHECK(memcmp(inplace, sm, mlen + 64) == 0);

        free(inplace);
        free(out);
        free(sm);
        free(msg);
    }
}

LS_CASE(ed25519_short_signed_message)
{
    unsigned char sm[63] = {0}, out[64];
    uint64_t olen = 0;
    ed25519_pubkey pk = {0};
    LS_EQ_INT(crypto_sign_ed25519_open(out, &olen, sm, sizeof sm, pk), -1);
    LS_EQ_UINT(olen, (uint64_t)-1);
}

LS_CASE(ed25519_keypair_round_trip)
{
    ls_rng_seed(&g_rng, 0x5eed25519ull);
    for (int k = 0; k < 8; k++) {
        ed25519_pubkey pk;
        ed25519_privkey sk;
        LS_EQ_INT(crypto_sign_ed25519_keypair(pk, sk), 0);
        LS_CHECK(memcmp(sk + 32, pk, 32) == 0);   /* libssh layout: seed || pk */

        unsigned char dpk[32], dsk[64];
        ed25519_create_keypair(dpk, dsk, sk);
        LS_CHECK(memcmp(dpk, pk, 32) == 0);

        unsigned char msg[48], sm[48 + 64], out[48 + 64];
        for (int i = 0; i < 48; i++) msg[i] = (unsigned char)(k * 31 + i);
        uint64_t smlen = 0, olen = 0;
        LS_EQ_INT(crypto_sign_ed25519(sm, &smlen, msg, sizeof msg, sk), 0);
        LS_EQ_INT(crypto_sign_ed25519_open(out, &olen, sm, smlen, pk), 0);
        LS_EQ_UINT(olen, sizeof msg);
        LS_CHECK(memcmp(out, msg, sizeof msg) == 0);
        sm[70] ^= 0x10;
        LS_EQ_INT(crypto_sign_ed25519_open(out, &olen, sm, smlen, pk), -1);
    }

    ed25519_pubkey pk;
    ed25519_privkey sk;
    g_random_fail = 1;
    LS_EQ_INT(crypto_sign_ed25519_keypair(pk, sk), -1);
    g_random_fail = 0;
}
