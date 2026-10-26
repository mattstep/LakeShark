/* A bc-style calculator over IEEE double.

   The subset of POSIX/GNU bc that is useful for arithmetic at a prompt:
   expressions, variables, scale/ibase/obase/last, and the -l maths library.
   No functions, loops, if, arrays or strings. docs/BC.md lists what differs
   from bc and why.

   Pure C with no ESP-IDF dependency, so the bench compiles it with plain cc.
   Every piece of state lives in bc_ctx_t: no malloc, no statics, so the
   console and the BC app can each hold a context of their own. The parser
   keeps its operand and operator stacks in the context rather than
   recursing, and never calls printf: the console runs it on the REPL task,
   whose 4 KB stack has about 1.4 KB to spare (ls_term_esp.c). A recursive
   parser measured over 500 bytes per level of parentheses there; this one's
   deepest call chain is about 1.4 KB at -Os however deep the nesting, which
   is the case the cap on BC_STACK is for. */

#ifndef LS_BC_H
#define LS_BC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC_VARS        32     /* named variables a context can hold */
#define BC_NAME_MAX    16     /* longest name, including the terminator */
#define BC_SCALE_MAX   20     /* digits after the point; a double has ~15 */
#define BC_SCALE_DEFAULT 6    /* bc's is 0; see docs/BC.md */
#define BC_STACK       48     /* pending operands, and operators: nesting depth */
#define BC_FMT_MAX     400    /* one formatted number: 2^1023 in decimal fits */

typedef enum {
    BC_OK = 0,
    BC_ERROR,      /* output ends with the error, caret and column */
    BC_QUIT,       /* quit, q, exit or halt as a statement */
} bc_status_t;

typedef struct {
    char   name[BC_NAME_MAX];
    double v;
    int    scale;
} bc_var_t;

/* Parser internals, here only so the context can hold them. */
typedef struct {
    double  v;
    int16_t scale;
    int16_t lv;       /* where it can be stored, for assignment and ++ */
    bool    assign;   /* outermost operation was an assignment: not printed */
} bc_val_t;           /* 16 bytes: it is copied by value through the parser */

typedef struct {
    int type, prec, pos, extra;
} bc_op_t;

typedef struct {
    int    type, pos;
    double num;
    int    num_scale;
    char   name[BC_NAME_MAX];
} bc_tok_t;

typedef struct bc_ctx_s {
    /* What persists between lines. */
    bc_var_t vars[BC_VARS];
    int      nvars;
    int      scale, ibase, obase;
    double   last;
    int      last_scale;

    /* The last error of bc_run: 1-based column in its line, 0 for none. */
    int         err_col;
    const char *err_msg;

    /* Working state for one bc_run; callers leave it alone. */
    const char *src;
    int         pos, noeval;
    bool        failed;
    int         err_pos;
    char        err_name[BC_NAME_MAX];
    char        last_name[BC_NAME_MAX];
    bc_tok_t    tok;
    bc_val_t    vals[BC_STACK];
    bc_op_t     ops[BC_STACK];
    int         nvals, nops;
    char       *out;
    size_t      out_n, out_len;
    char        fmt[BC_FMT_MAX];
} bc_ctx_t;

/* Fresh context: no variables, scale BC_SCALE_DEFAULT, ibase and obase 10. */
void bc_init(bc_ctx_t *c);

/* Run one input line, which may hold several statements separated by ';' or
   newlines. Each expression statement appends its value and a newline to
   `out` (always terminated, truncated to out_n). An error stops the line and
   appends the offending line, a caret under the column and the message. */
bc_status_t bc_run(bc_ctx_t *c, const char *text, char *out, size_t out_n);

/* `v`, already at `scale` digits, as bc would print it in the context's
   obase. Returns the length, or 0 when it does not fit in n. */
size_t bc_format(const bc_ctx_t *c, double v, int scale, char *buf, size_t n);

/* `v` cut (not rounded) to `scale` decimal digits after the point, after
   first rounding to the 15 significant digits a double actually holds - so
   0.7+0.1 is .8 at scale 1 and not .7. */
double bc_truncate(double v, int scale);

#ifdef __cplusplus
}
#endif

#endif /* LS_BC_H */
