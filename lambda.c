/*
 * lambda - lambda calculus beta-reduction playground
 *
 * Copyright (C) 2026 Luke Collins
 * Website: https://lc.mt
 * Source: https://github.com/drmenguin/lambda
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "lambda.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Small utilities --------------------------------------------------------- */

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }
    memcpy(p, s, n);
    return p;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }
    return p;
}

static int streq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/* Constructors / destruction --------------------------------------------- */

Term *term_var(const char *name)
{
    Term *t = xmalloc(sizeof *t);
    t->type = TERM_VAR;
    t->as.var.name = xstrdup(name);
    return t;
}

Term *term_app(Term *left, Term *right)
{
    Term *t = xmalloc(sizeof *t);
    t->type = TERM_APP;
    t->as.app.left = left;
    t->as.app.right = right;
    return t;
}

Term *term_abs(const char *param, Term *body)
{
    Term *t = xmalloc(sizeof *t);
    t->type = TERM_ABS;
    t->as.abs.param = xstrdup(param);
    t->as.abs.body = body;
    return t;
}

Term *term_clone(const Term *t)
{
    if (!t) return NULL;

    switch (t->type) {
        case TERM_VAR:
            return term_var(t->as.var.name);

        case TERM_APP:
            return term_app(term_clone(t->as.app.left), term_clone(t->as.app.right));

        case TERM_ABS:
            return term_abs(t->as.abs.param, term_clone(t->as.abs.body));
    }

    return NULL;
}

void term_free(Term *t)
{
    if (!t) return;

    switch (t->type) {
        case TERM_VAR:
            free(t->as.var.name);
            break;

        case TERM_APP:
            term_free(t->as.app.left);
            term_free(t->as.app.right);
            break;

        case TERM_ABS:
            free(t->as.abs.param);
            term_free(t->as.abs.body);
            break;
    }

    free(t);
}

/* Lexer ------------------------------------------------------------------- */

typedef enum {
    TOK_LAMBDA,
    TOK_DOT,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_IDENT,
    TOK_END,
    TOK_ERROR
} TokenType;

typedef struct {
    TokenType type;
    char lexeme[LAMBDA_MAX_IDENT];
    size_t pos;
} Token;

typedef struct {
    const char *src;
    size_t pos;
    Token tok;
    char *errbuf;
    size_t errbuf_sz;
    int failed;
} Parser;

static void parser_error(Parser *p, const char *fmt, ...)
{
    if (p->failed) return;

    p->failed = 1;

    if (!p->errbuf || p->errbuf_sz == 0) return;

    va_list args;
    va_start(args, fmt);
    vsnprintf(p->errbuf, p->errbuf_sz, fmt, args);
    va_end(args);
}

static int is_ident_char(unsigned char c)
{
    return isalnum(c) || c == '_' || c == '\'';
}

static int read_subscript_digit(const char *src, size_t pos, char *digit)
{
    unsigned char a = (unsigned char)src[pos];
    if (a != 0xE2) return 0;

    unsigned char b = (unsigned char)src[pos + 1];
    if (b != 0x82) return 0;

    unsigned char c = (unsigned char)src[pos + 2];
    if (c >= 0x80 && c <= 0x89) {
        *digit = (char)('0' + (c - 0x80));
        return 1;
    }

    return 0;
}

static int append_lexeme_char(Token *t, size_t *len, char c)
{
    if (*len + 1 >= LAMBDA_MAX_IDENT) {
        t->type = TOK_ERROR;
        snprintf(t->lexeme, sizeof t->lexeme, "identifier too long");
        return 0;
    }

    t->lexeme[(*len)++] = c;
    t->lexeme[*len] = '\0';
    return 1;
}

