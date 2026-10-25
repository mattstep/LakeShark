/* See bc.h. An operator-precedence parser that evaluates as it parses:
   there is no tree to allocate, and a line is short enough that re-parsing
   costs nothing. Precedence follows bc (bc 7 and GNU bc agree on this part):

       ||  <  &&  <  relational  <  assignment  <  + -  <  * / %  <  ^
           <  unary - and !  <  ++ --

   so -2^2 is (-2)^2 = 4, a = 3 < 5 is (a = 3) < 5, and ^ and = are right
   associative. Operands and pending operators sit on stacks in the context,
   not on the C stack, so nesting depth costs no stack (see bc.h).

   Values carry a scale the way bc numbers do, and each operator gives its
   result bc's scale and truncates to it: that is what makes 7/2*2 equal 6 at
   scale 0 and 1/3 print .333333 at scale 6, and why 4/2 prints 2.000000. */

#include "bc.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

enum {
    T_END = 0, T_SEP, T_NUM, T_NAME, T_DOT, T_LP, T_RP, T_COMMA,
    T_PLUS, T_MINUS, T_MUL, T_DIV, T_MOD, T_POW,
    T_INC, T_DEC, T_NOT, T_AND, T_OR,
    T_LT, T_LE, T_GT, T_GE, T_EQ, T_NE,
    T_ASSIGN, T_ADDEQ, T_SUBEQ, T_MULEQ, T_DIVEQ, T_MODEQ, T_POWEQ,
};

/* Operator-stack entries that are not tokens. */
enum { O_LP = 100, O_FN, O_NEG, O_NOT, O_PREINC, O_PREDEC };

enum { P_OR = 1, P_AND, P_REL, P_ASSIGN, P_ADD, P_MUL, P_POW, P_UNARY };

/* Where a value came from, when it can be assigned to. */
enum { LV_NONE = -1, LV_SCALE = -2, LV_IBASE = -3, LV_OBASE = -4, LV_LAST = -5,
       LV_PENDING = -6 /* a name not yet in the table: c->last_name */ };

typedef bc_val_t val_t;

static const double P10[] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

/* ------------------------------------------------------------ numbers */

/* a * 10^k, exact-power steps so k within +-22 is one correctly rounded op. */
static double scale10(double a, int k)
{
    while (k > 22)  { a *= 1e22; k -= 22; }
    while (k < -22) { a /= 1e22; k += 22; }
    return k >= 0 ? a * P10[k] : a / P10[-k];
}

/* a > 0 and finite, as m * 10^(e - 14) with m a 15-digit integer: the digits
   a double can be trusted with. Done by hand rather than with printf because
   newlib's printf wants a kilobyte of stack the REPL task does not have. */
static void dec15(double a, int64_t *m, int *e)
{
    int e10 = (int)floor(log10(a));
    double r = floor(scale10(a, 14 - e10) + 0.5);
    if (r >= 1e15) { e10++; r = floor(scale10(a, 14 - e10) + 0.5); }
    else if (r < 1e14) { e10--; r = floor(scale10(a, 14 - e10) + 0.5); }
    if (r >= 1e15) { r = 1e14; e10++; }          /* 9.99..95 rounding up */
    *m = (int64_t)r;
    *e = e10;
}

double bc_truncate(double v, int scale)
{
    if (v == 0 || !isfinite(v)) return v;
    if (scale < 0) scale = 0;
    if (scale > BC_SCALE_MAX) scale = BC_SCALE_MAX;
    const double a = fabs(v);
    if (a >= 1e15) return scale == 0 ? trunc(v) : v;   /* no fraction left to cut */
    int64_t m;
    int e;
    dec15(a, &m, &e);
    const int keep = e + 1 + scale;              /* digits of m that survive */
    double r;
    if (keep <= 0) return 0.0;
    if (keep < 15) {
        int64_t cut = 1;
        for (int i = keep; i < 15; i++) cut *= 10;
        r = (double)(m / cut) / P10[scale];
    } else {
        r = scale10((double)m, e - 14);
    }
    return v < 0 ? -r : r;
}

/* ------------------------------------------------------------- output */

static void out_ch(bc_ctx_t *c, char ch)
{
    if (!c->out || c->out_n == 0) return;
    if (c->out_len + 1 < c->out_n) c->out[c->out_len++] = ch;
    c->out[c->out_len] = '\0';
}

