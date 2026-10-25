/* The bc calculator engine (components/lsbc): what bc prints for the things
   people type at it, and where it says an error is. The expected strings
   were checked against bc 7.0.3 (macOS /usr/bin/bc) except where docs/BC.md
   records a deliberate difference: default scale 6, % as fmod.

   Plain cc builds it, because the bench CMake does not build on every host:
     cc -std=c11 -Wall -Wextra -fsanitize=address,undefined -Ibench/harness \
        -Ibench/shims -Icomponents/lsbc bench/tests/test_bc.c \
        components/lsbc/bc.c bench/harness/ls_test.c bench/harness/shim_impl.c \
        -lm -o test_bc && ./test_bc */

#include "ls_test.h"
#include "bc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bc_ctx_t s_bc;
static char s_out[1024];

static void fresh(void) { bc_init(&s_bc); }

/* Output of one line, in the running context. */
static const char *run(const char *line)
{
    bc_run(&s_bc, line, s_out, sizeof(s_out));
    return s_out;
}

/* One line in a fresh context. */
static const char *calc(const char *line)
{
    fresh();
    return run(line);
}

static double value(const char *line)
{
    return strtod(run(line), NULL);
}

/* The error a line gives: status, message line and column. */
static void check_error(const char *line, const char *msg, int col, int at)
{
    fresh();
    const bc_status_t st = bc_run(&s_bc, line, s_out, sizeof(s_out));
    LS_CHECK_MSG(st == BC_ERROR, "line %d: [%s] did not fail: [%s]", at, line, s_out);
    LS_CHECK_MSG(strstr(s_out, msg) != NULL, "line %d: [%s] gave [%s], want [%s]", at, line, s_out, msg);
    LS_CHECK_MSG(s_bc.err_col == col, "line %d: [%s] column %d, want %d", at, line, s_bc.err_col, col);
    char tail[48];
    snprintf(tail, sizeof(tail), " at column %d\n", col);
    LS_CHECK_MSG(strstr(s_out, tail) != NULL, "line %d: [%s] no '%s' in [%s]", at, line, tail, s_out);
}
#define ERR(line, msg, col) check_error(line, msg, col, __LINE__)

LS_CASE(precedence_and_associativity_are_bcs)
{
    LS_EQ_STR(calc("2^3^2"), "512\n");          /* right associative */
    LS_EQ_STR(calc("-2^2"), "4\n");             /* unary minus binds tighter, as in bc */
    LS_EQ_STR(calc("-(2^2)"), "-4\n");
    LS_EQ_STR(calc("0-2^2"), "-4\n");
    LS_EQ_STR(calc("2+3*4"), "14\n");
    LS_EQ_STR(calc("(2+3)*4"), "20\n");
    LS_EQ_STR(calc("10-4-3"), "3\n");           /* left associative */
    LS_EQ_STR(calc("2*3^2"), "18\n");
    LS_EQ_STR(calc("2^-2"), ".250000\n");
    LS_EQ_STR(calc("7 % 3"), "1\n");
    LS_EQ_STR(calc("-7 % 3"), "-1\n");
    LS_EQ_STR(calc("7.5 % 2"), "1.5\n");
    LS_EQ_STR(calc("- -3"), "3\n");
    LS_EQ_STR(calc("((((((((((((((((((((1+1))))))))))))))))))))"), "2\n");
}

LS_CASE(division_follows_scale)
{
    fresh();
    LS_EQ_STR(run("1/3"), ".333333\n");          /* default scale 6 */
    LS_EQ_STR(run("scale=0; 1/3"), "0\n");
    LS_EQ_STR(run("7/2"), "3\n");
    LS_EQ_STR(run("7/2*2"), "6\n");              /* the scale=0 integer idiom */
    LS_EQ_STR(run("-7/2"), "-3\n");              /* truncation toward zero */
    LS_EQ_STR(run(".5*.5"), ".2\n");             /* bc's multiply scale rule */
    LS_EQ_STR(run("scale=6; 1/3"), ".333333\n");
    LS_EQ_STR(run("4/2"), "2.000000\n");
    LS_EQ_STR(run("scale"), "6\n");
}

