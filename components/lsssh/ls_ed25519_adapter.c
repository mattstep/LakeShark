/* libssh's Ed25519 entry points (libssh/ed25519.h) on top of the orlp/ed25519
   copy MeshCore already links (components/meshcore/upstream/ed25519).

   libssh normally compiles its own Ed25519 (src/external/ed25519.c, fe25519.c,
   ge25519.c, sc25519.c); ge25519.c alone carries ~109 KB of precomputed base
   point multiples, the same data MeshCore's precomp_data.h already puts in
   flash. The top-level CMakeLists.txt marks those four files header-only and
   compiles this file into libssh in their place, so only one table ships.

   Key format is unchanged: libssh's 64-byte secret key is seed || public key,
   as OpenSSH stores it. orlp's 64-byte private key is the clamped SHA-512 of
   the seed, so every call re-derives it from the seed. Ed25519 is
   deterministic: the signatures are byte-identical to libssh's own code.

   Only compiled when CONFIG_LS_SSH is set (from the CMake block that adds it). */

#include <stdint.h>
#include <string.h>

#include "config.h"           /* libssh port config; priv.h needs it first */
#include "libssh/ed25519.h"   /* prototypes, ed25519_pubkey/ed25519_privkey */
#include "ed_25519.h"         /* MeshCore's orlp/ed25519 */

/* libssh.h; declared again here so the host test needs only the shim header. */
int ssh_get_random(void *where, int len, int strong);

#define LS_ED_SEED_LEN 32

static void ls_ed_wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

int crypto_sign_ed25519_keypair(ed25519_pubkey pk, ed25519_privkey sk)
{
    unsigned char seed[LS_ED_SEED_LEN];
    unsigned char expanded[64];

    /* Same source and strength as libssh's external/ed25519.c. */
    if (!ssh_get_random(seed, LS_ED_SEED_LEN, 0)) {
        ls_ed_wipe(seed, sizeof seed);
        return -1;
    }
    ed25519_create_keypair(pk, expanded, seed);
    memcpy(sk, seed, LS_ED_SEED_LEN);
    memcpy(sk + LS_ED_SEED_LEN, pk, ED25519_PK_LEN);

    ls_ed_wipe(seed, sizeof seed);
    ls_ed_wipe(expanded, sizeof expanded);
    return 0;
}

int crypto_sign_ed25519(unsigned char *sm, uint64_t *smlen,
                        const unsigned char *m, uint64_t mlen,
                        const ed25519_privkey sk)
{
    unsigned char pk[ED25519_PK_LEN];
    unsigned char expanded[64];
    unsigned char sig[ED25519_SIG_LEN];

    if (mlen > (uint64_t)(SIZE_MAX - ED25519_SIG_LEN)) {
        return -1;
    }

    /* The public key is re-derived from the seed rather than taken from
       sk[32..64]: signing with a mismatched public key can leak the scalar. */
    ed25519_create_keypair(pk, expanded, sk);

    /* Message first, so sm == m (or any overlap) still works. */
    memmove(sm + ED25519_SIG_LEN, m, (size_t)mlen);
    ed25519_sign(sig, sm + ED25519_SIG_LEN, (size_t)mlen, pk, expanded);
    memcpy(sm, sig, ED25519_SIG_LEN);
    *smlen = mlen + ED25519_SIG_LEN;

    ls_ed_wipe(expanded, sizeof expanded);
    ls_ed_wipe(sig, sizeof sig);
    return 0;
}

int crypto_sign_ed25519_open(unsigned char *m, uint64_t *mlen,
                             const unsigned char *sm, uint64_t smlen,
                             const ed25519_pubkey pk)
{
    size_t n;

    *mlen = (uint64_t)-1;
    if (smlen < ED25519_SIG_LEN || smlen - ED25519_SIG_LEN > (uint64_t)SIZE_MAX) {
        return -1;
    }
    n = (size_t)(smlen - ED25519_SIG_LEN);

    if (!ed25519_verify(sm, sm + ED25519_SIG_LEN, n, pk)) {
        memset(m, 0, n);
        return -1;
    }
    memmove(m, sm + ED25519_SIG_LEN, n);
    *mlen = n;
    return 0;
}