static void out_str(bc_ctx_t *c, const char *s)
{
    while (*s) out_ch(c, *s++);
}

static void out_int(bc_ctx_t *c, long n)
{
    char d[24];
    int k = 0;
    if (n < 0) { out_ch(c, '-'); n = -n; }
    do { d[k++] = (char)('0' + n % 10); n /= 10; } while (n && k < (int)sizeof(d));
    while (k) out_ch(c, d[--k]);
}

/* ------------------------------------------------------------- format */

typedef struct { char *b; size_t n, len; bool full; } sbuf_t;

static void sb_ch(sbuf_t *s, char ch)
{
    if (s->len + 1 < s->n) s->b[s->len++] = ch;
    else s->full = true;
}

static const char DIG[] = "0123456789ABCDEF";

/* Integer part of a >= 0 in `base`, most significant first; nothing for 0.
   Digits go in backwards and are turned round in place: no scratch buffer,
   because this runs on the REPL task's stack. */
static void sb_int(sbuf_t *s, double ip, int base)
{
    if (ip < 1) return;
    if (base == 10 && ip >= 9.2e18) {
        /* Past int64: the 15 digits the double holds, then zeros. */
        int64_t m;
        int e;
        dec15(ip, &m, &e);
        char d[15];
        for (int i = 14; i >= 0; i--) { d[i] = (char)('0' + m % 10); m /= 10; }
        for (int i = 0; i <= e; i++) sb_ch(s, i < 15 ? d[i] : '0');
        return;
    }
    const size_t from = s->len;
    if (ip < 9.2e18) {
        uint64_t u = (uint64_t)ip;
        while (u && !s->full) { sb_ch(s, DIG[u % (unsigned)base]); u /= (unsigned)base; }
    } else {
        /* A power-of-two base divides a double exactly. */
        while (ip >= 1 && !s->full) {
            const double d = fmod(ip, base);
            sb_ch(s, DIG[(int)d]);
            ip = (ip - d) / base;
        }
    }
    for (size_t i = from, j = s->len; i + 1 < j; i++, j--) {
        const char t = s->b[i];
        s->b[i] = s->b[j - 1];
        s->b[j - 1] = t;
    }
}

static size_t format_base(double v, int scale, int base, char *buf, size_t n)
{
    if (!buf || n == 0) return 0;
    sbuf_t s = { buf, n, 0, false };
    if (scale < 0) scale = 0;
    if (scale > BC_SCALE_MAX) scale = BC_SCALE_MAX;
    if (!isfinite(v)) { buf[0] = '\0'; return 0; }
    v = bc_truncate(v, scale);
    const double a = fabs(v);
    if (v < 0) sb_ch(&s, '-');
    if (base == 10) {
        if (a >= 1e15) {
            sb_int(&s, trunc(a), 10);
        } else if (a > 0) {
            int64_t m;
            int e;
            dec15(a, &m, &e);
            char d[15];
            for (int i = 14; i >= 0; i--) { d[i] = (char)('0' + m % 10); m /= 10; }
            for (int i = 0; i <= e; i++) sb_ch(&s, d[i]);
            if (scale) {
                sb_ch(&s, '.');
                for (int i = e + 1; i < e + 1 + scale; i++) sb_ch(&s, i >= 0 && i < 15 ? d[i] : '0');
            }
        }
    } else {
        const double ip = floor(a);
        double fp = a - ip;
        sb_int(&s, ip, base);
        if (scale) {
            /* bc's rule: enough digits in this base to carry `scale` decimal
               digits, so 1/3 at scale 6 is .55554 in hex. */
            int k = 0;
            for (double room = 1; room < P10[scale]; room *= base) k++;
            sb_ch(&s, '.');
            for (int i = 0; i < k; i++) {
                fp *= base;
                const double d = floor(fp);
                sb_ch(&s, DIG[(int)d]);
                fp -= d;
            }
        }
    }
    if (s.full) { buf[0] = '\0'; return 0; }
    buf[s.len] = '\0';
    /* bc prints zero as 0 whatever its scale, and never as -0 or -.000 */
    bool any = false;
    for (size_t i = 0; i < s.len; i++)
        if (buf[i] != '-' && buf[i] != '.' && buf[i] != '0') { any = true; break; }
    if (!any) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    return s.len;
}