LS_CASE(results_are_truncated_not_rounded)
{
    fresh();
    LS_EQ_STR(run("scale=2; 2/3"), ".66\n");
    LS_EQ_STR(run("-2/3"), "-.66\n");
    LS_EQ_STR(run("scale=1; 0.7+0.1"), ".8\n");  /* not .7 from 0.7999... */
    LS_EQ_STR(run("0.1+0.2"), ".3\n");
    LS_EQ_STR(run("scale=0; 0.29*100"), "29.00\n");
    LS_NEAR(bc_truncate(2.0 / 3.0, 2), 0.66, 1e-12);
    LS_NEAR(bc_truncate(-2.0 / 3.0, 3), -0.666, 1e-12);
}

LS_CASE(formatting_matches_bc)
{
    LS_EQ_STR(calc(".5"), ".5\n");
    LS_EQ_STR(calc("0.5"), ".5\n");
    LS_EQ_STR(calc("-0.5"), "-.5\n");
    LS_EQ_STR(calc("7.10"), "7.10\n");          /* trailing zeros kept */
    LS_EQ_STR(calc("100.00/1"), "100.000000\n");
    LS_EQ_STR(calc("100.00+0"), "100.00\n");
    LS_EQ_STR(calc("0"), "0\n");
    LS_EQ_STR(calc("0.000"), "0\n");
    LS_EQ_STR(calc("-0"), "0\n");
    LS_EQ_STR(calc("1.5^2"), "2.25\n");
    LS_EQ_STR(calc("2^62"), "4611686018427387904\n");
    LS_EQ_STR(calc("123456789*1000"), "123456789000\n");
    LS_EQ_STR(calc("2^100"), "1267650600228230000000000000000\n");
}

LS_CASE(obase_formats_integers_and_fractions)
{
    fresh();
    LS_EQ_STR(run("obase=16; 10"), "A\n");
    LS_EQ_STR(run("255"), "FF\n");
    LS_EQ_STR(run("10.5"), "A.8\n");
    LS_EQ_STR(run("-255.75"), "-FF.C0\n");
    LS_EQ_STR(run("1/3"), ".55554\n");
    LS_EQ_STR(run("obase=2; 255"), "11111111\n");
    LS_EQ_STR(run("2.5"), "10.1000\n");
    LS_EQ_STR(run("0"), "0\n");
    LS_EQ_STR(run("obase=8; 64"), "100\n");
    LS_EQ_STR(run("1/3"), ".2525251\n");
    LS_EQ_STR(run("obase=10; 255"), "255\n");
    LS_EQ_STR(run("obase=2; 2^60"), "1000000000000000000000000000000000000000000000000000000000000\n");
}

LS_CASE(ibase_16_reads_hex)
{
    fresh();
    LS_EQ_STR(run("ibase=16; FF"), "255\n");
    LS_EQ_STR(run("A+1"), "11\n");
    LS_EQ_STR(run("1.8"), "1.5\n");
    LS_EQ_STR(run("10"), "16\n");
    LS_EQ_STR(run("ff"), "0\n");                /* lower case is a variable, as in bc */
    LS_EQ_STR(run("ibase=A; 10"), "10\n");      /* bc's way back to decimal */
    LS_EQ_STR(run("ibase"), "10\n");
    LS_EQ_STR(run("A"), "10\n");                /* a lone digit is its value in any base */
    LS_EQ_STR(run("ibase=16; obase=10; ibase=A; obase"), "10\n");
}

