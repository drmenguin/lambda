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
#include <errno.h>
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
#define VERSION "0.1.7"
#define STEP_PREFIX "→ᵦ "

static int curses_started = 0;

typedef struct {
    char *name;
    char *source;
    Term *term;
    Term *normal;
    int normal_stopped;
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
        free(env->items[i].source);
        term_free(env->items[i].term);
        term_free(env->items[i].normal);
    }
}

static ssize_t env_find(const Env *env, const char *name)
{
    for (size_t i = 0; i < env->count; i++) {
        if (strcmp(env->items[i].name, name) == 0) return (ssize_t)i;
    }
    return -1;
}

static int env_set_raw(Env *env, const char *name, Term *term, const char *source)
{
    ssize_t i = env_find(env, name);
    if (i >= 0) {
        term_free(env->items[i].term);
        term_free(env->items[i].normal);
        free(env->items[i].source);
        env->items[i].term = term;
        env->items[i].normal = NULL;
        env->items[i].normal_stopped = 0;
        env->items[i].source = source ? xstrdup_main(source) : NULL;
        return 1;
    }

    if (env->count >= MAX_DEFS) {
        term_free(term);
        return 0;
    }

    env->items[env->count].name = xstrdup_main(name);
    env->items[env->count].source = source ? xstrdup_main(source) : NULL;
    env->items[env->count].term = term;
    env->items[env->count].normal = NULL;
    env->items[env->count].normal_stopped = 0;
    env->count++;
    return 1;
}

static int env_remove(Env *env, const char *name)
{
    ssize_t idx = env_find(env, name);
    if (idx < 0) return 0;

    size_t i = (size_t)idx;
    free(env->items[i].name);
    free(env->items[i].source);
    term_free(env->items[i].term);
    term_free(env->items[i].normal);

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
                             const Term *last_output,
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

            if (strcmp(name, "%") == 0) {
                if (!last_output) {
                    snprintf(err, errsz, "no previous reduction for '%%'");
                    return NULL;
                }
                return term_clone(last_output);
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

            return expand_defs_rec(env->items[idx].term, env, last_output, bound, nbound,
                                   new_expanding, nexpanding + 1, err, errsz);
        }

        case TERM_APP: {
            Term *l = expand_defs_rec(t->as.app.left, env, last_output, bound, nbound,
                                      expanding, nexpanding, err, errsz);
            if (!l) return NULL;
            Term *r = expand_defs_rec(t->as.app.right, env, last_output, bound, nbound,
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

            Term *body = expand_defs_rec(t->as.abs.body, env, last_output,
                                         new_bound, nbound + 1,
                                         expanding, nexpanding, err, errsz);
            if (!body) return NULL;
            return term_abs(t->as.abs.param, body);
        }
    }

    return NULL;
}

static Term *expand_defs(const Term *t, const Env *env, const Term *last_output,
                         char *err, size_t errsz)
{
    const char *bound[1];
    const char *expanding[1];
    if (err && errsz) err[0] = '\0';
    return expand_defs_rec(t, env, last_output, bound, 0, expanding, 0, err, errsz);
}

static Term *reduce_expanded_normal_form(const Term *t, const Env *env,
                                         const Term *last_output,
                                         int *stopped_early,
                                         char *err, size_t errsz)
{
    Term *expanded = expand_defs(t, env, last_output, err, errsz);
    if (!expanded) return NULL;

    int steps = 0;
    Term *normal = term_reduce_normal_order(expanded, MAX_STEPS, &steps,
                                            stopped_early);
    (void)steps;
    term_free(expanded);
    return normal;
}

static void env_recompute_normals(Env *env, const Term *last_output)
{
    for (size_t i = 0; i < env->count; i++) {
        char err[256];
        int stopped_early = 0;

        term_free(env->items[i].normal);
        env->items[i].normal = reduce_expanded_normal_form(env->items[i].term,
                                                           env, last_output,
                                                           &stopped_early,
                                                           err, sizeof err);
        env->items[i].normal_stopped = env->items[i].normal ? stopped_early : 0;
    }
}

static char *trim_in_place(char *s)
{
    while (isspace((unsigned char)*s)) s++;

    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';

    return s;
}