size_t bc_format(const bc_ctx_t *c, double v, int scale, char *buf, size_t n)
{
    return format_base(v, scale, c ? c->obase : 10, buf, n);
}

/* ------------------------------------------------------------- errors */

static void fail(bc_ctx_t *c, int pos, const char *msg)
{
    if (c->failed) return;
    c->failed = true;
    c->err_pos = pos;
    c->err_msg = msg;
}

static val_t vnum(double v, int scale)
{
    val_t r = { v, (int16_t)scale, LV_NONE, false };
    return r;
}

/* The result of an operator, at its scale; inf and nan never escape. */
static val_t result(bc_ctx_t *c, double v, int scale, int pos)
{
    if (scale > BC_SCALE_MAX) scale = BC_SCALE_MAX;
    if (!isfinite(v)) { fail(c, pos, "overflow"); return vnum(0, 0); }
    return vnum(bc_truncate(v, scale), scale);
}

/* -------------------------------------------------------------- lexer */

static bool is_digit(char ch)  { return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'); }
static bool is_lower(char ch)  { return ch >= 'a' && ch <= 'z'; }
static bool is_namech(char ch) { return is_lower(ch) || (ch >= '0' && ch <= '9') || ch == '_'; }

static void lex_number(bc_ctx_t *c)
{
    const char *s = c->src;
    int p = c->pos;
    const double base = c->ibase;
    double m = 0;
    int frac = 0;
    bool point = false;
    for (;; p++) {
        const char ch = s[p];
        if (ch == '.' && !point) { point = true; continue; }
        if (ch >= 'G' && ch <= 'Z') { fail(c, p, "digit not supported (0-9 and A-F only)"); return; }
        if (!is_digit(ch)) break;
        m = m * base + (ch <= '9' ? ch - '0' : ch - 'A' + 10);
        if (point) frac++;
    }
    double v = m;
    if (frac) v = base == 10 ? scale10(m, -frac) : m / pow(base, frac);
    c->tok.type = T_NUM;
    c->tok.num = v;
    c->tok.num_scale = frac > BC_SCALE_MAX ? BC_SCALE_MAX : frac;
    c->pos = p;
    if (!isfinite(v)) fail(c, c->tok.pos, "overflow");
}

static void lex(bc_ctx_t *c)
{
    const char *s = c->src;
    if (c->failed) { c->tok.type = T_END; return; }
    for (;;) {
        const char ch = s[c->pos];
        if (ch == ' ' || ch == '\t' || ch == '\r') { c->pos++; continue; }
        if (ch == '\\' && s[c->pos + 1] == '\n') { c->pos += 2; continue; }
        if (ch == '#') {                         /* GNU bc: to the end of the line */
            while (s[c->pos] && s[c->pos] != '\n') c->pos++;
            continue;
        }
        if (ch == '/' && s[c->pos + 1] == '*') {
            const char *end = strstr(s + c->pos + 2, "*/");
            if (!end) { fail(c, c->pos, "comment not closed"); c->tok.type = T_END; return; }
            c->pos = (int)(end - s) + 2;
            continue;
        }
        break;
    }
    c->tok.pos = c->pos;
    const char ch = s[c->pos], nx = ch ? s[c->pos + 1] : 0;
    if (!ch) { c->tok.type = T_END; return; }
    if (is_digit(ch) || (ch == '.' && is_digit(nx))) { lex_number(c); return; }
    if (is_lower(ch)) {
        int k = 0;
        while (is_namech(s[c->pos])) {
            if (k == BC_NAME_MAX - 1) { fail(c, c->tok.pos, "name too long"); c->tok.type = T_END; return; }
            c->tok.name[k++] = s[c->pos++];
        }
        c->tok.name[k] = '\0';
        c->tok.type = T_NAME;
        return;
    }
    static const struct { char a, b; int one, two; } OPS[] = {
        { '+', '+', T_PLUS, T_INC },   { '-', '-', T_MINUS, T_DEC },
        { '+', '=', T_PLUS, T_ADDEQ }, { '-', '=', T_MINUS, T_SUBEQ },
        { '*', '=', T_MUL, T_MULEQ },  { '/', '=', T_DIV, T_DIVEQ },
        { '%', '=', T_MOD, T_MODEQ },  { '^', '=', T_POW, T_POWEQ },
        { '<', '=', T_LT, T_LE },      { '>', '=', T_GT, T_GE },
        { '=', '=', T_ASSIGN, T_EQ },  { '!', '=', T_NOT, T_NE },
        { '&', '&', -1, T_AND },       { '|', '|', -1, T_OR },
    };
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
        if (ch != OPS[i].a) continue;
        if (nx == OPS[i].b) { c->tok.type = OPS[i].two; c->pos += 2; return; }
    }
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) {
        if (ch == OPS[i].a && OPS[i].one >= 0) { c->tok.type = OPS[i].one; c->pos++; return; }
    }
    c->pos++;
    switch (ch) {
    case ';': case '\n': c->tok.type = T_SEP; return;
    case '(': c->tok.type = T_LP; return;
    case ')': c->tok.type = T_RP; return;
    case ',': c->tok.type = T_COMMA; return;
    case '.': c->tok.type = T_DOT; return;
    default:
        fail(c, c->tok.pos, "syntax error");
        c->tok.type = T_END;
        return;
    }
}

