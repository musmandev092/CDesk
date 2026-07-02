#include "services/calc.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DC_CALC_MAX_TOKENS 96

/* Token kinds. Unary +/- get their own operator codes ('u'/'p') distinct
 * from binary '-'/'+' so the shunting-yard precedence table can give them
 * the highest, right-associative precedence (as in every other calculator). */
typedef enum { TOK_NUM, TOK_OP, TOK_LPAREN, TOK_RPAREN } tok_kind;

typedef struct {
    tok_kind kind;
    double num;
    char op; /* '+' '-' '*' '/' '%' '^' 'u' (unary minus) 'p' (unary plus) */
} token;

/* True if `c` may legally appear in a math expression (used for the
 * cheap "does this even look like math" pre-filter in the launcher). */
static bool is_math_char(char c)
{
    return isdigit((unsigned char)c) || isspace((unsigned char)c) || c == '.' || c == '+' ||
           c == '-' || c == '*' || c == '/' || c == '%' || c == '^' || c == '(' || c == ')';
}

bool dc_calc_looks_like_math(const char *expr)
{
    if (!expr || !expr[0])
        return false;

    bool has_digit = false;
    bool has_op_or_paren = false;
    for (const char *p = expr; *p; p++) {
        if (!is_math_char(*p))
            return false;
        if (isdigit((unsigned char)*p))
            has_digit = true;
        else if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '%' || *p == '^' ||
                 *p == '(' || *p == ')')
            has_op_or_paren = true;
    }
    /* Require both a digit and an operator/paren so a bare number like "80"
     * (or an empty/whitespace-only string) doesn't hijack normal search. */
    return has_digit && has_op_or_paren;
}

/* Tokenize `s` into `out` (capacity `max`). Returns token count, or -1 on a
 * malformed token (garbage character, unterminated number). Unary +/- are
 * distinguished from binary here: a '+'/'-' is unary when it's the first
 * token or immediately follows another operator or '('. */
static int tokenize(const char *s, token *out, int max)
{
    int n = 0;
    bool prev_is_value = false; /* true after a number or ')' */

    for (const char *p = s; *p;) {
        if (isspace((unsigned char)*p)) {
            p++;
            continue;
        }
        if (n >= max)
            return -1;

        if (isdigit((unsigned char)*p) || *p == '.') {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end == p)
                return -1;
            out[n].kind = TOK_NUM;
            out[n].num = v;
            n++;
            p = end;
            prev_is_value = true;
            continue;
        }

        if (*p == '(') {
            out[n].kind = TOK_LPAREN;
            n++;
            p++;
            prev_is_value = false;
            continue;
        }
        if (*p == ')') {
            out[n].kind = TOK_RPAREN;
            n++;
            p++;
            prev_is_value = true;
            continue;
        }
        if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '%' || *p == '^') {
            char op = *p;
            if (!prev_is_value && (op == '+' || op == '-'))
                op = (op == '-') ? 'u' : 'p'; /* unary */
            else if (!prev_is_value)
                return -1; /* binary op with no left operand, e.g. "*5" */
            out[n].kind = TOK_OP;
            out[n].op = op;
            n++;
            p++;
            prev_is_value = false;
            continue;
        }
        return -1; /* stray character (shouldn't happen after the pre-filter) */
    }
    return n;
}

static int precedence(char op)
{
    switch (op) {
    case 'u':
    case 'p':
        return 4;
    case '^':
        return 3;
    case '*':
    case '/':
    case '%':
        return 2;
    case '+':
    case '-':
        return 1;
    default:
        return -1;
    }
}

static bool is_right_assoc(char op)
{
    return op == '^' || op == 'u' || op == 'p';
}

static bool is_unary(char op)
{
    return op == 'u' || op == 'p';
}

/* Pop `op` off the operator stack and apply it to the value stack (1 operand
 * for unary, 2 for binary). Returns false on stack underflow or div/mod by
 * zero (via *div_zero). */