static int parse_load_path_arg(char *arg, char **path, char *err, size_t errsz)
{
    char *s = trim_in_place(arg);
    if (*s == '\0') {
        snprintf(err, errsz, "expected a file path");
        return 0;
    }

    char *out = s;
    char quote = '\0';
    int closed_quote = 0;
    if (*s == '\'' || *s == '"') {
        quote = *s;
        s++;
    }

    *path = out;

    while (*s) {
        if (quote && *s == quote) {
            s++;
            closed_quote = 1;
            break;
        }

        if (*s == '\\' && s[1] != '\0') {
            s++;
        }

        *out++ = *s++;
    }

    *out = '\0';

    if (quote && closed_quote) {
        s = trim_in_place(s);
        if (*s != '\0') {
            snprintf(err, errsz, "unexpected text after quoted file path");
            return 0;
        }
    }

    if (**path == '\0') {
        snprintf(err, errsz, "expected a file path");
        return 0;
    }

    return 1;
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
    int command = buf[0] == ':';
    for (size_t i = 0; buf[i]; i++) {
        unsigned char c = (unsigned char)buf[i];

        if (i == cursor) {
            int cy;
            getyx(stdscr, cy, cursor_x);
            (void)cy;
        }

        if (!command && buf[i] == '\\') {
            addwstr(L"λ");
            can_subscript = 0;
        } else if (!command && isdigit(c) && can_subscript) {
            addwstr(subscript_digit_wstr(buf[i]));
        } else {
            addch(c);
            can_subscript = !command && (isalpha(c) || c == '\'');
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
    printw("Reduction uses normal-order beta reduction; steps are shown with →ᵦ\n\n");
    printw("Commands:\n");
    printw("  :q             quit\n");
    printw("  :clear         clear the terminal\n");
    printw("  :defs          show definitions\n");
    printw("  :free NAME     forget a saved definition\n");
    printw("  :load FILE     load definitions from FILE\n");
    printw("  :help          show this help\n");
    printw("  :version       show version information\n");
    printw("\n");
    printw("Keys:\n");
    printw("  Left/Right     move within the current input\n");
    printw("  Up/Down        recall previous/next input\n");
    printw("  Home/End       jump to start/end of input\n");
    printw("\n");
    printw("Syntax:\n");
    printw("  \\x.x           abstraction; displayed as λx.x while typing\n");
    printw("  x y z          application; left associative: (x y) z\n");
    printw("  f \\x.x         bare lambda arguments are accepted: f (λx.x)\n");
    printw("  xx             same as x x; lowercase letters split into variables\n");
    printw("  x1 x2          subscripted variables; displayed as x₁ x₂\n");
    printw("  KI, Ki         uppercase-starting names stay as one identifier\n");
    printw("  (\\x.x) y       beta-reduces to y\n");
    printw("  M = \\x.x       save a named definition\n");
    printw("  M <- (\\x.x) y  reduce first, then save the result as M\n");
    printw("  %%              previous reduction result\n");
    printw("  M y            use a named definition\n");
    printw("  :free M        remove a saved definition\n");
    printw("  :load defs.lc  import definitions from the file defs.lc\n");
    printw("  :load \"my file.lc\"  quote or backslash-escape spaces in paths\n");
    printw("\n");
    printw("Other:\n");
    printw("  [M, N]         shown beside terms alpha-equivalent to saved definitions\n");
    printw("  [M*]           M reduces to the displayed term\n");
    printw("\n");
}

static void print_definition(const Definition *def, const char *indent)
{
    char *s = term_to_string(def->term, 1);
    printw("%s%s = %s\n", indent, def->name, s);
    free(s);
}

static int source_seen_before(const Env *env, size_t idx)
{
    const char *source = env->items[idx].source;
    if (!source) return 0;

    for (size_t i = 0; i < idx; i++) {
        if (env->items[i].source && strcmp(env->items[i].source, source) == 0) {
            return 1;
        }
    }

    return 0;
}

static void show_defs(const Env *env)
{
    if (env->count == 0) {
        printw("No definitions.\n");
        return;
    }

    int any_manual = 0;
    for (size_t i = 0; i < env->count; i++) {
        if (env->items[i].source) continue;
        print_definition(&env->items[i], "");
        any_manual = 1;
    }

    int printed_file_group = 0;
    for (size_t i = 0; i < env->count; i++) {
        const char *source = env->items[i].source;
        if (!source || source_seen_before(env, i)) continue;

        if (any_manual || printed_file_group) printw("\n");
        printw("%s:\n", source);
        for (size_t j = i; j < env->count; j++) {
            if (env->items[j].source &&
                strcmp(env->items[j].source, source) == 0) {
                print_definition(&env->items[j], "  ");
            }
        }
        printed_file_group = 1;
    }
}

static void append_match(char **s, size_t *cap, size_t *len, int *any,
                         const char *name, int star)
{
    size_t namelen = strlen(name);
    size_t need = *len + namelen + (star ? 1 : 0) + (*any ? 2 : 1) + 2;
    if (need > *cap) {
        while (*cap < need) *cap *= 2;
        char *new_s = realloc(*s, *cap);
        if (!new_s) {
            free(*s);
            if (curses_started) endwin();
            fprintf(stderr, "out of memory\n");
            exit(EXIT_FAILURE);
        }
        *s = new_s;
    }

    if (!*any) {
        (*s)[(*len)++] = '[';
        (*s)[*len] = '\0';
    } else {
        (*s)[(*len)++] = ',';
        (*s)[(*len)++] = ' ';
        (*s)[*len] = '\0';
    }

    memcpy(*s + *len, name, namelen);
    *len += namelen;
    if (star) (*s)[(*len)++] = '*';
    (*s)[*len] = '\0';
    *any = 1;
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
        if (term_alpha_equivalent(term, env->items[i].term)) {
            append_match(&s, &cap, &len, &any, env->items[i].name, 0);
        } else if (env->items[i].normal &&
                   term_alpha_equivalent(term, env->items[i].normal)) {
            append_match(&s, &cap, &len, &any, env->items[i].name, 1);
        }
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

static int evaluate_and_print(Term *parsed, const Env *env,
                              const Term *last_output, Term **out_final)
{
    char err[256];
    Term *expanded = expand_defs(parsed, env, last_output, err, sizeof err);
    if (!expanded) {
        printw("Expansion error: %s\n", err);
        return 0;
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

        print_term_with_matches(STEP_PREFIX, current, env);

        if (step == MAX_STEPS) {
            printw("Stopped after %d steps; term may not have a normal form.\n", MAX_STEPS);
        }
    }

    if (out_final) *out_final = term_clone(current);
    term_free(current);
    return 1;
}

static int save_definition_from_input(Env *env, char *input, char *err, size_t errsz,
                                      const Term *last_output, Term **saved,
                                      int *reduced_first, int *stopped_early,
                                      const char *source)
{
    char *arrow = strstr(input, "<-");
    char *eq = strchr(input, '=');
    int reduce_first = arrow && (!eq || arrow < eq);
    char *op = reduce_first ? arrow : eq;

    if (!op) {
        snprintf(err, errsz, "expected NAME=TERM or NAME<-TERM");
        return 0;
    }

    *op = '\0';
    char *name = trim_in_place(input);
    char *rhs = trim_in_place(op + (reduce_first ? 2 : 1));

    if (!valid_def_name(name)) {
        snprintf(err, errsz,
                 "invalid definition name; use uppercase-starting names like KI, or lowercase variables like x1");
        return 0;
    }

    if (*rhs == '\0') {
        snprintf(err, errsz, "expected a term after '%s'", reduce_first ? "<-" : "=");
        return 0;
    }

    Term *t = parse_lambda(rhs, err, errsz);
    if (!t) return 0;

    if (reduce_first) {
        int stopped = 0;
        Term *normal = reduce_expanded_normal_form(t, env, last_output,
                                                   &stopped, err, errsz);
        term_free(t);
        if (!normal) return 0;
        t = normal;
        if (stopped_early) *stopped_early = stopped;
    } else if (stopped_early) {
        *stopped_early = 0;
    }

    if (!env_set_raw(env, name, t, source)) {
        snprintf(err, errsz, "too many definitions; cannot add %s", name);
        return 0;
    }

    env_recompute_normals(env, last_output);

    if (saved) {
        ssize_t idx = env_find(env, name);
        *saved = idx >= 0 ? env->items[idx].term : NULL;
    }
    if (reduced_first) *reduced_first = reduce_first;
    return 1;
}

static int define_from_arg(Env *env, const char *arg, const Term *last_output)
{
    char err[256];
    char *copy = xstrdup_main(arg);

    if (!save_definition_from_input(env, copy, err, sizeof err, last_output,
                                    NULL, NULL, NULL, NULL)) {
        fprintf(stderr, "Definition error: %s\n", err);
        free(copy);
        return 1;
    }

    free(copy);
    return 0;
}

static int load_definitions_from_file(Env *env, const char *path,
                                      const Term *last_output,
                                      size_t *imported,
                                      char *err, size_t errsz)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errsz, "%s: %s", path, strerror(errno));
        return 0;
    }

    char line[INPUT_CAP];
    size_t line_no = 0;
    size_t count = 0;

    while (fgets(line, sizeof line, f)) {
        line_no++;
        if (!strchr(line, '\n') && !feof(f)) {
            snprintf(err, errsz, "%s:%zu: line too long", path, line_no);
            fclose(f);
            return 0;
        }
        line[strcspn(line, "\n")] = '\0';

        char *input = trim_in_place(line);
        if (*input == '\0' || *input == '#') continue;

        char def_err[256];
        if (!save_definition_from_input(env, input, def_err, sizeof def_err,
                                        last_output, NULL, NULL, NULL, path)) {
            snprintf(err, errsz, "%s:%zu: %s", path, line_no, def_err);
            fclose(f);
            return 0;
        }
        count++;
    }

    if (ferror(f)) {
        snprintf(err, errsz, "%s: read error", path);
        fclose(f);
        return 0;
    }

    fclose(f);
    if (imported) *imported = count;
    return 1;
}

