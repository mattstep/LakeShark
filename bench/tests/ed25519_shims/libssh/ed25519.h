/* Host stand-in for libssh-mirror/include/libssh/ed25519.h (whose priv.h
   needs libssh's full port config). Same types and prototypes; keep in step
   with the real header, which the firmware build compiles the adapter against. */
#ifndef ED25519_H_
#define ED25519_H_
#include <stdint.h>

#define ED25519_PK_LEN 32
#define ED25519_SK_LEN 64
#define ED25519_SIG_LEN 64

typedef uint8_t ed25519_pubkey[ED25519_PK_LEN];
typedef uint8_t ed25519_privkey[ED25519_SK_LEN];
typedef uint8_t ed25519_signature[ED25519_SIG_LEN];

int crypto_sign_ed25519_keypair(ed25519_pubkey pk, ed25519_privkey sk);
int crypto_sign_ed25519(unsigned char *sm, uint64_t *smlen,
                        const unsigned char *m, uint64_t mlen,
                        const ed25519_privkey sk);
int crypto_sign_ed25519_open(unsigned char *m, uint64_t *mlen,
                             const unsigned char *sm, uint64_t smlen,
                             const ed25519_pubkey pk);
#endif