LS_CASE(variables_assignment_and_compound_ops)
{
    fresh();
    LS_EQ_STR(run("x=5"), "");                  /* an assignment prints nothing */
    LS_EQ_STR(run("x"), "5\n");
    LS_EQ_STR(run("y"), "0\n");                 /* unset is 0 */
    LS_EQ_STR(run("x+=2; x"), "7\n");
    LS_EQ_STR(run("x-=1; x"), "6\n");
    LS_EQ_STR(run("x*=3; x"), "18\n");
    LS_EQ_STR(run("x/=4; x"), "4.500000\n");
    LS_EQ_STR(run("x%=4; x"), ".500000\n");
    LS_EQ_STR(run("x=3; x^=3; x"), "27\n");
    LS_EQ_STR(run("a=b=4; a+b"), "8\n");        /* right associative */
    LS_EQ_STR(run("(c=5)"), "5\n");             /* parenthesised, it prints */
    LS_EQ_STR(run("d=3<5; d"), "1\n3\n");       /* (d=3) < 5, as in bc */
    LS_EQ_STR(run("long_name_2 = 1.25; long_name_2*2"), "2.50\n");
    LS_EQ_STR(run("s=2; s(0); s"), "0\n2\n");   /* functions and variables are apart */
    LS_EQ_STR(run("scale=2; scale"), "2\n");
    LS_EQ_STR(run("scale+=1; scale"), "3\n");
}

LS_CASE(increment_and_decrement)
{
    fresh();
    LS_EQ_STR(run("x=5"), "");
    LS_EQ_STR(run("x++"), "5\n");
    LS_EQ_STR(run("x"), "6\n");
    LS_EQ_STR(run("++x"), "7\n");
    LS_EQ_STR(run("x--"), "7\n");
    LS_EQ_STR(run("--x"), "5\n");
    LS_EQ_STR(run("n++; n"), "0\n1\n");         /* creates it */
    LS_EQ_STR(run("-x++"), "-5\n");
    LS_EQ_STR(run("x"), "6\n");
    LS_EQ_STR(run("x+++1"), "7\n");             /* (x++) + 1 */
    LS_EQ_STR(run("scale++; scale"), "6\n7\n");
}

LS_CASE(last_and_dot_are_the_previous_result)
{
    fresh();
    LS_EQ_STR(run("2+3"), "5\n");
    LS_EQ_STR(run("last*2"), "10\n");
    LS_EQ_STR(run(". + 1"), "11\n");
    LS_EQ_STR(run("x = 4"), "");                /* not printed, so not last */
    LS_EQ_STR(run("."), "11\n");
    LS_EQ_STR(run("1/4"), ".250000\n");
    LS_EQ_STR(run("scale(last)"), "6\n");
    LS_EQ_STR(run("last=2; last"), "2\n");
}

LS_CASE(functions_give_known_values)
{
    fresh();
    LS_EQ_STR(run("s(0)"), "0\n");
    LS_EQ_STR(run("c(0)"), "1.000000\n");
    LS_EQ_STR(run("e(1)"), "2.718281\n");
    LS_EQ_STR(run("sqrt(2)"), "1.414213\n");
    LS_EQ_STR(run("sqrt(4)"), "2.000000\n");
    LS_EQ_STR(run("a(1)*4"), "3.141592\n");
    LS_NEAR(value("l(e(1))"), 1.0, 2e-6);
    LS_EQ_STR(run("l(1)"), "0\n");
    LS_EQ_STR(run("abs(-2.5)"), "2.5\n");
    LS_EQ_STR(run("length(123.45)"), "5\n");
    LS_EQ_STR(run("length(.001)"), "1\n");
    LS_EQ_STR(run("length(0)"), "1\n");
    LS_EQ_STR(run("length(100)"), "3\n");
    LS_EQ_STR(run("scale(1.250)"), "3\n");
    LS_EQ_STR(run("scale(7)"), "0\n");
    LS_EQ_STR(run("scale=10; s(1)"), ".8414709848\n");
    LS_EQ_STR(run("scale=14; 4*a(1)"), "3.14159265358976\n");
}