static int evaluate_source_stdout(const char *source, Env *env, Term **last_output)
{
    char err[256];
    Term *parsed = parse_lambda(source, err, sizeof err);
    if (!parsed) {
        fprintf(stderr, "Parse error: %s\n", err);
        return 1;
    }

    Term *current = expand_defs(parsed, env, last_output ? *last_output : NULL,
                                err, sizeof err);
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

        print_term_with_matches(STEP_PREFIX, current, env);

        if (step == MAX_STEPS) {
            printf("Stopped after %d steps; term may not have a normal form.\n", MAX_STEPS);
        }
    }

    if (last_output) {
        term_free(*last_output);
        *last_output = term_clone(current);
        env_recompute_normals(env, *last_output);
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
    fprintf(out, "Reduction uses normal-order beta reduction; steps are shown with →ᵦ.\n");
    fprintf(out, "\n");
    fprintf(out, "Options:\n");
    fprintf(out, "  -d, --define NAME=TERM  save a lazy definition before reducing expressions\n");
    fprintf(out, "      --define NAME<-TERM reduce first, then save the result\n");
    fprintf(out, "  -e, --eval EXPR         reduce EXPR\n");
    fprintf(out, "  -f, --free NAME         forget a saved command-line definition\n");
    fprintf(out, "  -l, --load FILE         load definitions from FILE\n");
    fprintf(out, "  -h, --help              show this help\n");
    fprintf(out, "  -V, --version           show version information\n");
    fprintf(out, "\n");
    fprintf(out, "Interactive commands include :defs, :free NAME, :load FILE, :version, :clear, :help, and :q.\n");
}