/* ---------------------------------------------------------- variables */

static int find_var(const bc_ctx_t *c, const char *name)
{
    for (int i = 0; i < c->nvars; i++)
        if (!strcmp(c->vars[i].name, name)) return i;
    return -1;
}

static val_t read_lv(const bc_ctx_t *c, int lv)
{
    val_t r = vnum(0, 0);
    switch (lv) {
    case LV_SCALE: r.v = c->scale; break;
    case LV_IBASE: r.v = c->ibase; break;
    case LV_OBASE: r.v = c->obase; break;
    case LV_LAST:  r.v = c->last; r.scale = (int16_t)c->last_scale; break;
    case LV_PENDING: break;                      /* never assigned: 0 */
    default:
        if (lv >= 0) { r.v = c->vars[lv].v; r.scale = (int16_t)c->vars[lv].scale; }
        break;
    }
    r.lv = (int16_t)lv;
    return r;
}

/* An assignment is about to happen: a new name gets its slot now, while
   c->last_name still holds it. */
static int bind(bc_ctx_t *c, int lv, int pos)
{
    if (lv != LV_PENDING || c->noeval) return lv;
    const int found = find_var(c, c->last_name);
    if (found >= 0) return found;
    if (c->nvars >= BC_VARS) { fail(c, pos, "too many variables (32)"); return LV_NONE; }
    bc_var_t *v = &c->vars[c->nvars];
    memcpy(v->name, c->last_name, sizeof(v->name));
    v->v = 0;
    v->scale = 0;
    return c->nvars++;
}

static val_t store(bc_ctx_t *c, int lv, val_t x, int pos)
{
    val_t r = vnum(x.v, x.scale);
    r.assign = true;
    if (c->failed || c->noeval) return r;
    const double t = trunc(x.v);
    switch (lv) {
    case LV_SCALE:
        if (t < 0 || t > BC_SCALE_MAX) { fail(c, pos, "scale must be 0 to 20"); break; }
        c->scale = (int)t;
        r.v = t; r.scale = 0;
        break;
    case LV_IBASE:
        if (t != 10 && t != 16) { fail(c, pos, "ibase must be 10 or 16"); break; }
        c->ibase = (int)t;
        r.v = t; r.scale = 0;
        break;
    case LV_OBASE:
        if (t != 2 && t != 8 && t != 10 && t != 16) { fail(c, pos, "obase must be 2, 8, 10 or 16"); break; }
        c->obase = (int)t;
        r.v = t; r.scale = 0;
        break;
    case LV_LAST:
        c->last = x.v;
        c->last_scale = x.scale;
        break;
    default:
        if (lv >= 0) { c->vars[lv].v = x.v; c->vars[lv].scale = x.scale; }
        break;
    }
    return r;
}

/* ------------------------------------------------------------ parser */

static bool truth(val_t x) { return x.v != 0; }