LS_CASE(comparison_and_logic_give_0_or_1)
{
    LS_EQ_STR(calc("1<2"), "1\n");
    LS_EQ_STR(calc("2<=1"), "0\n");
    LS_EQ_STR(calc("3>2"), "1\n");
    LS_EQ_STR(calc("3>=3"), "1\n");
    LS_EQ_STR(calc("3==3.0"), "1\n");
    LS_EQ_STR(calc("3!=3"), "0\n");
    LS_EQ_STR(calc("1<2<3"), "1\n");
    LS_EQ_STR(calc("!0"), "1\n");
    LS_EQ_STR(calc("!5"), "0\n");
    LS_EQ_STR(calc("!0+5"), "6\n");            /* ! is unary, as in bc 7 */
    LS_EQ_STR(calc("1 && 0"), "0\n");
    LS_EQ_STR(calc("2 && 3"), "1\n");
    LS_EQ_STR(calc("0 || 0"), "0\n");
    LS_EQ_STR(calc("0 || 7"), "1\n");
    LS_EQ_STR(calc("1 || 0 && 0"), "1\n");     /* && binds tighter */
    /* short-circuit: the right side neither runs nor fails */
    fresh();
    LS_EQ_STR(run("0 && x++; x"), "0\n0\n");
    LS_EQ_STR(run("1 || 1/0"), "1\n");
    LS_EQ_STR(run("0 && (y=4); y"), "0\n0\n");
    LS_EQ_INT(s_bc.nvars, 0);                   /* nothing was created either */
    LS_EQ_STR(run("1 && x++; x"), "0\n1\n");   /* x++ is the old 0 */
}

LS_CASE(several_statements_on_a_line)
{
    fresh();
    LS_EQ_STR(run("a=2; b=3; a*b"), "6\n");
    LS_EQ_STR(run("1;2;3"), "1\n2\n3\n");
    LS_EQ_STR(run("1\n2\n"), "1\n2\n");
    LS_EQ_STR(run(";;  ;"), "");
    LS_EQ_STR(run(""), "");
    LS_EQ_STR(run("1 /* a comment */ + 2 # and another"), "3\n");
    LS_EQ_STR(run("1 + \\\n 2"), "3\n");
    /* what ran before an error stays done */
    LS_EQ_INT(bc_run(&s_bc, "z=9; 1/0; z=1", s_out, sizeof(s_out)), BC_ERROR);
    LS_EQ_STR(run("z"), "9\n");
}

LS_CASE(quit_ends_the_session)
{
    fresh();
    LS_EQ_INT(bc_run(&s_bc, "quit", s_out, sizeof(s_out)), BC_QUIT);
    LS_EQ_INT(bc_run(&s_bc, "q", s_out, sizeof(s_out)), BC_QUIT);
    LS_EQ_INT(bc_run(&s_bc, "  exit  ", s_out, sizeof(s_out)), BC_QUIT);
    LS_EQ_INT(bc_run(&s_bc, "1+1; quit", s_out, sizeof(s_out)), BC_QUIT);
    LS_EQ_STR(s_out, "2\n");
    LS_EQ_INT(bc_run(&s_bc, "q=5; q*2", s_out, sizeof(s_out)), BC_OK);   /* q is still a name */
    LS_EQ_STR(s_out, "10\n");
}

