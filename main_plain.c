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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STEPS 300
#define LINE_CAP 4096
#define VERSION "0.1.9"
#define STEP_PREFIX "→ᵦ "

static int reduce_and_print(const char *source)
{
    char err[256];
    Term *current = parse_lambda(source, err, sizeof err);
    if (!current) {
        fprintf(stderr, "Parse error: %s\n", err);
        return 1;
    }

    char *s = term_to_string(current, 1);
    printf("  %s\n", s);
    free(s);

    for (int step = 1; step <= MAX_STEPS; step++) {
        int changed = 0;
        Term *next = term_reduce_once(current, &changed);
        if (!changed) {
            term_free(next);
            break;
        }

        term_free(current);
        current = next;

        s = term_to_string(current, 1);
        printf("%s%s\n", STEP_PREFIX, s);
        free(s);

        if (step == MAX_STEPS) {
            printf("Stopped after %d steps; term may not have a normal form.\n", MAX_STEPS);
        }
    }

    term_free(current);
    return 0;
}

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out, "Usage: %s [OPTION...] [EXPR...]\n", prog);
    fprintf(out, "\n");
    fprintf(out, "Reduce lambda calculus expressions from arguments or standard input.\n");
    fprintf(out, "Reduction uses normal-order beta reduction; steps are shown with →ᵦ.\n");
    fprintf(out, "\n");
    fprintf(out, "Options:\n");
    fprintf(out, "  -e, --eval EXPR  reduce EXPR\n");
    fprintf(out, "  -h, --help       show this help\n");
    fprintf(out, "  -V, --version    show version information\n");
}

int main(int argc, char **argv)
{
    int status = 0;

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            const char *arg = argv[i];

            if (strcmp(arg, "--") == 0) {
                for (i++; i < argc; i++) {
                    status |= reduce_and_print(argv[i]);
                }
                return status;
            }

            if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
                print_usage(stdout, argv[0]);
                return 0;
            }

            if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
                printf("lambda-cli %s\n", VERSION);
                return 0;
            }

            if (strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "%s: expected an expression\n", arg);
                    return 2;
                }
                status |= reduce_and_print(argv[i]);
                continue;
            }

            if (arg[0] == '-') {
                fprintf(stderr, "Unknown option: %s\n", arg);
                fprintf(stderr, "Try '%s --help'.\n", argv[0]);
                return 2;
            }

            status |= reduce_and_print(arg);
        }
        return status;
    }

    char line[LINE_CAP];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') continue;
        status |= reduce_and_print(line);
    }

    return status;
}