static int prec_of(int t)
{
    switch (t) {
    case T_OR: return P_OR;
    case T_AND: return P_AND;
    case T_LT: case T_LE: case T_GT: case T_GE: case T_EQ: case T_NE: return P_REL;
    case T_ASSIGN: case T_ADDEQ: case T_SUBEQ: case T_MULEQ:
    case T_DIVEQ: case T_MODEQ: case T_POWEQ: return P_ASSIGN;
    case T_PLUS: case T_MINUS: return P_ADD;
    case T_MUL: case T_DIV: case T_MOD: return P_MUL;
    case T_POW: return P_POW;
    default: return 0;
    }
}

static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

static val_t binop(bc_ctx_t *c, int op, val_t a, val_t b, int pos)
{
    if (c->failed || c->noeval) return vnum(0, 0);
    const int S = c->scale;
    switch (op) {
    case T_PLUS:  return result(c, a.v + b.v, imax(a.scale, b.scale), pos);
    case T_MINUS: return result(c, a.v - b.v, imax(a.scale, b.scale), pos);
    case T_MUL:
        return result(c, a.v * b.v, imin(a.scale + b.scale, imax(S, imax(a.scale, b.scale))), pos);
    case T_DIV:
        if (b.v == 0) { fail(c, pos, "divide by zero"); return vnum(0, 0); }
        return result(c, a.v / b.v, S, pos);
    case T_MOD:
        /* fmod, not bc's a - (a/b)*b at the current scale: with the default
           scale of 6 bc's rule makes 7 % 3 come out as .000001. */
        if (b.v == 0) { fail(c, pos, "divide by zero"); return vnum(0, 0); }
        return result(c, fmod(a.v, b.v), imax(a.scale, b.scale), pos);
    case T_POW: {
        if (b.v != trunc(b.v)) { fail(c, pos, "exponent must be an integer"); return vnum(0, 0); }
        if (a.v == 0 && b.v < 0) { fail(c, pos, "divide by zero"); return vnum(0, 0); }
        const double want = (double)a.scale * fabs(b.v);
        const int sc = b.v < 0 ? S : (int)fmin(want, (double)imax(S, a.scale));
        return result(c, pow(a.v, b.v), sc, pos);
    }
    case T_LT: return vnum(a.v <  b.v, 0);
    case T_LE: return vnum(a.v <= b.v, 0);
    case T_GT: return vnum(a.v >  b.v, 0);
    case T_GE: return vnum(a.v >= b.v, 0);
    case T_EQ: return vnum(a.v == b.v, 0);
    case T_NE: return vnum(a.v != b.v, 0);
    default:   return vnum(0, 0);
    }
}

static int compound_op(int t)
{
    switch (t) {
    case T_ADDEQ: return T_PLUS;
    case T_SUBEQ: return T_MINUS;
    case T_MULEQ: return T_MUL;
    case T_DIVEQ: return T_DIV;
    case T_MODEQ: return T_MOD;
    case T_POWEQ: return T_POW;
    default:      return 0;
    }
}

typedef enum { F_SQRT, F_LENGTH, F_SCALE, F_ABS, F_S, F_C, F_A, F_L, F_E } fn_t;

static int find_fn(const char *name)
{
    static const char *const FN[] = { "sqrt", "length", "scale", "abs", "s", "c", "a", "l", "e" };
    for (unsigned i = 0; i < sizeof(FN) / sizeof(FN[0]); i++)
        if (!strcmp(FN[i], name)) return (int)i;
    return -1;
}

/* length(): significant digits as bc 7 counts them - leading zeros do not
   count, the trailing zeros of the scale do, and a zero is its scale. */
static int length_of(bc_ctx_t *c, val_t x)
{
    const size_t n = format_base(x.v, x.scale, 10, c->fmt, sizeof(c->fmt));
    int count = 0;
    bool lead = true;
    for (size_t i = 0; i < n; i++) {
        const char ch = c->fmt[i];
        if (ch < '0' || ch > '9') continue;
        if (lead && ch == '0') continue;
        lead = false;
        count++;
    }
    if (!count) count = x.scale > 0 ? x.scale : 1;
    return count;
}

