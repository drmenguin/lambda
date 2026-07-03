/*
 * lambda - interactive lambda calculus beta-reduction playground
 *
 * Copyright (C) 2026 Luke Collins
 * Website: https://lc.mt
 * Source: https://github.com/drmenguin/lambda
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _XOPEN_SOURCE_EXTENDED 1

#include "lambda.h"

#include <ctype.h>
#include <locale.h>
#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <wchar.h>
#include <wctype.h>

#define INPUT_CAP 4096
#define HISTORY_CAP 128
#define MAX_DEFS 128
#define MAX_STEPS 300
#define VERSION "0.1.2"

static int curses_started = 0;

typedef struct {
    char *name;
    Term *term;
} Definition;

typedef struct {
    Definition items[MAX_DEFS];
    size_t count;
} Env;

typedef struct {
    char *items[HISTORY_CAP];
    size_t count;
} History;

static char *xstrdup_main(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) {
        if (curses_started) endwin();
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }
    memcpy(p, s, n);
    return p;
}

static void history_free(History *history)
{
    for (size_t i = 0; i < history->count; i++) {
        free(history->items[i]);
    }
}

static void history_add(History *history, const char *line)
{
    if (!*line) return;

    if (history->count > 0 &&
        strcmp(history->items[history->count - 1], line) == 0) {
        return;
    }

    if (history->count == HISTORY_CAP) {
        free(history->items[0]);
        memmove(history->items, history->items + 1,
                (HISTORY_CAP - 1) * sizeof history->items[0]);
        history->count--;
    }

    history->items[history->count++] = xstrdup_main(line);
}

static void env_free(Env *env)
{
    for (size_t i = 0; i < env->count; i++) {
        free(env->items[i].name);
        term_free(env->items[i].term);
    }
}

static ssize_t env_find(const Env *env, const char *name)
{
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->items[i].name, name) == 0) return (ssize_t)i;
    }
    return -1;
}

static int env_set(Env *env, const char *name, Term *term)
{
    ssize_t i = env_find(env, name);
    if (i >= 0) {
        term_free(env->items[i].term);
        env->items[i].term = term;
        return 1;
    }

    if (env->count >= MAX_DEFS) {
        term_free(term);
        return 0;
    }

    env->items[env->count].name = xstrdup_main(name);
    env->items[env->count].term = term;
    env->count++;
    return 1;
}

static int env_remove(Env *env, const char *name)
{
    ssize_t idx = env_find(env, name);
    if (idx < 0) return 0;

    size_t i = (size_t)idx;
    free(env->items[i].name);
    term_free(env->items[i].term);

    if (i + 1 < env->count) {
        memmove(env->items + i, env->items + i + 1,
                (env->count - i - 1) * sizeof env->items[0]);
    }

    env->count--;
    return 1;
}

static int name_in_list(const char *name, const char **xs, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(name, xs[i]) == 0) return 1;
    }
    return 0;
}

static Term *expand_defs_rec(const Term *t, const Env *env,
                             const char **bound, size_t nbound,
                             const char **expanding, size_t nexpanding,
                             char *err, size_t errsz)
{
    switch (t->type) {
        case TERM_VAR: {
            const char *name = t->as.var.name;

            if (name_in_list(name, bound, nbound)) {
                return term_clone(t);
            }

            ssize_t idx = env_find(env, name);
            if (idx < 0) {
                return term_clone(t);
            }

            if (name_in_list(name, expanding, nexpanding)) {
                snprintf(err, errsz, "cyclic definition involving '%s'", name);
                return NULL;
            }

            const char *new_expanding[MAX_DEFS];
            for (size_t i = 0; i < nexpanding; i++) new_expanding[i] = expanding[i];
            new_expanding[nexpanding] = name;

            return expand_defs_rec(env->items[idx].term, env, bound, nbound,
                                   new_expanding, nexpanding + 1, err, errsz);
        }

        case TERM_APP: {
            Term *l = expand_defs_rec(t->as.app.left, env, bound, nbound,
                                      expanding, nexpanding, err, errsz);
            if (!l) return NULL;
            Term *r = expand_defs_rec(t->as.app.right, env, bound, nbound,
                                      expanding, nexpanding, err, errsz);
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

            Term *body = expand_defs_rec(t->as.abs.body, env, new_bound, nbound + 1,
                                         expanding, nexpanding, err, errsz);
            if (!body) return NULL;
            return term_abs(t->as.abs.param, body);
        }
    }

    return NULL;
}

static Term *expand_defs(const Term *t, const Env *env, char *err, size_t errsz)
{
    const char *bound[1];
    const char *expanding[1];
    if (err && errsz) err[0] = '\0';
    return expand_defs_rec(t, env, bound, 0, expanding, 0, err, errsz);
}

static char *trim_in_place(char *s)
{
    while (isspace((unsigned char)*s)) s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';

    return s;
}

static int valid_def_name(const char *s)
{
    if (!*s) return 0;

    if (islower((unsigned char)*s)) {
        s++;
        while (isdigit((unsigned char)*s) || *s == '\'') s++;
        return *s == '\0';
    }

    if (!(isupper((unsigned char)*s) || *s == '_')) return 0;

    for (const char *p = s + 1; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '\'')) return 0;
    }

    return 1;
}

static const wchar_t *subscript_digit_wstr(char c)
{
    switch (c) {
        case '0': return L"₀";
        case '1': return L"₁";
        case '2': return L"₂";
        case '3': return L"₃";
        case '4': return L"₄";
        case '5': return L"₅";
        case '6': return L"₆";
        case '7': return L"₇";
        case '8': return L"₈";
        case '9': return L"₉";
    }

    return NULL;
}

static void render_input_line(int y, const char *prompt, const char *buf, size_t cursor)
{
    move(y, 0);
    clrtoeol();
    addstr(prompt);

    int cursor_x = 0;
    int can_subscript = 0;
    for (size_t i = 0; buf[i]; i++) {
        unsigned char c = (unsigned char)buf[i];

        if (i == cursor) {
            int cy;
            getyx(stdscr, cy, cursor_x);
            (void)cy;
        }

        if (buf[i] == '\\') {
            addwstr(L"λ");
            can_subscript = 0;
        } else if (isdigit(c) && can_subscript) {
            addwstr(subscript_digit_wstr(buf[i]));
        } else {
            addch(c);
            can_subscript = isalpha(c) || c == '\'';
        }
    }

    if (cursor == strlen(buf)) {
        int cy;
        getyx(stdscr, cy, cursor_x);
        (void)cy;
    }

    move(y, cursor_x);
    refresh();
}

static void set_input_line(char *buf, size_t cap, size_t *len, size_t *cursor,
                           const char *src)
{
    snprintf(buf, cap, "%s", src);
    *len = strlen(buf);
    *cursor = *len;
}

static void insert_input_char(char *buf, size_t cap, size_t *len, size_t *cursor, char c)
{
    if (*len + 1 >= cap) {
        beep();
        return;
    }

    memmove(buf + *cursor + 1, buf + *cursor, *len - *cursor + 1);
    buf[*cursor] = c;
    (*cursor)++;
    (*len)++;
}

static int read_lambda_line(const char *prompt, char *buf, size_t cap,
                            const History *history)
{
    int y, x;
    getyx(stdscr, y, x);
    (void)x;

    size_t len = 0;
    size_t cursor = 0;
    size_t history_pos = history->count;
    char draft[INPUT_CAP];

    buf[0] = '\0';
    draft[0] = '\0';
    render_input_line(y, prompt, buf, cursor);

    for (;;) {
        wint_t wc;
        int rc = get_wch(&wc);

        if (rc == ERR) continue;

        if (wc == L'\n' || wc == L'\r' || wc == KEY_ENTER) {
            addch('\n');
            buf[len] = '\0';
            return 1;
        }

        if (wc == 27) { /* Escape */
            return 0;
        }

        if (wc == KEY_BACKSPACE || wc == 127 || wc == 8) {
            if (cursor > 0) {
                memmove(buf + cursor - 1, buf + cursor, len - cursor + 1);
                cursor--;
                len--;
                render_input_line(y, prompt, buf, cursor);
            }
            continue;
        }

        if (rc == KEY_CODE_YES) {
            switch (wc) {
                case KEY_LEFT:
                    if (cursor > 0) cursor--;
                    break;

                case KEY_RIGHT:
                    if (cursor < len) cursor++;
                    break;

                case KEY_HOME:
                    cursor = 0;
                    break;

                case KEY_END:
                    cursor = len;
                    break;

                case KEY_DC:
                    if (cursor < len) {
                        memmove(buf + cursor, buf + cursor + 1, len - cursor);
                        len--;
                    }
                    break;

                case KEY_UP:
                    if (history->count > 0 && history_pos > 0) {
                        if (history_pos == history->count) {
                            snprintf(draft, sizeof draft, "%s", buf);
                        }
                        history_pos--;
                        set_input_line(buf, cap, &len, &cursor,
                                       history->items[history_pos]);
                    }
                    break;

                case KEY_DOWN:
                    if (history_pos < history->count) {
                        history_pos++;
                        set_input_line(buf, cap, &len, &cursor,
                                       history_pos == history->count
                                           ? draft
                                           : history->items[history_pos]);
                    }
                    break;

                default:
                    break;
            }

            render_input_line(y, prompt, buf, cursor);
            continue;
        }

        if (wc == 1) {
            cursor = 0;
            render_input_line(y, prompt, buf, cursor);
            continue;
        }

        if (wc == 5) {
            cursor = len;
            render_input_line(y, prompt, buf, cursor);
            continue;
        }

        if (wc == L'\\' || wc == L'λ') {
            insert_input_char(buf, cap, &len, &cursor, '\\');
            render_input_line(y, prompt, buf, cursor);
            continue;
        }

        if (wc >= L'₀' && wc <= L'₉') {
            insert_input_char(buf, cap, &len, &cursor, (char)('0' + (wc - L'₀')));
            render_input_line(y, prompt, buf, cursor);
            continue;
        }

        if (wc < 128 && isprint((unsigned char)wc)) {
            insert_input_char(buf, cap, &len, &cursor, (char)wc);
            render_input_line(y, prompt, buf, cursor);
            continue;
        }
    }
}

