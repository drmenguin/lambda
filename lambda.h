/*
 * lambda - lambda calculus beta-reduction playground
 *
 * Copyright (C) 2026 Luke Collins
 * Website: https://lc.mt
 * Source: https://github.com/drmenguin/lambda
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LAMBDA_H
#define LAMBDA_H

#include <stddef.h>

#define LAMBDA_DEFAULT_MAX_STEPS 300
#define LAMBDA_MAX_IDENT 64

/* AST --------------------------------------------------------------------- */

typedef enum {
    TERM_VAR,
    TERM_APP,
    TERM_ABS
} TermType;

typedef struct Term Term;

struct Term {
    TermType type;
    union {
        struct {
            char *name;
        } var;

        struct {
            Term *left;
            Term *right;
        } app;

        struct {
            char *param;
            Term *body;
        } abs;
    } as;
};

/* Constructors / destruction --------------------------------------------- */

Term *term_var(const char *name);
Term *term_app(Term *left, Term *right);
Term *term_abs(const char *param, Term *body);
Term *term_clone(const Term *t);
void term_free(Term *t);

/* Parsing ----------------------------------------------------------------- */

/*
   Parses syntax such as:
       x
       x y z              meaning ((x y) z)
       f \x.x             meaning f (\x.x)
       xx                 meaning x x
       x1 x2              subscripted variables, printed as x₁ x₂
       KI, Ki             uppercase-starting multi-character names
       %                  previous-result pseudo-variable for front ends
       \x.x
       \x y. x y          meaning \x.\y.(x y)
       (\x.x) y

   Both backslash and the UTF-8 lambda character are accepted as lambdas.
   UTF-8 subscript digits are accepted in variable names and normalized to
   plain digits internally.
   On failure, returns NULL and writes an error message into errbuf.
*/
Term *parse_lambda(const char *source, char *errbuf, size_t errbuf_sz);

/* Beta reduction ---------------------------------------------------------- */

int term_free_in(const char *name, const Term *t);

/*
   Returns 1 if a and b are alpha-equivalent: the same term up to consistent
   renaming of bound variables. Free variable names must match exactly.
*/
int term_alpha_equivalent(const Term *a, const Term *b);

/*
   Performs one normal-order reduction step.
   The returned term is freshly allocated. If *changed is 0, the returned term
   is just a clone of the input term.
*/
Term *term_reduce_once(const Term *t, int *changed);

/*
   Reduces by normal order until no more redexes are found, or max_steps is hit.
   If max_steps <= 0, a sensible default is used.
*/
Term *term_reduce_normal_order(const Term *t, int max_steps,
                               int *steps, int *stopped_early);

/* Printing ---------------------------------------------------------------- */

/* Returns a malloc'd string. Caller must free it. */
char *term_to_string(const Term *t, int unicode_lambda);

#endif