static val_t call(bc_ctx_t *c, int fn, val_t x, int pos)
{
    if (c->failed || c->noeval) return vnum(0, 0);
    const int S = c->scale;
    switch ((fn_t)fn) {
    case F_SQRT:
        if (x.v < 0) { fail(c, pos, "square root of a negative number"); return vnum(0, 0); }
        return result(c, sqrt(x.v), imax(S, x.scale), pos);
    case F_LENGTH: return vnum(length_of(c, x), 0);
    case F_SCALE:  return vnum(x.scale, 0);
    case F_ABS:    return result(c, fabs(x.v), x.scale, pos);
    case F_S:      return result(c, sin(x.v), S, pos);
    case F_C:      return result(c, cos(x.v), S, pos);
    case F_A:      return result(c, atan(x.v), S, pos);
    case F_L:
        if (x.v <= 0) { fail(c, pos, "logarithm of zero or a negative number"); return vnum(0, 0); }
        return result(c, log(x.v), S, pos);
    case F_E:      return result(c, exp(x.v), S, pos);
    }
    return vnum(0, 0);
}

static bool is_word(const char *name, const char *const *list)
{
    for (; *list; list++) if (!strcmp(*list, name)) return true;
    return false;
}

static const char *const UNSUPPORTED[] = {
    "define", "if", "else", "while", "for", "do", "return", "break", "continue",
    "auto", "print", "read", "limits", "warranty", "void", "string", NULL };
static const char *const QUITS[] = { "quit", "exit", "halt", NULL };

/* ++x / x++ and the -- pair; `post` returns the old value. */
static val_t step(bc_ctx_t *c, val_t x, int dir, bool post, int pos)
{
    if (c->failed) return vnum(0, 0);
    if (x.lv == LV_NONE) { fail(c, pos, "++ and -- need a variable"); return vnum(0, 0); }
    const int lv = bind(c, x.lv, pos);
    if (c->failed) return vnum(0, 0);
    const val_t nv = binop(c, dir > 0 ? T_PLUS : T_MINUS, x, vnum(1, 0), pos);
    const val_t stored = store(c, lv, nv, pos);
    return post ? vnum(x.v, x.scale) : vnum(stored.v, stored.scale);   /* printed: bc 7 does */
}

/* ------------------------------------------------------------ stacks */

static void push_val(bc_ctx_t *c, val_t v, int pos)
{
    if (c->nvals >= BC_STACK) { fail(c, pos, "expression too long or too deeply nested"); return; }
    c->vals[c->nvals++] = v;
}

static val_t pop_val(bc_ctx_t *c)
{
    if (c->nvals <= 0) return vnum(0, 0);        /* unreachable: the state machine pairs them */
    return c->vals[--c->nvals];
}

static void push_op(bc_ctx_t *c, int type, int prec, int pos, int extra)
{
    if (c->nops >= BC_STACK) { fail(c, pos, "expression too long or too deeply nested"); return; }
    c->ops[c->nops++] = (bc_op_t){ type, prec, pos, extra };
}

/* Apply the operator on top of the stack to its operands. */
static void reduce(bc_ctx_t *c)
{
    const bc_op_t op = c->ops[--c->nops];
    val_t r = pop_val(c), l;
    switch (op.type) {
    case O_NEG:    push_val(c, vnum(-r.v, r.scale), op.pos); return;
    case O_NOT:    push_val(c, vnum(!truth(r), 0), op.pos); return;
    case O_PREINC: push_val(c, step(c, r, 1, false, op.pos), op.pos); return;
    case O_PREDEC: push_val(c, step(c, r, -1, false, op.pos), op.pos); return;
    default: break;
    }
    l = pop_val(c);
    switch (op.type) {
    case T_AND: case T_OR:
        c->noeval -= op.extra;
        push_val(c, vnum(op.type == T_AND ? (truth(l) && truth(r)) : (truth(l) || truth(r)), 0), op.pos);
        return;
    case T_ASSIGN: case T_ADDEQ: case T_SUBEQ: case T_MULEQ:
    case T_DIVEQ: case T_MODEQ: case T_POWEQ: {
        const int bop = compound_op(op.type);
        push_val(c, store(c, op.extra, bop ? binop(c, bop, l, r, op.pos) : r, op.pos), op.pos);
        return;
    }
    default:
        push_val(c, binop(c, op.type, l, r, op.pos), op.pos);
        return;
    }
}

/* An operand: a number, a name, `.`, or the opening of a group or a call
   (which leaves the parser still wanting an operand). Returns true when an
   operand was completed. */
