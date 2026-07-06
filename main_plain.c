/*
 * lambda-cli - plain command-line lambda calculus beta reducer
 *
 * Copyright (C) 2026 Luke Collins
 * Website: https://lc.mt
 * Source: https://github.com/drmenguin/lambda
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "lambda.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MAX_STEPS LAMBDA_DEFAULT_MAX_STEPS
#define LINE_CAP 4096
#define VERSION "0.1.13"
#define STEP_PREFIX " →ᵦ "

typedef struct {
    Term **items;
    size_t count;
    size_t cap;
} LineResults;

typedef struct {
    Term *current;
} StepSession;

static void *xmalloc_main(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }
    return p;
}

static int parse_positive_int_arg(const char *arg, const char *label,
                                  int *out)
{
    const char *s = arg;
    while (isspace((unsigned char)*s)) s++;

    if (*s == '\0') {
        fprintf(stderr, "expected a number after %s\n", label);
        return 0;
    }

    errno = 0;
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (s == end || errno == ERANGE || value < 1 || value > INT_MAX) {
        fprintf(stderr, "expected a positive number after %s\n", label);
        return 0;
    }

    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') {
        fprintf(stderr, "unexpected text after %s value\n", label);
        return 0;
    }

    *out = (int)value;
    return 1;
}

static int name_in_list(const char *name, const char **xs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(name, xs[i]) == 0) return 1;
    }
    return 0;
}

static int parse_line_ref_name(const char *name, size_t *line_no)
{
    if (name[0] != '%' || !isdigit((unsigned char)name[1])) return 0;

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(name + 1, &end, 10);
    if (*end != '\0' || errno == ERANGE || value == 0 ||
        value > (unsigned long)SIZE_MAX) {
        return 0;
    }

    *line_no = (size_t)value;
    return 1;
}

static Term *line_results_get(const LineResults *lines, size_t line_no)
{
    if (line_no == 0 || line_no > lines->count) return NULL;
    return lines->items[line_no - 1];
}

static size_t line_results_add(LineResults *lines, const Term *term)
{
    if (lines->count == lines->cap) {
        size_t new_cap = lines->cap ? lines->cap * 2 : 64;
        Term **new_items = realloc(lines->items, new_cap * sizeof lines->items[0]);
        if (!new_items) {
            fprintf(stderr, "out of memory\n");
            exit(EXIT_FAILURE);
        }
        lines->items = new_items;
        lines->cap = new_cap;
    }

    lines->items[lines->count++] = term ? term_clone(term) : NULL;
    return lines->count;
}

static void line_results_free(LineResults *lines)
{
    for (size_t i = 0; i < lines->count; i++) term_free(lines->items[i]);
    free(lines->items);
}

static Term *expand_history_refs_rec(const Term *t, const Term *last_output,
                                     const LineResults *lines,
                                     const char **bound, size_t nbound,
                                     char *err, size_t errsz)
{
    switch (t->type) {
        case TERM_VAR: {
            const char *name = t->as.var.name;
            size_t line_no = 0;

            if (name_in_list(name, bound, nbound)) return term_clone(t);

            if (strcmp(name, "%") == 0) {
                if (!last_output) {
                    snprintf(err, errsz, "no previous reduction for '%%'");
                    return NULL;
                }
                return term_clone(last_output);
            }

            if (parse_line_ref_name(name, &line_no)) {
                Term *line = line_results_get(lines, line_no);
                if (!line) {
                    snprintf(err, errsz, "no reduction result for line %zu", line_no);
                    return NULL;
                }
                return term_clone(line);
            }

            return term_clone(t);
        }

        case TERM_APP: {
            Term *l = expand_history_refs_rec(t->as.app.left, last_output,
                                              lines, bound, nbound,
                                              err, errsz);
            if (!l) return NULL;
            Term *r = expand_history_refs_rec(t->as.app.right, last_output,
                                              lines, bound, nbound,
                                              err, errsz);
            if (!r) {
                term_free(l);
                return NULL;
            }
            return term_app(l, r);
        }

        case TERM_ABS: {
            const char *new_bound[256];
            if (nbound >= 256) {
                snprintf(err, errsz, "too many nested binders");
                return NULL;
            }
            for (size_t i = 0; i < nbound; i++) new_bound[i] = bound[i];
            new_bound[nbound] = t->as.abs.param;

            Term *body = expand_history_refs_rec(t->as.abs.body, last_output,
                                                 lines, new_bound, nbound + 1,
                                                 err, errsz);
            if (!body) return NULL;
            return term_abs(t->as.abs.param, body);
        }
    }

    return NULL;
}

static Term *expand_history_refs(const Term *t, const Term *last_output,
                                 const LineResults *lines,
                                 char *err, size_t errsz)
{
    const char *bound[1];
    if (err && errsz) err[0] = '\0';
    return expand_history_refs_rec(t, last_output, lines, bound, 0, err, errsz);
}

static void replace_term(Term **slot, const Term *term)
{
    term_free(*slot);
    *slot = term ? term_clone(term) : NULL;
}

static void step_session_set(StepSession *session, const Term *term)
{
    replace_term(&session->current, term);
}

static void step_session_clear(StepSession *session)
{
    term_free(session->current);
    session->current = NULL;
}

static int parse_step_suffix(char *input, int *step_limit, char *err, size_t errsz)
{
    *step_limit = 0;

    char *end = input + strlen(input);
    while (end > input && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }

    char *digits = end;
    while (digits > input && isdigit((unsigned char)digits[-1])) digits--;

    if (digits == input || digits[-1] != '/') return 1;

    char *slash = digits - 1;
    const char *number = digits;

    if (*number == '\0') {
        *step_limit = 1;
    } else if (!parse_positive_int_arg(number, "/", step_limit)) {
        snprintf(err, errsz, "expected a positive number after /");
        return 0;
    }

    *slash = '\0';
    while (slash > input && isspace((unsigned char)slash[-1])) {
        *--slash = '\0';
    }

    return 1;
}

static void print_term(const char *prefix, const Term *term)
{
    char *s = term_to_string(term, 1);
    printf("%s%s\n", prefix, s);
    free(s);
}

static int term_can_reduce(const Term *term)
{
    int changed = 0;
    Term *next = term_reduce_once(term, &changed);
    term_free(next);
    return changed;
}

static void print_numbered_term(LineResults *lines, const char *marker,
                                const Term *term, const char *suffix)
{
    size_t line_no = line_results_add(lines, term);
    char *s = term_to_string(term, 1);
    printf("[%zu]>%s %s%s\n", line_no, marker, s, suffix ? suffix : "");
    free(s);
}

static int reduce_steps_from(Term **current, int step_limit)
{
    int steps_taken = 0;

    for (int step = 1; step <= step_limit; step++) {
        int changed = 0;
        Term *next = term_reduce_once(*current, &changed);
        if (!changed) {
            term_free(next);
            break;
        }

        term_free(*current);
        *current = next;
        steps_taken++;
    }

    return steps_taken;
}

static int reduce_and_print(char *source, int max_steps,
                            LineResults *lines, Term **last_output,
                            StepSession *session, Term **out_result)
{
    char err[256];
    int step_limit = 0;

    if (!parse_step_suffix(source, &step_limit, err, sizeof err)) {
        fprintf(stderr, "Parse error: %s\n", err);
        return 1;
    }

    if (source[0] == '\0') {
        if (!step_limit) {
            if (!session->current) return 0;
            step_limit = 1;
        }
        if (!session->current) {
            fprintf(stderr, "No gradual reduction to continue.\n");
            return 1;
        }

        Term *current = term_clone(session->current);
        int steps = reduce_steps_from(&current, step_limit);
        if (steps == 0) {
            printf("Already in normal form.\n");
        } else {
            print_numbered_term(lines, "ᵦ", current,
                                term_can_reduce(current) ? "  (...)" : "");
        }
        step_session_set(session, current);
        replace_term(last_output, current);
        if (out_result) *out_result = term_clone(current);
        term_free(current);
        return 0;
    }

    Term *current = parse_lambda(source, err, sizeof err);
    if (!current) {
        fprintf(stderr, "Parse error: %s\n", err);
        return 1;
    }

    Term *expanded = expand_history_refs(current, last_output ? *last_output : NULL,
                                         lines, err, sizeof err);
    term_free(current);
    if (!expanded) {
        fprintf(stderr, "Expansion error: %s\n", err);
        return 1;
    }

    current = expanded;

    if (step_limit) {
        int steps = reduce_steps_from(&current, step_limit);
        if (steps > 0) {
            print_numbered_term(lines, "ᵦ", current,
                                term_can_reduce(current) ? "  (...)" : "");
        }
        step_session_set(session, current);
        replace_term(last_output, current);
        if (out_result) *out_result = term_clone(current);
        term_free(current);
        return 0;
    }

    if (term_can_reduce(current)) {
        print_term("  ", current);
    }

    step_session_clear(session);

    int numbered_printed = 0;
    for (int step = 1; step <= max_steps; step++) {
        int changed = 0;
        Term *next = term_reduce_once(current, &changed);
        if (!changed) {
            term_free(next);
            break;
        }

        term_free(current);
        current = next;

        int more = term_can_reduce(current);
        if (more && step < max_steps) {
            print_term(STEP_PREFIX, current);
        } else {
            print_numbered_term(lines, "ᵦ", current,
                                more ? "  (...)" : "");
            numbered_printed = 1;
        }

        if (step == max_steps) {
            printf("Stopped after %d steps; term may not have a normal form.\n",
                   max_steps);
        }
    }

    if (!numbered_printed) {
        /* No beta step was printed, so the original term is already the result. */
        print_numbered_term(lines, "", current, "");
    }
    replace_term(last_output, current);
    if (out_result) *out_result = term_clone(current);
    term_free(current);
    return 0;
}

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out, "Usage: %s [OPTION...] [EXPR...]\n", prog);
    fprintf(out, "\n");
    fprintf(out, "Reduce lambda calculus expressions from arguments or standard input.\n");
    fprintf(out, "Reduction uses normal-order beta reduction; final results are numbered as [N]>.\n");
    fprintf(out, "Numbered [N]> output lines can be referenced later as %%N.\n");
    fprintf(out, "A trailing / or /N performs one or N reduction steps; /N or empty input continues.\n");
    fprintf(out, "\n");
    fprintf(out, "Options:\n");
    fprintf(out, "  -e, --eval EXPR    reduce EXPR\n");
    fprintf(out, "      --max-steps N  stop reducing after N steps (default %d)\n",
            DEFAULT_MAX_STEPS);
    fprintf(out, "  -h, --help         show this help\n");
    fprintf(out, "  -V, --version      show version information\n");
}

