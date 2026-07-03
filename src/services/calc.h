/* calc.h — tiny standalone math-expression evaluator for the launcher's
 * "type an expression, see the answer" row (docs/POLISH.md P4 item 1; DMS
 * doesn't actually ship this in the current DankLauncherV2 QML — see
 * launcher.c's comment — so this mirrors the common spotlight-calculator
 * pattern instead of a specific DMS source).
 *
 * Deliberately dependency-free (no cJSON, no dankc headers) so it can be
 * compiled and unit-tested completely standalone — see tests/test_calc.c.
 */
#ifndef DC_SERVICES_CALC_H
#define DC_SERVICES_CALC_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DC_CALC_OK = 0,
    DC_CALC_ERR_EMPTY,   /* nothing to evaluate */
    DC_CALC_ERR_SYNTAX,  /* bad token, unbalanced parens, missing operand, ... */
    DC_CALC_ERR_DIVZERO, /* division or modulo by zero */
} dc_calc_status;

/* Quick pre-filter: does `expr` look like an arithmetic expression at all
 * (only digits/operators/parens/whitespace, and at least one binary
 * operator or a parenthesis)? Callers should gate dc_calc_eval() on this so
 * a plain app-name query like "firefox" never gets treated as math. */
bool dc_calc_looks_like_math(const char *expr);

/* Evaluate `expr` (+ - * / % ^ and parentheses, decimal literals, unary
 * +/-). On DC_CALC_OK, *out holds the result. */
dc_calc_status dc_calc_eval(const char *expr, double *out);

/* Format a result the way a calculator display would: no trailing zeros,
 * reasonable precision. `out` must be at least 48 bytes. */
void dc_calc_format(double value, char *out, size_t cap);

#endif /* DC_SERVICES_CALC_H */