static void print_help(void)
{
    printw("Commands:\n");
    printw("  :q             quit\n");
    printw("  :clear         clear the terminal\n");
    printw("  :defs          show definitions\n");
    printw("  :free NAME     forget a saved definition\n");
    printw("  :help          show this help\n");
    printw("\n");
    printw("Keys:\n");
    printw("  Left/Right     move within the current input\n");
    printw("  Up/Down        recall previous/next input\n");
    printw("  Home/End       jump to start/end of input\n");
    printw("\n");
    printw("Syntax:\n");
    printw("  \\x.x           abstraction; displayed as λx.x while typing\n");
    printw("  x y z          application; left associative: (x y) z\n");
    printw("  xx             same as x x; lowercase letters split into variables\n");
    printw("  x1 x2          subscripted variables; displayed as x₁ x₂\n");
    printw("  KI, Ki         uppercase-starting names stay as one identifier\n");
    printw("  (\\x.x) y       beta-reduces to y\n");
    printw("  I = \\x.x       save a named definition\n");
    printw("  I y            use a named definition\n");
    printw("  :free I        remove a saved definition\n");
    printw("  [I, J]         shown beside terms alpha-equivalent to saved definitions\n");
    printw("\n");
}

static void show_defs(const Env *env)
{
    if (env->count == 0) {
        printw("No definitions.\n");
        return;
    }

    for (size_t i = 0; i < env->count; i++) {
        char *s = term_to_string(env->items[i].term, 1);
        printw("%s = %s\n", env->items[i].name, s);
        free(s);
    }
}

