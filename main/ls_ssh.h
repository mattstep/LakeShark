/* SSH client: the 'ssh' console command.

   Ed25519 only. The board keeps one identity, generated on the board and
   stored in NVS; the private key never leaves it. Server keys are trusted
   on first use ('ssh -y') and then pinned in NVS, so a changed host key is
   refused until 'ssh forget <host>'.

   The command runs one remote command (or checks a login) and doubles as
   the regression test for the session engine in components/lsssh, which
   the SSH app's interactive shell uses too. */

#ifndef LS_SSH_H
#define LS_SSH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the 'ssh' console command. */
void ls_ssh_console_register(void);

#ifdef __cplusplus
}
#endif

#endif