int main(int argc, char **argv)
{
    int status = 0;
    int max_steps = DEFAULT_MAX_STEPS;
    LineResults lines = {0};
    Term *last_output = NULL;
    StepSession session = {0};

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            const char *arg = argv[i];

            if (strcmp(arg, "--") == 0) {
                for (i++; i < argc; i++) {
                    char *copy = xmalloc_main(strlen(argv[i]) + 1);
                    strcpy(copy, argv[i]);
                    Term *result = NULL;
                    int rc = reduce_and_print(copy, max_steps, &lines,
                                              &last_output, &session, &result);
                    status |= rc;
                    term_free(result);
                    free(copy);
                }
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return status;
            }

            if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
                print_usage(stdout, argv[0]);
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return 0;
            }

            if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
                printf("lambda-cli %s\n", VERSION);
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return 0;
            }

            if (strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "%s: expected an expression\n", arg);
                    term_free(last_output);
                    step_session_clear(&session);
                    line_results_free(&lines);
                    return 2;
                }
                char *copy = xmalloc_main(strlen(argv[i]) + 1);
                strcpy(copy, argv[i]);
                Term *result = NULL;
                int rc = reduce_and_print(copy, max_steps, &lines,
                                          &last_output, &session, &result);
                status |= rc;
                term_free(result);
                free(copy);
                continue;
            }

            if (strcmp(arg, "--max-steps") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "%s: expected a step count\n", arg);
                    term_free(last_output);
                    step_session_clear(&session);
                    line_results_free(&lines);
                    return 2;
                }
                if (!parse_positive_int_arg(argv[i], "--max-steps",
                                            &max_steps)) {
                    term_free(last_output);
                    step_session_clear(&session);
                    line_results_free(&lines);
                    return 2;
                }
                continue;
            }

            if (strncmp(arg, "--max-steps=", 12) == 0) {
                if (!parse_positive_int_arg(arg + 12, "--max-steps",
                                            &max_steps)) {
                    term_free(last_output);
                    step_session_clear(&session);
                    line_results_free(&lines);
                    return 2;
                }
                continue;
            }

            if (arg[0] == '-') {
                fprintf(stderr, "Unknown option: %s\n", arg);
                fprintf(stderr, "Try '%s --help'.\n", argv[0]);
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return 2;
            }

            char *copy = xmalloc_main(strlen(arg) + 1);
            strcpy(copy, arg);
            Term *result = NULL;
            int rc = reduce_and_print(copy, max_steps, &lines,
                                      &last_output, &session, &result);
            status |= rc;
            term_free(result);
            free(copy);
        }
        term_free(last_output);
        step_session_clear(&session);
        line_results_free(&lines);
        return status;
    }

    char line[LINE_CAP];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = '\0';
        Term *result = NULL;
        int rc = reduce_and_print(line, max_steps, &lines,
                                  &last_output, &session, &result);
        status |= rc;
        term_free(result);
    }

    term_free(last_output);
    step_session_clear(&session);
    line_results_free(&lines);
    return status;
}