static bool operand(bc_ctx_t *c)
{
    const int t = c->tok.type, pos = c->tok.pos;
    switch (t) {
    case T_MINUS: push_op(c, O_NEG, P_UNARY, pos, 0); lex(c); return false;
    case T_NOT:   push_op(c, O_NOT, P_UNARY, pos, 0); lex(c); return false;
    case T_INC:   push_op(c, O_PREINC, P_UNARY, pos, 0); lex(c); return false;
    case T_DEC:   push_op(c, O_PREDEC, P_UNARY, pos, 0); lex(c); return false;
    case T_LP:    push_op(c, O_LP, 0, pos, 0); lex(c); return false;
    case T_NUM:
        push_val(c, vnum(c->tok.num, c->tok.num_scale), pos);
        lex(c);
        return true;
    case T_DOT:
        push_val(c, read_lv(c, LV_LAST), pos);
        lex(c);
        return true;
    case T_NAME: {
        char name[BC_NAME_MAX];
        memcpy(name, c->tok.name, sizeof(name));
        if (is_word(name, UNSUPPORTED)) { fail(c, pos, "not supported: no functions, loops, if or print"); return false; }
        if (is_word(name, QUITS)) { fail(c, pos, "quit must be a statement of its own"); return false; }
        lex(c);
        if (c->tok.type == T_LP) {
            const int fn = find_fn(name);
            if (fn < 0) {
                memcpy(c->err_name, name, sizeof(c->err_name));
                fail(c, pos, "unknown function");
                return false;
            }
            push_op(c, O_FN, 0, pos, fn);
            lex(c);
            return false;
        }
        int lv;
        if (!strcmp(name, "scale"))      lv = LV_SCALE;
        else if (!strcmp(name, "ibase")) lv = LV_IBASE;
        else if (!strcmp(name, "obase")) lv = LV_OBASE;
        else if (!strcmp(name, "last"))  lv = LV_LAST;
        else {
            memcpy(c->last_name, name, sizeof(c->last_name));
            const int i = find_var(c, name);
            lv = i >= 0 ? i : LV_PENDING;
        }
        push_val(c, read_lv(c, lv), pos);
        return true;
    }
    case T_END: case T_SEP:
        fail(c, pos, "syntax error: the expression is not finished");
        return false;
    default:
        fail(c, pos, "syntax error");
        return false;
    }
}

/* The innermost open group or call, or -1. */
static int open_marker(const bc_ctx_t *c)
{
    for (int i = c->nops - 1; i >= 0; i--)
        if (c->ops[i].type == O_LP || c->ops[i].type == O_FN) return i;
    return -1;
}

/* One statement's expression, up to (not past) the ';', newline or end. */
static val_t expression(bc_ctx_t *c)
{
    c->nvals = c->nops = 0;
    bool want_operand = true;
    while (!c->failed) {
        if (want_operand) {
            want_operand = !operand(c);
            continue;
        }
        const int t = c->tok.type, pos = c->tok.pos;
        if (t == T_INC || t == T_DEC) {          /* postfix: tighter than anything */
            const val_t x = pop_val(c);
            push_val(c, step(c, x, t == T_INC ? 1 : -1, true, pos), pos);
            lex(c);
            continue;
        }
        if (t == T_RP) {
            if (open_marker(c) < 0) { fail(c, pos, "syntax error: ) without ("); break; }
            while (c->ops[c->nops - 1].type != O_LP && c->ops[c->nops - 1].type != O_FN && !c->failed) reduce(c);
            if (c->failed) break;
            const bc_op_t m = c->ops[--c->nops];
            val_t x = pop_val(c);
            if (m.type == O_FN) x = call(c, m.extra, x, m.pos);
            x.lv = LV_NONE;
            x.assign = false;                    /* (a = 5) prints, as in bc */
            push_val(c, x, pos);
            lex(c);
            continue;
        }
        if (t == T_COMMA) {
            const int m = open_marker(c);
            fail(c, pos, m >= 0 && c->ops[m].type == O_FN ? "this function takes one argument" : "syntax error");
            break;
        }
        const int p = prec_of(t);
        if (p) {
            const bool right = t == T_POW || p == P_ASSIGN;
            while (c->nops && c->ops[c->nops - 1].prec &&
                   (right ? c->ops[c->nops - 1].prec > p : c->ops[c->nops - 1].prec >= p) && !c->failed)
                reduce(c);
            if (c->failed) break;
            int extra = 0;
            const val_t top = c->vals[c->nvals - 1];
            if (p == P_ASSIGN) {
                if (top.lv == LV_NONE) { fail(c, pos, "only a variable can be assigned to"); break; }
                extra = bind(c, top.lv, pos);
            } else if (t == T_AND || t == T_OR) {
                /* Short-circuit, as GNU bc and bc 7 both do: the right side
                   is parsed but nothing in it is evaluated or stored. */
                extra = t == T_AND ? !truth(top) : truth(top);
                c->noeval += extra;
            }
            push_op(c, t, p, pos, extra);
            lex(c);
            want_operand = true;
            continue;
        }
        if (t == T_SEP || t == T_END) {
            if (open_marker(c) >= 0) { fail(c, pos, "syntax error: missing )"); break; }
            while (c->nops && !c->failed) reduce(c);
            break;
        }
        fail(c, pos, "syntax error");
    }
    if (c->failed) return vnum(0, 0);
    return pop_val(c);
}