static Token next_token_raw(const char *src, size_t *pos)
{
    Token t;
    t.type = TOK_ERROR;
    t.lexeme[0] = '\0';

    while (isspace((unsigned char)src[*pos])) {
        (*pos)++;
    }

    t.pos = *pos;

    unsigned char c = (unsigned char)src[*pos];

    if (c == '\0') {
        t.type = TOK_END;
        return t;
    }

    /* Accept ASCII backslash and UTF-8 lambda: CE BB. */
    if (c == '\\') {
        t.type = TOK_LAMBDA;
        strcpy(t.lexeme, "\\");
        (*pos)++;
        return t;
    }

    if (c == 0xCE && (unsigned char)src[*pos + 1] == 0xBB) {
        t.type = TOK_LAMBDA;
        strcpy(t.lexeme, "λ");
        (*pos) += 2;
        return t;
    }

    if (c == '.') {
        t.type = TOK_DOT;
        strcpy(t.lexeme, ".");
        (*pos)++;
        return t;
    }

    if (c == '(') {
        t.type = TOK_LPAREN;
        strcpy(t.lexeme, "(");
        (*pos)++;
        return t;
    }

    if (c == ')') {
        t.type = TOK_RPAREN;
        strcpy(t.lexeme, ")");
        (*pos)++;
        return t;
    }

    if (c == '%') {
        t.type = TOK_IDENT;
        strcpy(t.lexeme, "%");
        (*pos)++;
        return t;
    }

    if (islower(c)) {
        size_t len = 0;
        if (!append_lexeme_char(&t, &len, (char)c)) return t;
        (*pos)++;

        for (;;) {
            char digit;
            unsigned char next = (unsigned char)src[*pos];

            if (isdigit(next)) {
                if (!append_lexeme_char(&t, &len, (char)next)) return t;
                (*pos)++;
                continue;
            }

            if (read_subscript_digit(src, *pos, &digit)) {
                if (!append_lexeme_char(&t, &len, digit)) return t;
                (*pos) += 3;
                continue;
            }

            if (next == '\'') {
                if (!append_lexeme_char(&t, &len, '\'')) return t;
                (*pos)++;
                continue;
            }

            break;
        }

        t.type = TOK_IDENT;
        return t;
    }

    if (isupper(c) || c == '_' || isdigit(c)) {
        size_t len = 0;

        for (;;) {
            char digit;
            unsigned char next = (unsigned char)src[*pos];

            if (is_ident_char(next)) {
                if (!append_lexeme_char(&t, &len, src[*pos])) return t;
                (*pos)++;
                continue;
            }

            if (read_subscript_digit(src, *pos, &digit)) {
                if (!append_lexeme_char(&t, &len, digit)) return t;
                (*pos) += 3;
                continue;
            }

            break;
        }

        t.type = TOK_IDENT;
        return t;
    }

    t.type = TOK_ERROR;
    snprintf(t.lexeme, sizeof t.lexeme, "unexpected character '%c'", c);
    (*pos)++;
    return t;
}

static void advance(Parser *p)
{
    p->tok = next_token_raw(p->src, &p->pos);
    if (p->tok.type == TOK_ERROR) {
        parser_error(p, "lexical error at position %zu: %s", p->tok.pos, p->tok.lexeme);
    }
}

/* Parser ------------------------------------------------------------------ */

static Term *parse_term(Parser *p);
static Term *parse_abstraction(Parser *p);

static int starts_atom(TokenType ty)
{
    return ty == TOK_IDENT || ty == TOK_LPAREN || ty == TOK_LAMBDA;
}

static Term *parse_atom(Parser *p)
{
    if (p->failed) return NULL;

    if (p->tok.type == TOK_IDENT) {
        char name[LAMBDA_MAX_IDENT];
        snprintf(name, sizeof name, "%s", p->tok.lexeme);
        advance(p);
        return term_var(name);
    }

    if (p->tok.type == TOK_LPAREN) {
        advance(p);
        Term *inside = parse_term(p);

        if (p->failed) {
            term_free(inside);
            return NULL;
        }

        if (p->tok.type != TOK_RPAREN) {
            parser_error(p, "expected ')' at position %zu", p->tok.pos);
            term_free(inside);
            return NULL;
        }

        advance(p);
        return inside;
    }

    if (p->tok.type == TOK_LAMBDA) {
        return parse_abstraction(p);
    }

    parser_error(p, "expected a variable, abstraction, or '(' at position %zu", p->tok.pos);
    return NULL;
}

static Term *parse_application(Parser *p)
{
    Term *left = parse_atom(p);

    if (p->failed) {
        term_free(left);
        return NULL;
    }

    while (!p->failed && starts_atom(p->tok.type)) {
        Term *right = parse_atom(p);
        if (p->failed) {
            term_free(left);
            term_free(right);
            return NULL;
        }
        left = term_app(left, right);
    }

    return left;
}

