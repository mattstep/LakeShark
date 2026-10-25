/* 'bc': the calculator on the serial console.

   bc <expression...>  the arguments are joined with spaces (the console has
                       already split them and stripped quotes) and run as one
                       line, so `bc 2 * 3` and `bc "scale=2; 1/3"` both work.
   bc                  a bc> session until quit, Ctrl-D or Ctrl-C.

   The session reads with linenoise() on the REPL task, the same call and the
   same stdin the console prompt itself uses one frame up, so typing echoes
   and edits exactly as it does at lakeshark>, Up recalls earlier lines, and
   it falls back to the same dumb mode on a terminal without escapes. fgets()
   on that stdin would read the line too, but nothing echoes on a UART except
   linenoise, so the operator would type blind.

   One context, in PSRAM, kept between invocations: `bc x=2` then `bc x*3`
   prints 6, and a session sees what the one-shot form set. TERMINAL runs
   console commands on its own task with stdout redirected to its scrollback
   (ls_term_esp.c) and stdin still on the UART, so a session there would wait
   on a keyboard nobody is typing at: it is refused, and the BC app is the
   calculator for the board's own keys. */

#include "sdkconfig.h"
#if defined(CONFIG_LS_BC) && CONFIG_LS_BC

#include "ls_bc_cmd.h"

#include "bc.h"
#include "esp_attr.h"
#include "linenoise/linenoise.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_BC 256

EXT_RAM_BSS_ATTR static bc_ctx_t s_bc;
EXT_RAM_BSS_ATTR static char     s_line[LINE_MAX_BC];
EXT_RAM_BSS_ATTR static char     s_out[1024];
static bool        s_ready;
/* The REPL and TERMINAL's runner are serialised by console_lock.c, so this
   only matters if that ever changes; it is cheaper than finding out. */
static atomic_flag s_busy = ATOMIC_FLAG_INIT;

static int run_line(const char *line)
{
    const bc_status_t st = bc_run(&s_bc, line, s_out, sizeof(s_out));
    fputs(s_out, stdout);
    return st == BC_ERROR ? 1 : 0;
}

static void session(void)
{
    printf("bc: scale=%d ibase=%d obase=%d. 'quit', Ctrl-D or Ctrl-C to leave; docs/BC.md for the rest.\n",
           s_bc.scale, s_bc.ibase, s_bc.obase);
    for (;;) {
        char *line = linenoise("bc> ");
        if (!line) break;                        /* Ctrl-C, Ctrl-D, or no memory */
        if (line[0]) linenoiseHistoryAdd(line);
        const bc_status_t st = bc_run(&s_bc, line, s_out, sizeof(s_out));
        linenoiseFree(line);
        fputs(s_out, stdout);
        fflush(stdout);
        if (st == BC_QUIT) break;
    }
}

int ls_bc_command(int argc, char **argv)
{
    if (atomic_flag_test_and_set(&s_busy)) {
        printf("bc: already running in another session\n");
        return 1;
    }
    if (!s_ready) {
        bc_init(&s_bc);
        s_ready = true;
    }
    int rc = 0;
    if (argc > 1) {
        size_t n = 0;
        s_line[0] = '\0';
        for (int i = 1; i < argc && rc == 0; i++) {
            const size_t k = strlen(argv[i]);
            if (n + k + 2 > sizeof(s_line)) { printf("bc: expression too long\n"); rc = 1; break; }
            if (n) s_line[n++] = ' ';
            memcpy(s_line + n, argv[i], k + 1);
            n += k;
        }
        if (rc == 0) rc = run_line(s_line);
    } else if (fileno(stdout) < 0 || fileno(stdin) < 0) {
        printf("bc: a bc> session needs the serial console. Here, give the expression "
               "(bc 2^10), or open the BC app in SYSTEM.\n");
        rc = 1;
    } else {
        session();
    }
    atomic_flag_clear(&s_busy);
    return rc;
}

#endif