static char *alpha_matches_to_string(const Term *term, const Env *env)
{
    size_t cap = 32;
    size_t len = 0;
    int any = 0;
    char *s = malloc(cap);
    if (!s) {
        if (curses_started) endwin();
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }
    s[0] = '\0';

    for (size_t i = 0; i < env->count; i++) {
        if (!term_alpha_equivalent(term, env->items[i].term)) continue;

        const char *name = env->items[i].name;
        size_t need = len + strlen(name) + (any ? 2 : 1) + 2;
        if (need > cap) {
            while (cap < need) cap *= 2;
            char *new_s = realloc(s, cap);
            if (!new_s) {
                free(s);
                if (curses_started) endwin();
                fprintf(stderr, "out of memory\n");
                exit(EXIT_FAILURE);
            }
            s = new_s;
        }

        if (!any) {
            s[len++] = '[';
            s[len] = '\0';
        } else {
            s[len++] = ',';
            s[len++] = ' ';
            s[len] = '\0';
        }

        memcpy(s + len, name, strlen(name) + 1);
        len += strlen(name);
        any = 1;
    }

    if (!any) return s;

    if (len + 2 > cap) {
        char *new_s = realloc(s, len + 2);
        if (!new_s) {
            free(s);
            if (curses_started) endwin();
            fprintf(stderr, "out of memory\n");
            exit(EXIT_FAILURE);
        }
        s = new_s;
    }

    s[len++] = ']';
    s[len] = '\0';
    return s;
}