static Term *parse_abstraction(Parser *p)
{
    /* Current token is lambda. Parse \x y z. body as \x.\y.\z.body. */
    advance(p);

    char params[64][LAMBDA_MAX_IDENT];
    size_t nparams = 0;

    if (p->tok.type != TOK_IDENT) {
        parser_error(p, "expected a variable after lambda at position %zu", p->tok.pos);
        return NULL;
    }

    while (p->tok.type == TOK_IDENT) {
        if (nparams >= 64) {
            parser_error(p, "too many parameters in abstraction");
            return NULL;
        }
        snprintf(params[nparams++], LAMBDA_MAX_IDENT, "%s", p->tok.lexeme);
        advance(p);
    }

    if (p->tok.type != TOK_DOT) {
        parser_error(p, "expected '.' after lambda parameter at position %zu", p->tok.pos);
        return NULL;
    }

    advance(p);

    Term *body = parse_term(p);
    if (p->failed) {
        term_free(body);
        return NULL;
    }

    for (size_t i = nparams; i > 0; i--) {
        body = term_abs(params[i - 1], body);
    }

    return body;
}

static Term *parse_term(Parser *p)
{
    if (p->tok.type == TOK_LAMBDA) {
        return parse_abstraction(p);
    }

    return parse_application(p);
}

Term *parse_lambda(const char *source, char *errbuf, size_t errbuf_sz)
{
    if (errbuf && errbuf_sz > 0) errbuf[0] = '\0';

    Parser p;
    p.src = source;
    p.pos = 0;
    p.errbuf = errbuf;
    p.errbuf_sz = errbuf_sz;
    p.failed = 0;

    advance(&p);

    if (p.tok.type == TOK_END) {
        parser_error(&p, "empty input");
        return NULL;
    }

    Term *t = parse_term(&p);

    if (!p.failed && p.tok.type != TOK_END) {
        parser_error(&p, "unexpected input at position %zu", p.tok.pos);
    }

    if (p.failed) {
        term_free(t);
        return NULL;
    }

    return t;
}

/* Free variables and fresh names ----------------------------------------- */

int term_free_in(const char *name, const Term *t)
{
    if (!t) return 0;

    switch (t->type) {
        case TERM_VAR:
            return streq(name, t->as.var.name);

        case TERM_APP:
            return term_free_in(name, t->as.app.left) ||
                   term_free_in(name, t->as.app.right);

        case TERM_ABS:
            if (streq(name, t->as.abs.param)) return 0;
            return term_free_in(name, t->as.abs.body);
    }

    return 0;
}

static int term_name_occurs(const char *name, const Term *t)
{
    if (!t) return 0;

    switch (t->type) {
        case TERM_VAR:
            return streq(name, t->as.var.name);

        case TERM_APP:
            return term_name_occurs(name, t->as.app.left) ||
                   term_name_occurs(name, t->as.app.right);

        case TERM_ABS:
            return streq(name, t->as.abs.param) ||
                   term_name_occurs(name, t->as.abs.body);
    }

    return 0;
}

static char *fresh_var_avoiding(const Term *a, const Term *b, const char *extra)
{
    char candidate[64];
    const char bases[] = "xyzabcdefghijklmnopqrstuvw";

    for (size_t primes = 0; primes < sizeof candidate - 2; primes++) {
        for (size_t i = 0; bases[i]; i++) {
            candidate[0] = bases[i];
            for (size_t j = 0; j < primes; j++) candidate[j + 1] = '\'';
            candidate[primes + 1] = '\0';

            if (extra && streq(candidate, extra)) continue;
            if (term_name_occurs(candidate, a)) continue;
            if (term_name_occurs(candidate, b)) continue;
            return xstrdup(candidate);
        }
    }

    fprintf(stderr, "out of fresh variable names\n");
    exit(EXIT_FAILURE);
}

/* Alpha-equivalence ------------------------------------------------------- */

typedef struct {
    const char *left;
    const char *right;
} AlphaBinding;

static int alpha_lookup_left(const AlphaBinding *env, size_t nenv,
                             const char *left, const char **right)
{
    for (size_t i = nenv; i > 0; i--) {
        if (streq(env[i - 1].left, left)) {
            *right = env[i - 1].right;
            return 1;
        }
    }

    return 0;
}

static int alpha_lookup_right(const AlphaBinding *env, size_t nenv,
                              const char *right, const char **left)
{
    for (size_t i = nenv; i > 0; i--) {
        if (streq(env[i - 1].right, right)) {
            *left = env[i - 1].left;
            return 1;
        }
    }

    return 0;
}