static int missing_arg(const char *option)
{
    fprintf(stderr, "%s: expected an argument\n", option);
    return 2;
}

static int run_batch(int argc, char **argv, Env *env)
{
    int status = 0;
    Term *last_output = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            for (i++; i < argc; i++) {
                status |= evaluate_source_stdout(argv[i], env, &last_output);
            }
            term_free(last_output);
            return status;
        }

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout, argv[0]);
            term_free(last_output);
            return 0;
        }

        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("lambda %s\n", VERSION);
            term_free(last_output);
            return 0;
        }

        if (strcmp(arg, "-d") == 0 || strcmp(arg, "--define") == 0) {
            if (++i >= argc) {
                term_free(last_output);
                return missing_arg(arg);
            }
            status |= define_from_arg(env, argv[i], last_output);
            continue;
        }

        if (strncmp(arg, "--define=", 9) == 0) {
            status |= define_from_arg(env, arg + 9, last_output);
            continue;
        }

        if (strcmp(arg, "-l") == 0 || strcmp(arg, "--load") == 0) {
            if (++i >= argc) {
                term_free(last_output);
                return missing_arg(arg);
            }
            char err[512];
            size_t imported = 0;
            if (!load_definitions_from_file(env, argv[i], last_output,
                                            &imported, err, sizeof err)) {
                fprintf(stderr, "Load error: %s\n", err);
                status |= 1;
            }
            continue;
        }

        if (strncmp(arg, "--load=", 7) == 0) {
            char err[512];
            size_t imported = 0;
            if (!load_definitions_from_file(env, arg + 7, last_output,
                                            &imported, err, sizeof err)) {
                fprintf(stderr, "Load error: %s\n", err);
                status |= 1;
            }
            continue;
        }

        if (strcmp(arg, "-f") == 0 || strcmp(arg, "--free") == 0) {
            if (++i >= argc) {
                term_free(last_output);
                return missing_arg(arg);
            }
            if (!env_remove(env, argv[i])) {
                fprintf(stderr, "No definition named %s\n", argv[i]);
                status |= 1;
            } else {
                env_recompute_normals(env, last_output);
            }
            continue;
        }

        if (strncmp(arg, "--free=", 7) == 0) {
            if (!env_remove(env, arg + 7)) {
                fprintf(stderr, "No definition named %s\n", arg + 7);
                status |= 1;
            } else {
                env_recompute_normals(env, last_output);
            }
            continue;
        }

        if (strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0) {
            if (++i >= argc) {
                term_free(last_output);
                return missing_arg(arg);
            }
            status |= evaluate_source_stdout(argv[i], env, &last_output);
            continue;
        }

        if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", arg);
            fprintf(stderr, "Try '%s --help'.\n", argv[0]);
            term_free(last_output);
            return 2;
        }

        status |= evaluate_source_stdout(arg, env, &last_output);
    }

    term_free(last_output);
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

    printw("Lambda Calculus Beta Reduction %s\n", VERSION);
    printw("Type backslash (\\) to enter λ. Type :help for help, :q to quit.\n\n");
    refresh();

    char line[INPUT_CAP];
    Term *last_output = NULL;

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

        if (strcmp(input, ":version") == 0) {
            printw("lambda %s\n", VERSION);
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

        if (strncmp(input, ":load", 5) == 0 &&
            (input[5] == '\0' || isspace((unsigned char)input[5]))) {
            char *path = NULL;
            char path_err[256];

            if (!parse_load_path_arg(input + 5, &path, path_err, sizeof path_err)) {
                printw("Usage: :load FILE\n");
                printw("Load error: %s\n", path_err);
                continue;
            }

            char err[512];
            size_t imported = 0;
            if (!load_definitions_from_file(env, path, last_output,
                                            &imported, err, sizeof err)) {
                printw("Load error: %s\n", err);
                continue;
            }

            printw("Imported %zu definition%s from %s, type :defs to see %s.\n",
                   imported, imported == 1 ? "" : "s", path,
                   imported == 1 ? "it" : "them");
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
                env_recompute_normals(env, last_output);
            } else {
                printw("No definition named %s.\n", name);
            }
            continue;
        }

        if (strchr(input, '=') || strstr(input, "<-")) {
            char err[256];
            Term *saved = NULL;
            int reduced_first = 0;
            int stopped_early = 0;

            if (!save_definition_from_input(env, input, err, sizeof err,
                                            last_output, &saved,
                                            &reduced_first, &stopped_early,
                                            NULL)) {
                printw("Definition error: %s\n", err);
                continue;
            }

            char *pretty = term_to_string(saved, 1);
            char *name = trim_in_place(input);
            printw("Saved %s %s %s\n", name, reduced_first ? "<-" : "=", pretty);
            if (reduced_first && stopped_early) {
                printw("Stopped after %d steps; term may not have a normal form.\n",
                       MAX_STEPS);
            }
            free(pretty);
            continue;
        }

        char err[256];
        Term *parsed = parse_lambda(input, err, sizeof err);
        if (!parsed) {
            printw("Parse error: %s\n", err);
            continue;
        }

        Term *new_last = NULL;
        if (evaluate_and_print(parsed, env, last_output, &new_last)) {
            term_free(last_output);
            last_output = new_last;
            env_recompute_normals(env, last_output);
        }
        term_free(parsed);
    }

    term_free(last_output);
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