/* ------------------------------------------------------------ driver */

void bc_init(bc_ctx_t *c)
{
    memset(c, 0, sizeof(*c));
    c->scale = BC_SCALE_DEFAULT;
    c->ibase = 10;
    c->obase = 10;
}

/* Is the current token a quit word standing alone as a statement? */
static bool at_quit(bc_ctx_t *c)
{
    if (c->tok.type != T_NAME) return false;
    if (!is_word(c->tok.name, QUITS) && strcmp(c->tok.name, "q")) return false;
    const bc_tok_t saved = c->tok;
    const int pos = c->pos;
    lex(c);
    const bool alone = !c->failed && (c->tok.type == T_SEP || c->tok.type == T_END);
    c->tok = saved;
    c->pos = pos;
    c->failed = false;                           /* the peek may not fail the line */
    return alone;
}

static void report(bc_ctx_t *c)
{
    const char *s = c->src;
    int start = c->err_pos;
    while (start > 0 && s[start - 1] != '\n') start--;
    int end = c->err_pos;
    while (s[end] && s[end] != '\n') end++;
    for (int i = start; i < end; i++) out_ch(c, s[i]);
    out_ch(c, '\n');
    for (int i = start; i < c->err_pos; i++) out_ch(c, s[i] == '\t' ? '\t' : ' ');
    out_str(c, "^\n");
    c->err_col = c->err_pos - start + 1;
    out_str(c, c->err_msg);
    if (c->err_name[0]) { out_str(c, " '"); out_str(c, c->err_name); out_ch(c, '\''); }
    out_str(c, " at column ");
    out_int(c, c->err_col);
    out_ch(c, '\n');
}

bc_status_t bc_run(bc_ctx_t *c, const char *text, char *out, size_t out_n)
{
    c->src = text ? text : "";
    c->pos = 0;
    c->noeval = 0;
    c->failed = false;
    c->err_col = 0;
    c->err_msg = NULL;
    c->err_name[0] = '\0';
    c->out = out;
    c->out_n = out_n;
    c->out_len = 0;
    if (out && out_n) out[0] = '\0';

    lex(c);
    while (!c->failed) {
        while (c->tok.type == T_SEP) lex(c);
        if (c->tok.type == T_END || c->failed) break;
        if (at_quit(c)) return BC_QUIT;
        c->noeval = 0;
        const val_t r = expression(c);
        if (c->failed) break;
        if (c->tok.type != T_SEP && c->tok.type != T_END) { fail(c, c->tok.pos, "syntax error"); break; }
        if (r.assign) continue;
        const size_t n = format_base(r.v, r.scale, c->obase, c->fmt, sizeof(c->fmt));
        if (!n) { fail(c, c->tok.pos > 0 ? c->tok.pos - 1 : 0, "too long to print in this obase"); break; }
        out_str(c, c->fmt);
        out_ch(c, '\n');
        c->last = r.v;
        c->last_scale = r.scale;
    }
    if (c->failed) { report(c); return BC_ERROR; }
    return BC_OK;
}