static void print_term_with_matches(const char *prefix, const Term *term, const Env *env)
{
    char *s = term_to_string(term, 1);
    char *matches = alpha_matches_to_string(term, env);

    if (matches[0] == '\0' && curses_started) {
        printw("%s%s\n", prefix, s);
    } else if (curses_started) {
        int y, x;
        int width = getmaxx(stdscr);
        printw("%s%s", prefix, s);
        getyx(stdscr, y, x);

        int match_len = (int)strlen(matches);
        if (x + 1 + match_len < width) {
            move(y, width - match_len);
            printw("%s", matches);
        } else {
            printw(" %s", matches);
        }
        addch('\n');
    } else {
        printf("%s%s", prefix, s);
        if (matches[0] != '\0') {
            printf("    %s", matches);
        }
        putchar('\n');
    }

    free(matches);
    free(s);
}

static void evaluate_and_print(Term *parsed, const Env *env)
{
    char err[256];
    Term *expanded = expand_defs(parsed, env, err, sizeof err);
    if (!expanded) {
        printw("Expansion error: %s\n", err);
        return;
    }

    Term *current = expanded;
    print_term_with_matches("  ", current, env);

    for (int step = 1; step <= MAX_STEPS; step++) {
        int changed = 0;
        Term *next = term_reduce_once(current, &changed);
        if (!changed) {
            term_free(next);
            break;
        }

        term_free(current);
        current = next;

        print_term_with_matches("→ ", current, env);

        if (step == MAX_STEPS) {
            printw("Stopped after %d steps; term may not have a normal form.\n", MAX_STEPS);
        }
    }

    term_free(current);
}

static int save_definition_from_input(Env *env, char *input, char *err, size_t errsz,
                                      Term **saved)
{
    char *eq = strchr(input, '=');
    if (!eq) {
        snprintf(err, errsz, "expected NAME=TERM");
        return 0;
    }

    *eq = '\0';
    char *name = trim_in_place(input);
    char *rhs = trim_in_place(eq + 1);

    if (!valid_def_name(name)) {
        snprintf(err, errsz,
                 "invalid definition name; use uppercase-starting names like KI, or lowercase variables like x1");
        return 0;
    }

    if (*rhs == '\0') {
        snprintf(err, errsz, "expected a term after '='");
        return 0;
    }

    Term *t = parse_lambda(rhs, err, errsz);
    if (!t) return 0;

    if (!env_set(env, name, t)) {
        snprintf(err, errsz, "too many definitions; cannot add %s", name);
        return 0;
    }

    if (saved) *saved = t;
    return 1;
}

static int define_from_arg(Env *env, const char *arg)
{
    char err[256];
    char *copy = xstrdup_main(arg);

    if (!save_definition_from_input(env, copy, err, sizeof err, NULL)) {
        fprintf(stderr, "Definition error: %s\n", err);
        free(copy);
        return 1;
    }

    free(copy);
    return 0;
}