LS_CASE(errors_name_the_column_with_a_caret)
{
    fresh();
    LS_EQ_INT(bc_run(&s_bc, "1 + 2/0", s_out, sizeof(s_out)), BC_ERROR);
    LS_EQ_STR(s_out, "1 + 2/0\n     ^\ndivide by zero at column 6\n");

    ERR("1/0", "divide by zero", 2);
    ERR("5 % 0", "divide by zero", 3);
    ERR("0^-1", "divide by zero", 2);
    ERR("x=0; 3/x", "divide by zero", 7);
    ERR("x=1; x/=0", "divide by zero", 7);
    ERR("1 +", "syntax error", 4);
    ERR("2 3", "syntax error", 3);
    ERR("(1+2", "missing )", 5);
    ERR("1+2)", ") without (", 4);
    ERR("1 $ 2", "syntax error", 3);
    ERR("3 = 4", "only a variable can be assigned to", 3);
    ERR("a+b = 4", "only a variable can be assigned to", 5);
    ERR("5++", "++ and -- need a variable", 2);
    ERR("foo(2)", "unknown function 'foo'", 1);
    ERR("1 + j(1, 2)", "unknown function 'j'", 5);
    ERR("sqrt(1, 2)", "takes one argument", 7);
    ERR("sqrt()", "syntax error", 6);
    ERR("sqrt(-1)", "square root of a negative number", 1);
    ERR("l(0)", "logarithm of zero or a negative number", 1);
    ERR("l(-3)", "logarithm of zero or a negative number", 1);
    ERR("10^400", "overflow", 3);
    ERR("e(1000)", "overflow", 1);
    ERR("2^0.5", "exponent must be an integer", 2);
    ERR("scale=21", "scale must be 0 to 20", 6);
    ERR("scale=-1", "scale must be 0 to 20", 6);
    ERR("ibase=8", "ibase must be 10 or 16", 6);
    ERR("obase=3", "obase must be 2, 8, 10 or 16", 6);
    ERR("define f(x) { }", "not supported", 1);
    ERR("while (1) 2", "not supported", 1);
    ERR("1 + quit", "quit must be a statement of its own", 5);
    ERR("1G", "digit not supported", 2);
    ERR("abcdefghijklmnopq = 1", "name too long", 1);
    ERR("1 /* open", "comment not closed", 3);
    ERR("x[1]", "syntax error", 2);
    ERR("\"text\"", "syntax error", 1);
    ERR("1;\n2/0", "divide by zero", 2);         /* the column is within its own line */
    ERR("1 & 2", "syntax error", 3);

    /* a long chain overflows the stacks cleanly instead of the C stack */
    char deep[200];
    memset(deep, '(', 120);
    strcpy(deep + 120, "1");
    fresh();
    LS_EQ_INT(bc_run(&s_bc, deep, s_out, sizeof(s_out)), BC_ERROR);
    LS_CHECK(strstr(s_out, "too deeply nested") != NULL);
}

LS_CASE(the_variable_table_fills_with_an_error)
{
    fresh();
    char line[32];
    for (int i = 0; i < BC_VARS; i++) {
        snprintf(line, sizeof(line), "v%d = %d", i, i);
        LS_EQ_INT(bc_run(&s_bc, line, s_out, sizeof(s_out)), BC_OK);
    }
    LS_EQ_INT(bc_run(&s_bc, "v0 = 9; v31", s_out, sizeof(s_out)), BC_OK);   /* existing ones still assign */
    LS_EQ_STR(s_out, "31\n");
    LS_EQ_INT(bc_run(&s_bc, "w = 1", s_out, sizeof(s_out)), BC_ERROR);
    LS_CHECK(strstr(s_out, "too many variables") != NULL);
    LS_EQ_STR(run("w"), "0\n");                 /* reading one never creates it */
}

LS_CASE(contexts_are_independent_and_output_is_bounded)
{
    bc_ctx_t a, b;
    char oa[64], ob[64], tiny[4];
    bc_init(&a);
    bc_init(&b);
    bc_run(&a, "x = 2; scale = 1", oa, sizeof(oa));
    bc_run(&b, "x = 3", ob, sizeof(ob));
    bc_run(&a, "x/3", oa, sizeof(oa));
    bc_run(&b, "x/3", ob, sizeof(ob));
    LS_EQ_STR(oa, ".6\n");
    LS_EQ_STR(ob, "1.000000\n");
    bc_run(&a, "123456", tiny, sizeof(tiny));
    LS_EQ_STR(tiny, "123");
    LS_EQ_INT(bc_run(&a, "1", NULL, 0), BC_OK);
    char buf[8];
    LS_EQ_INT(bc_format(&a, 1234567.0, 0, buf, sizeof(buf)), 7);
    LS_EQ_STR(buf, "1234567");
    LS_EQ_INT(bc_format(&a, 12345678.0, 0, buf, sizeof(buf)), 0);
}
