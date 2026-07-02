/* Standalone unit test for src/services/calc.c — the launcher's math
 * expression evaluator (docs/POLISH.md P4 item 1). Deliberately built
 * without touching any other dankc module (see the `test-calc` Makefile
 * target: it compiles only calc.c + this file, no protocol/EGL/etc.). */
#include "services/calc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

static void expect_ok(const char *expr, double want)
{
    g_checks++;
    double got = 0.0;
    dc_calc_status st = dc_calc_eval(expr, &got);
    if (st != DC_CALC_OK) {
        printf("FAIL: %-24s expected OK == %g, got status %d\n", expr, want, (int)st);
        g_failures++;
        return;
    }
    if (fabs(got - want) > 1e-9 * (fabs(want) > 1.0 ? fabs(want) : 1.0)) {
        printf("FAIL: %-24s expected %g, got %g\n", expr, want, got);
        g_failures++;
        return;
    }
    printf("ok:   %-24s = %g\n", expr, got);
}

static void expect_status(const char *expr, dc_calc_status want)
{
    g_checks++;
    double got = 0.0;
    dc_calc_status st = dc_calc_eval(expr, &got);
    if (st != want) {
        printf("FAIL: %-24s expected status %d, got %d (val=%g)\n", expr, (int)want, (int)st, got);
        g_failures++;
        return;
    }
    printf("ok:   %-24s -> status %d\n", expr, (int)st);
}

static void expect_looks_like_math(const char *expr, bool want)
{
    g_checks++;
    bool got = dc_calc_looks_like_math(expr);
    if (got != want) {
        printf("FAIL: looks_like_math(%s) expected %d, got %d\n", expr, want, got);
        g_failures++;
        return;
    }
    printf("ok:   looks_like_math(%s) = %d\n", expr, got);
}

int main(void)
{
    /* Basic precedence: * before + */
    expect_ok("2+2*3", 8.0);
    expect_ok("2*3+2", 8.0);
    /* Parentheses override precedence */
    expect_ok("(2+2)*3", 12.0);
    expect_ok("2*(3+4)*2", 28.0);
    /* Division */
    expect_ok("10/4", 2.5);
    expect_ok("7/2+1", 4.5);
    /* Modulo */
    expect_ok("15%4", 3.0);
    expect_ok("80%15", 5.0);
    /* Exponent, right-associative */
    expect_ok("2^3", 8.0);
    expect_ok("2^3^2", 512.0); /* 2^(3^2), not (2^3)^2 */
    /* Unary minus/plus */
    expect_ok("-5+3", -2.0);
    expect_ok("3*-2", -6.0);
    expect_ok("-(2+3)", -5.0);
    expect_ok("+5-2", 3.0);
    expect_ok("--5", 5.0);
    /* Decimals */
    expect_ok("1.5+2.25", 3.75);
    expect_ok(".5*2", 1.0);
    /* Whitespace tolerance */
    expect_ok(" 2 + 2 * 3 ", 8.0);
    /* Nested parens */
    expect_ok("((1+2)*(3+4))", 21.0);

    /* Errors: malformed input */
    expect_status("", DC_CALC_ERR_EMPTY);
    expect_status("   ", DC_CALC_ERR_EMPTY);
    expect_status("2+", DC_CALC_ERR_SYNTAX);
    expect_status("*5", DC_CALC_ERR_SYNTAX);
    expect_status("2 3", DC_CALC_ERR_SYNTAX);       /* no operator between operands */
    expect_status("(2+3", DC_CALC_ERR_SYNTAX);      /* unmatched '(' */
    expect_status("2+3)", DC_CALC_ERR_SYNTAX);      /* unmatched ')' */
    expect_status("2..3+1", DC_CALC_ERR_SYNTAX);    /* malformed number */
    expect_status("2+*3", DC_CALC_ERR_SYNTAX);      /* two binary ops in a row */
    expect_status("hello", DC_CALC_ERR_SYNTAX);     /* not tested via looks_like_math gate */
    expect_status("1/0", DC_CALC_ERR_DIVZERO);
    expect_status("5%0", DC_CALC_ERR_DIVZERO);
    expect_status("1/(2-2)", DC_CALC_ERR_DIVZERO);

    /* Pre-filter used by the launcher to decide whether to even try
     * evaluating a query as math. */
    expect_looks_like_math("2+2*3", true);
    expect_looks_like_math("(1+2)", true);
    expect_looks_like_math("firefox", false);
    expect_looks_like_math("80", false);   /* bare number: let normal search win */
    expect_looks_like_math("", false);
    expect_looks_like_math("2 + 2", true);
    expect_looks_like_math("code --help", false); /* letters -> rejected by the char whitelist */

    char buf[48];
    dc_calc_format(8.0, buf, sizeof(buf));
    g_checks++;
    if (strcmp(buf, "8") != 0) {
        printf("FAIL: format(8.0) = '%s', want '8'\n", buf);
        g_failures++;
    } else {
        printf("ok:   format(8.0) = '%s'\n", buf);
    }

    dc_calc_format(2.5, buf, sizeof(buf));
    g_checks++;
    if (strcmp(buf, "2.5") != 0) {
        printf("FAIL: format(2.5) = '%s', want '2.5'\n", buf);
        g_failures++;
    } else {
        printf("ok:   format(2.5) = '%s'\n", buf);
    }

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