static int evaluate_source_stdout(const char *source, const Env *env)
{
    char err[256];
    Term *parsed = parse_lambda(source, err, sizeof err);
    if (!parsed) {
        fprintf(stderr, "Parse error: %s\n", err);
        return 1;
    }

    Term *current = expand_defs(parsed, env, err, sizeof err);
    term_free(parsed);
    if (!current) {
        fprintf(stderr, "Expansion error: %s\n", err);
        return 1;
    }

    print_term_with_matches("  ", current, env);

    for (int step = 1; step <= MAX_STEPS; step++) {
        int changed = 0;
        Term *next = term_reduce_once(current, &changed);
        if (!changed) {
            term_free(next);
            break;
        }

        term_free(current);
        current = next;

        print_term_with_matches("→ ", current, env);

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
    fprintf(out, "Launch the interactive ncurses reducer when no EXPR is given.\n");
    fprintf(out, "With expressions, reduce them in order and print each step.\n");
    fprintf(out, "\n");
    fprintf(out, "Options:\n");
    fprintf(out, "  -d, --define NAME=TERM  save a definition before reducing expressions\n");
    fprintf(out, "  -e, --eval EXPR         reduce EXPR\n");
    fprintf(out, "  -f, --free NAME         forget a saved command-line definition\n");
    fprintf(out, "  -h, --help              show this help\n");
    fprintf(out, "  -V, --version           show version information\n");
    fprintf(out, "\n");
    fprintf(out, "Interactive commands include :defs, :free NAME, :clear, :help, and :q.\n");
}

static int missing_arg(const char *option)
{
    fprintf(stderr, "%s: expected an argument\n", option);
    return 2;
}

static int run_batch(int argc, char **argv, Env *env)
{
    int status = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            for (i++; i < argc; i++) {
                status |= evaluate_source_stdout(argv[i], env);
            }
            return status;
        }

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 0;
        }

        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("lambda %s\n", VERSION);
            return 0;
        }

        if (strcmp(arg, "-d") == 0 || strcmp(arg, "--define") == 0) {
            if (++i >= argc) return missing_arg(arg);
            status |= define_from_arg(env, argv[i]);
            continue;
        }

        if (strncmp(arg, "--define=", 9) == 0) {
            status |= define_from_arg(env, arg + 9);
            continue;
        }

        if (strcmp(arg, "-f") == 0 || strcmp(arg, "--free") == 0) {
            if (++i >= argc) return missing_arg(arg);
            if (!env_remove(env, argv[i])) {
                fprintf(stderr, "No definition named %s\n", argv[i]);
                status |= 1;
            }
            continue;
        }

        if (strncmp(arg, "--free=", 7) == 0) {
            if (!env_remove(env, arg + 7)) {
                fprintf(stderr, "No definition named %s\n", arg + 7);
                status |= 1;
            }
            continue;
        }

        if (strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0) {
            if (++i >= argc) return missing_arg(arg);
            status |= evaluate_source_stdout(argv[i], env);
            continue;
        }

        if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg);
            fprintf(stderr, "Try '%s --help'.\n", argv[0]);
            return 2;
        }

        status |= evaluate_source_stdout(arg, env);
    }

    return status;
}

static int run_interactive(Env *env)
{
    initscr();
    curses_started = 1;
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    scrollok(stdscr, TRUE);

    History history;
    history.count = 0;

    printw("Lambda Calculus Beta Reduction\n");
    printw("Type backslash (\\) to enter λ. Type :help for help, :q to quit.\n\n");
    refresh();

    char line[INPUT_CAP];

    while (read_lambda_line("λ> ", line, sizeof line, &history)) {
        char *input = trim_in_place(line);

        if (*input == '\0') continue;

        history_add(&history, input);

        if (strcmp(input, ":q") == 0 || strcmp(input, ":quit") == 0) {
            break;
        }

        if (strcmp(input, ":help") == 0) {
            print_help();
            continue;
        }

        if (strcmp(input, ":clear") == 0) {
            clear();
            refresh();
            continue;
        }

        if (strcmp(input, ":defs") == 0) {
            show_defs(env);
            continue;
        }

        if (strncmp(input, ":free", 5) == 0 &&
            (input[5] == '\0' || isspace((unsigned char)input[5]))) {
            char *name = trim_in_place(input + 5);

            if (*name == '\0') {
                printw("Usage: :free NAME\n");
                continue;
            }

            if (!valid_def_name(name)) {
                printw("Invalid definition name: %s\n", name);
                continue;
            }

            if (env_remove(env, name)) {
                printw("Freed %s.\n", name);
            } else {
                printw("No definition named %s.\n", name);
            }
            continue;
        }

        char *eq = strchr(input, '=');
        if (eq) {
            char err[256];
            Term *saved = NULL;

            if (!save_definition_from_input(env, input, err, sizeof err, &saved)) {
                printw("Definition error: %s\n", err);
                continue;
            }

            char *pretty = term_to_string(saved, 1);
            char *name = trim_in_place(input);
            printw("Saved %s = %s\n", name, pretty);
            free(pretty);
            continue;
        }

        char err[256];
        Term *parsed = parse_lambda(input, err, sizeof err);
        if (!parsed) {
            printw("Parse error: %s\n", err);
            continue;
        }

        evaluate_and_print(parsed, env);
        term_free(parsed);
    }

    history_free(&history);
    endwin();
    curses_started = 0;
    return 0;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");

    Env env;
    env.count = 0;

    int status = argc > 1 ? run_batch(argc, argv, &env) : run_interactive(&env);

    env_free(&env);
    return status;
}