static bool apply_op(double *vals, int *vsp, char op, bool *div_zero)
{
    if (is_unary(op)) {
        if (*vsp < 1)
            return false;
        double a = vals[*vsp - 1];
        vals[*vsp - 1] = (op == 'u') ? -a : a;
        return true;
    }

    if (*vsp < 2)
        return false;
    double b = vals[--(*vsp)];
    double a = vals[--(*vsp)];
    double r = 0.0;
    switch (op) {
    case '+':
        r = a + b;
        break;
    case '-':
        r = a - b;
        break;
    case '*':
        r = a * b;
        break;
    case '/':
        if (b == 0.0) {
            *div_zero = true;
            return false;
        }
        r = a / b;
        break;
    case '%':
        if (b == 0.0) {
            *div_zero = true;
            return false;
        }
        r = fmod(a, b);
        break;
    case '^':
        r = pow(a, b);
        break;
    default:
        return false;
    }
    vals[(*vsp)++] = r;
    return true;
}

dc_calc_status dc_calc_eval(const char *expr, double *out)
{
    if (!expr)
        return DC_CALC_ERR_EMPTY;

    token toks[DC_CALC_MAX_TOKENS];
    int n = tokenize(expr, toks, DC_CALC_MAX_TOKENS);
    if (n < 0)
        return DC_CALC_ERR_SYNTAX;
    if (n == 0)
        return DC_CALC_ERR_EMPTY;

    /* Classic shunting-yard: a value stack and an operator stack, applying
     * operators eagerly whenever precedence allows so no separate RPN
     * buffer is needed. */
    double vals[DC_CALC_MAX_TOKENS];
    int vsp = 0;
    char ops[DC_CALC_MAX_TOKENS]; /* also holds '(' as a sentinel byte */
    int osp = 0;
    bool div_zero = false;

    for (int i = 0; i < n; i++) {
        token *t = &toks[i];
        if (t->kind == TOK_NUM) {
            if (vsp >= DC_CALC_MAX_TOKENS)
                return DC_CALC_ERR_SYNTAX;
            vals[vsp++] = t->num;
        } else if (t->kind == TOK_LPAREN) {
            if (osp >= DC_CALC_MAX_TOKENS)
                return DC_CALC_ERR_SYNTAX;
            ops[osp++] = '(';
        } else if (t->kind == TOK_RPAREN) {
            bool found = false;
            while (osp > 0) {
                char top = ops[--osp];
                if (top == '(') {
                    found = true;
                    break;
                }
                if (!apply_op(vals, &vsp, top, &div_zero))
                    return div_zero ? DC_CALC_ERR_DIVZERO : DC_CALC_ERR_SYNTAX;
            }
            if (!found)
                return DC_CALC_ERR_SYNTAX; /* unmatched ')' */
        } else {                          /* TOK_OP */
            char o1 = t->op;
            while (osp > 0 && ops[osp - 1] != '(' &&
                   (precedence(ops[osp - 1]) > precedence(o1) ||
                    (precedence(ops[osp - 1]) == precedence(o1) && !is_right_assoc(o1)))) {
                char top = ops[--osp];
                if (!apply_op(vals, &vsp, top, &div_zero))
                    return div_zero ? DC_CALC_ERR_DIVZERO : DC_CALC_ERR_SYNTAX;
            }
            if (osp >= DC_CALC_MAX_TOKENS)
                return DC_CALC_ERR_SYNTAX;
            ops[osp++] = o1;
        }
    }

    while (osp > 0) {
        char top = ops[--osp];
        if (top == '(')
            return DC_CALC_ERR_SYNTAX; /* unmatched '(' */
        if (!apply_op(vals, &vsp, top, &div_zero))
            return div_zero ? DC_CALC_ERR_DIVZERO : DC_CALC_ERR_SYNTAX;
    }

    if (vsp != 1)
        return DC_CALC_ERR_SYNTAX; /* e.g. "2 3" with no operator between */

    if (out)
        *out = vals[0];
    return DC_CALC_OK;
}

void dc_calc_format(double value, char *out, size_t cap)
{
    if (!out || cap == 0)
        return;
    if (isnan(value) || isinf(value)) {
        snprintf(out, cap, "undefined");
        return;
    }
    /* %.10g: enough precision to round-trip a double's visually meaningful
     * digits without printing float noise or trailing zeros. */
    snprintf(out, cap, "%.10g", value);
}