static int alpha_equiv_rec(const Term *a, const Term *b,
                           AlphaBinding *env, size_t nenv, size_t cap)
{
    if (a->type != b->type) return 0;

    switch (a->type) {
        case TERM_VAR: {
            const char *mapped_right;
            const char *mapped_left;
            int left_bound = alpha_lookup_left(env, nenv, a->as.var.name, &mapped_right);
            int right_bound = alpha_lookup_right(env, nenv, b->as.var.name, &mapped_left);

            if (left_bound || right_bound) {
                return left_bound && right_bound &&
                       streq(mapped_right, b->as.var.name) &&
                       streq(mapped_left, a->as.var.name);
            }

            return streq(a->as.var.name, b->as.var.name);
        }

        case TERM_APP:
            return alpha_equiv_rec(a->as.app.left, b->as.app.left, env, nenv, cap) &&
                   alpha_equiv_rec(a->as.app.right, b->as.app.right, env, nenv, cap);

        case TERM_ABS:
            if (nenv >= cap) {
                fprintf(stderr, "alpha-equivalence nesting limit exceeded\n");
                exit(EXIT_FAILURE);
            }
            env[nenv].left = a->as.abs.param;
            env[nenv].right = b->as.abs.param;
            return alpha_equiv_rec(a->as.abs.body, b->as.abs.body, env, nenv + 1, cap);
    }

    return 0;
}

int term_alpha_equivalent(const Term *a, const Term *b)
{
    AlphaBinding env[256];
    if (!a || !b) return 0;
    return alpha_equiv_rec(a, b, env, 0, sizeof env / sizeof env[0]);
}

/* Capture-avoiding substitution ------------------------------------------ */

/* Rename occurrences of old_name which are bound by an enclosing abstraction.
   Stop under an inner abstraction which binds old_name again. */
static Term *rename_bound_var(const Term *t, const char *old_name, const char *new_name)
{
    switch (t->type) {
        case TERM_VAR:
            if (streq(t->as.var.name, old_name)) return term_var(new_name);
            return term_var(t->as.var.name);

        case TERM_APP:
            return term_app(rename_bound_var(t->as.app.left, old_name, new_name),
                            rename_bound_var(t->as.app.right, old_name, new_name));

        case TERM_ABS:
            if (streq(t->as.abs.param, old_name)) {
                return term_clone(t);
            }
            return term_abs(t->as.abs.param,
                            rename_bound_var(t->as.abs.body, old_name, new_name));
    }

    return NULL;
}

static Term *substitute(const Term *body, const char *x, const Term *replacement)
{
    switch (body->type) {
        case TERM_VAR:
            if (streq(body->as.var.name, x)) return term_clone(replacement);
            return term_var(body->as.var.name);

        case TERM_APP:
            return term_app(substitute(body->as.app.left, x, replacement),
                            substitute(body->as.app.right, x, replacement));

        case TERM_ABS: {
            const char *param = body->as.abs.param;

            if (streq(param, x)) {
                /* The binder shadows x. */
                return term_clone(body);
            }

            if (!term_free_in(param, replacement)) {
                return term_abs(param, substitute(body->as.abs.body, x, replacement));
            }

            /* Capture would occur. Alpha-rename first. */
            char *fresh = fresh_var_avoiding(body->as.abs.body, replacement, x);
            Term *renamed_body = rename_bound_var(body->as.abs.body, param, fresh);
            Term *new_body = substitute(renamed_body, x, replacement);
            Term *result = term_abs(fresh, new_body);

            free(fresh);
            term_free(renamed_body);
            return result;
        }
    }

    return NULL;
}

/* Reduction --------------------------------------------------------------- */

Term *term_reduce_once(const Term *t, int *changed)
{
    if (changed) *changed = 0;

    switch (t->type) {
        case TERM_VAR:
            return term_var(t->as.var.name);

        case TERM_APP: {
            const Term *left = t->as.app.left;
            const Term *right = t->as.app.right;

            if (left->type == TERM_ABS) {
                if (changed) *changed = 1;
                return substitute(left->as.abs.body, left->as.abs.param, right);
            }

            int left_changed = 0;
            Term *new_left = term_reduce_once(left, &left_changed);
            if (left_changed) {
                if (changed) *changed = 1;
                return term_app(new_left, term_clone(right));
            }
            term_free(new_left);

            int right_changed = 0;
            Term *new_right = term_reduce_once(right, &right_changed);
            if (right_changed) {
                if (changed) *changed = 1;
                return term_app(term_clone(left), new_right);
            }
            term_free(new_right);

            return term_app(term_clone(left), term_clone(right));
        }

        case TERM_ABS: {
            int body_changed = 0;
            Term *new_body = term_reduce_once(t->as.abs.body, &body_changed);
            if (body_changed) {
                if (changed) *changed = 1;
                return term_abs(t->as.abs.param, new_body);
            }
            term_free(new_body);
            return term_abs(t->as.abs.param, term_clone(t->as.abs.body));
        }
    }

    return NULL;
}

Term *term_reduce_normal_order(const Term *t, int max_steps,
                               int *steps, int *stopped_early)
{
    if (max_steps <= 0) max_steps = LAMBDA_DEFAULT_MAX_STEPS;
    if (steps) *steps = 0;
    if (stopped_early) *stopped_early = 0;

    Term *current = term_clone(t);

    for (int i = 0; i < max_steps; i++) {
        int changed = 0;
        Term *next = term_reduce_once(current, &changed);

        if (!changed) {
            term_free(next);
            return current;
        }

        term_free(current);
        current = next;
        if (steps) (*steps)++;
    }

    if (stopped_early) *stopped_early = 1;
    return current;
}

/* Pretty-printer ---------------------------------------------------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *sb)
{
    sb->cap = 128;
    sb->len = 0;
    sb->buf = xmalloc(sb->cap);
    sb->buf[0] = '\0';
}

static void sb_reserve(StrBuf *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) return;

    while (sb->cap < need) sb->cap *= 2;
    char *newbuf = realloc(sb->buf, sb->cap);
    if (!newbuf) {
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }
    sb->buf = newbuf;
}

static void sb_append(StrBuf *sb, const char *s)
{
    size_t n = strlen(s);
    sb_reserve(sb, n);
    memcpy(sb->buf + sb->len, s, n + 1);
    sb->len += n;
}

static void sb_append_char(StrBuf *sb, char c)
{
    sb_reserve(sb, 1);
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

static const char *subscript_digit_str(char c)
{
    switch (c) {
        case '0': return "₀";
        case '1': return "₁";
        case '2': return "₂";
        case '3': return "₃";
        case '4': return "₄";
        case '5': return "₅";
        case '6': return "₆";
        case '7': return "₇";
        case '8': return "₈";
        case '9': return "₉";
    }

    return NULL;
}

static void sb_append_ident(StrBuf *sb, const char *name, int unicode)
{
    int can_subscript = 0;

    for (size_t i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];

        if (unicode && isdigit(c) && can_subscript) {
            sb_append(sb, subscript_digit_str((char)c));
            continue;
        }

        sb_append_char(sb, (char)c);
        if (isalpha(c) || c == '\'') can_subscript = 1;
    }
}

static int precedence(const Term *t)
{
    switch (t->type) {
        case TERM_ABS: return 1;
        case TERM_APP: return 2;
        case TERM_VAR: return 3;
    }
    return 0;
}

static void print_term_rec(StrBuf *sb, const Term *t, int parent_prec, int unicode_lambda)
{
    int my_prec = precedence(t);
    int need_parens = my_prec < parent_prec;

    if (need_parens) sb_append_char(sb, '(');

    switch (t->type) {
        case TERM_VAR:
            sb_append_ident(sb, t->as.var.name, unicode_lambda);
            break;

        case TERM_APP:
            print_term_rec(sb, t->as.app.left, 2, unicode_lambda);
            sb_append_char(sb, ' ');
            print_term_rec(sb, t->as.app.right, 3, unicode_lambda);
            break;

        case TERM_ABS: {
            sb_append(sb, unicode_lambda ? "λ" : "\\");

            const Term *cur = t;
            while (cur->type == TERM_ABS) {
                sb_append_ident(sb, cur->as.abs.param, unicode_lambda);
                cur = cur->as.abs.body;
                if (cur->type == TERM_ABS) sb_append_char(sb, ' ');
            }

            sb_append_char(sb, '.');
            print_term_rec(sb, cur, 1, unicode_lambda);
            break;
        }
    }

    if (need_parens) sb_append_char(sb, ')');
}

char *term_to_string(const Term *t, int unicode_lambda)
{
    StrBuf sb;
    sb_init(&sb);
    print_term_rec(&sb, t, 0, unicode_lambda);
    return sb.buf;
}
