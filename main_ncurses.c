/*
 * lambda - interactive lambda calculus beta reduction playground with optional eta
 *
 * Copyright (C) 2026 Luke Collins
 * Website: https://lc.mt
 * Source: https://github.com/drmenguin/lambda
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _XOPEN_SOURCE 700
#define _XOPEN_SOURCE_EXTENDED 1

#include "lambda.h"

#include <ctype.h>
#include <locale.h>
#include <curses.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <wchar.h>
#include <wctype.h>

#define INPUT_CAP 4096
#define HISTORY_CAP 128
#define SCROLLBACK_CAP 4096
#define MAX_DEFS 128
#define DEFAULT_MAX_STEPS LAMBDA_DEFAULT_MAX_STEPS
#define VERSION "0.2"
#define STEP_PREFIX_BETA " →ᵦ "
#define STEP_PREFIX_ETA " →η "
#ifndef LAMBDA_DATADIR
#define LAMBDA_DATADIR "/usr/share/lambda"
#endif

static int curses_started = 0;

typedef struct {
    char *name;
    char *source;
    char *original_rhs;
    int reduced_first;
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

typedef struct {
    Term **items;
    size_t count;
    size_t cap;
} LineResults;

typedef struct {
    Term *current;
} StepSession;

typedef struct {
    char *lines[SCROLLBACK_CAP];
    size_t count;
    int scroll;
    int partial;
} OutputLog;

typedef struct {
    int max_steps;
    int eta_enabled;
} Settings;

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

static OutputLog *active_output = NULL;

static void draw_utf8_span(const char *s, int skip_cols, int max_cols)
{
    mbstate_t st;
    memset(&st, 0, sizeof st);

    const char *p = s;
    int drawn = 0;
    while (*p && drawn < max_cols) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, MB_CUR_MAX, &st);
        if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
            memset(&st, 0, sizeof st);
            if (skip_cols > 0) {
                skip_cols--;
                p++;
                continue;
            }
            if (drawn + 1 > max_cols) break;
            addch((unsigned char)*p);
            p++;
            drawn++;
            continue;
        }

        int width = wcwidth(wc);
        if (width < 0) width = 1;
        if (skip_cols > 0) {
            skip_cols -= width;
            if (skip_cols < 0) skip_cols = 0;
            p += n;
            continue;
        }
        if (drawn + width > max_cols) break;
        addnwstr(&wc, 1);
        drawn += width;
        p += n;
    }
}

static int utf8_width(const char *s)
{
    mbstate_t st;
    memset(&st, 0, sizeof st);

    const char *p = s;
    int cols = 0;
    while (*p) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, MB_CUR_MAX, &st);
        if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
            memset(&st, 0, sizeof st);
            p++;
            cols++;
            continue;
        }

        int width = wcwidth(wc);
        cols += width < 0 ? 1 : width;
        p += n;
    }

    return cols;
}

static int visual_rows_for_line(const char *s, int cols)
{
    if (cols < 1) cols = 1;
    int width = utf8_width(s);
    if (width < 1) return 1;
    return (width + cols - 1) / cols;
}

static int output_visual_rows(const OutputLog *out, int cols)
{
    int rows = 0;
    for (size_t i = 0; i < out->count; i++) {
        rows += visual_rows_for_line(out->lines[i], cols);
    }
    return rows;
}

static void output_repaint(void)
{
    if (!curses_started || !active_output) return;

    int rows = getmaxy(stdscr) - 1;
    int cols = getmaxx(stdscr);
    if (rows < 1) return;
    if (cols < 1) cols = 1;

    int total_rows = output_visual_rows(active_output, cols);
    int max_scroll = total_rows - rows;
    if (max_scroll < 0) max_scroll = 0;
    if (active_output->scroll > max_scroll) active_output->scroll = max_scroll;

    int end = total_rows - active_output->scroll;
    if (end < 0) end = 0;
    int start = end > rows ? end - rows : 0;

    for (int y = 0; y < rows; y++) {
        move(y, 0);
        clrtoeol();
    }

    int visual_y = 0;
    int screen_y = 0;
    for (size_t i = 0; i < active_output->count && screen_y < rows; i++) {
        int line_rows = visual_rows_for_line(active_output->lines[i], cols);

        for (int part = 0; part < line_rows; part++, visual_y++) {
            if (visual_y < start) continue;
            if (visual_y >= end) break;

            move(screen_y++, 0);
            draw_utf8_span(active_output->lines[i], part * cols, cols);
        }
    }

    refresh();
}

static void output_free(OutputLog *out)
{
    for (size_t i = 0; i < out->count; i++) free(out->lines[i]);
}

static void output_clear(OutputLog *out)
{
    for (size_t i = 0; i < out->count; i++) free(out->lines[i]);
    out->count = 0;
    out->scroll = 0;
    out->partial = 0;
    output_repaint();
}

static void output_new_line(OutputLog *out)
{
    if (out->count == SCROLLBACK_CAP) {
        free(out->lines[0]);
        memmove(out->lines, out->lines + 1,
                (SCROLLBACK_CAP - 1) * sizeof out->lines[0]);
        out->count--;
    }

    out->lines[out->count++] = xstrdup_main("");
    out->partial = 1;
}

static void output_append_bytes(OutputLog *out, const char *s, size_t n)
{
    if (!out->partial || out->count == 0) output_new_line(out);

    char *old = out->lines[out->count - 1];
    size_t old_len = strlen(old);
    char *new_s = malloc(old_len + n + 1);
    if (!new_s) {
        if (curses_started) endwin();
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }

    memcpy(new_s, old, old_len);
    memcpy(new_s + old_len, s, n);
    new_s[old_len + n] = '\0';
    free(old);
    out->lines[out->count - 1] = new_s;
}

static void output_append_text(OutputLog *out, const char *text)
{
    const char *start = text;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == '\0') {
            if (p > start) output_append_bytes(out, start, (size_t)(p - start));
            if (*p == '\n') {
                if (!out->partial || out->count == 0) output_new_line(out);
                out->partial = 0;
                start = p + 1;
                continue;
            }
            break;
        }
    }

    if (out->scroll == 0) output_repaint();
}

static void output_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (n < 0) {
        va_end(args);
        return;
    }

    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        va_end(args);
        if (curses_started) endwin();
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }

    vsnprintf(buf, (size_t)n + 1, fmt, args);
    va_end(args);

    if (curses_started && active_output) {
        output_append_text(active_output, buf);
    } else if (curses_started) {
        waddstr(stdscr, buf);
    } else {
        printf("%s", buf);
    }

    free(buf);
}

static void output_scroll(OutputLog *out, int delta)
{
    int rows = getmaxy(stdscr) - 1;
    int cols = getmaxx(stdscr);
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;

    int max_scroll = output_visual_rows(out, cols) - rows;
    if (max_scroll < 0) max_scroll = 0;

    out->scroll += delta;
    if (out->scroll < 0) out->scroll = 0;
    if (out->scroll > max_scroll) out->scroll = max_scroll;
    output_repaint();
}

static char *input_to_display_string(const char *buf)
{
    size_t cap = strlen(buf) * 4 + 1;
    char *s = malloc(cap);
    if (!s) {
        if (curses_started) endwin();
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }
    s[0] = '\0';

    size_t len = 0;
    int command = buf[0] == ':';
    int can_subscript = 0;
    for (size_t i = 0; buf[i]; i++) {
        unsigned char c = (unsigned char)buf[i];
        const char *append = NULL;
        char single[2] = {(char)c, '\0'};

        if (!command && buf[i] == '\\') {
            append = "λ";
            can_subscript = 0;
        } else if (!command && isdigit(c) && can_subscript) {
            switch (buf[i]) {
                case '0': append = "₀"; break;
                case '1': append = "₁"; break;
                case '2': append = "₂"; break;
                case '3': append = "₃"; break;
                case '4': append = "₄"; break;
                case '5': append = "₅"; break;
                case '6': append = "₆"; break;
                case '7': append = "₇"; break;
                case '8': append = "₈"; break;
                case '9': append = "₉"; break;
            }
        } else {
            append = single;
            can_subscript = !command && (isalpha(c) || c == '\'');
        }

        size_t need = strlen(append);
        if (len + need + 1 > cap) {
            while (len + need + 1 > cap) cap *= 2;
            char *new_s = realloc(s, cap);
            if (!new_s) {
                free(s);
                if (curses_started) endwin();
                fprintf(stderr, "out of memory\n");
                exit(EXIT_FAILURE);
            }
            s = new_s;
        }
        memcpy(s + len, append, need);
        len += need;
        s[len] = '\0';
    }

    return s;
}

static int output_page_size(void)
{
    int page = getmaxy(stdscr) - 2;
    return page < 1 ? 1 : page;
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
    if (!lines || line_no == 0 || line_no > lines->count) return NULL;
    return lines->items[line_no - 1];
}

static size_t line_results_add(LineResults *lines, const Term *term)
{
    if (lines->count == lines->cap) {
        size_t new_cap = lines->cap ? lines->cap * 2 : 64;
        Term **new_items = realloc(lines->items, new_cap * sizeof lines->items[0]);
        if (!new_items) {
            if (curses_started) endwin();
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

static void env_free(Env *env)
{
    for (size_t i = 0; i < env->count; i++) {
        free(env->items[i].name);
        free(env->items[i].source);
        free(env->items[i].original_rhs);
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

static int env_set_raw(Env *env, const char *name, Term *term,
                       const char *source, const char *original_rhs,
                       int reduced_first)
{
    ssize_t i = env_find(env, name);
    if (i >= 0) {
        term_free(env->items[i].term);
        term_free(env->items[i].normal);
        free(env->items[i].source);
        free(env->items[i].original_rhs);
        env->items[i].term = term;
        env->items[i].normal = NULL;
        env->items[i].normal_stopped = 0;
        env->items[i].source = source ? xstrdup_main(source) : NULL;
        env->items[i].original_rhs = original_rhs ? xstrdup_main(original_rhs) : NULL;
        env->items[i].reduced_first = reduced_first;
        return 1;
    }

    if (env->count >= MAX_DEFS) {
        term_free(term);
        return 0;
    }

    env->items[env->count].name = xstrdup_main(name);
    env->items[env->count].source = source ? xstrdup_main(source) : NULL;
    env->items[env->count].original_rhs = original_rhs ? xstrdup_main(original_rhs) : NULL;
    env->items[env->count].reduced_first = reduced_first;
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
    free(env->items[i].original_rhs);
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
                             const LineResults *lines,
                             const char **bound, size_t nbound,
                             const char **expanding, size_t nexpanding,
                             char *err, size_t errsz)
{
    switch (t->type) {
        case TERM_VAR: {
            const char *name = t->as.var.name;
            size_t line_no = 0;

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

            if (parse_line_ref_name(name, &line_no)) {
                Term *line = line_results_get(lines, line_no);
                if (!line) {
                    snprintf(err, errsz, "no reduction result for line %zu", line_no);
                    return NULL;
                }
                return term_clone(line);
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

            return expand_defs_rec(env->items[idx].term, env, last_output, lines,
                                   bound, nbound,
                                   new_expanding, nexpanding + 1, err, errsz);
        }

        case TERM_APP: {
            Term *l = expand_defs_rec(t->as.app.left, env, last_output, lines,
                                      bound, nbound,
                                      expanding, nexpanding, err, errsz);
            if (!l) return NULL;
            Term *r = expand_defs_rec(t->as.app.right, env, last_output, lines,
                                      bound, nbound,
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

            Term *body = expand_defs_rec(t->as.abs.body, env, last_output, lines,
                                         new_bound, nbound + 1,
                                         expanding, nexpanding, err, errsz);
            if (!body) return NULL;
            return term_abs(t->as.abs.param, body);
        }
    }

    return NULL;
}

static Term *expand_defs(const Term *t, const Env *env, const Term *last_output,
                         const LineResults *lines, char *err, size_t errsz)
{
    const char *bound[1];
    const char *expanding[1];
    if (err && errsz) err[0] = '\0';
    return expand_defs_rec(t, env, last_output, lines, bound, 0,
                           expanding, 0, err, errsz);
}

static Term *expand_defs_shallow_rec(const Term *t, const Env *env,
                                     const Term *last_output,
                                     const LineResults *lines,
                                     const char **bound, size_t nbound,
                                     int *changed, char *err, size_t errsz)
{
    switch (t->type) {
        case TERM_VAR: {
            const char *name = t->as.var.name;
            size_t line_no = 0;

            if (name_in_list(name, bound, nbound)) {
                return term_clone(t);
            }

            if (strcmp(name, "%") == 0) {
                if (!last_output) {
                    snprintf(err, errsz, "no previous reduction for '%%'");
                    return NULL;
                }
                if (changed) *changed = 1;
                return term_clone(last_output);
            }

            if (parse_line_ref_name(name, &line_no)) {
                Term *line = line_results_get(lines, line_no);
                if (!line) {
                    snprintf(err, errsz, "no reduction result for line %zu", line_no);
                    return NULL;
                }
                if (changed) *changed = 1;
                return term_clone(line);
            }

            ssize_t idx = env_find(env, name);
            if (idx < 0) {
                return term_clone(t);
            }

            if (changed) *changed = 1;
            return term_clone(env->items[idx].term);
        }

        case TERM_APP: {
            Term *l = expand_defs_shallow_rec(t->as.app.left, env, last_output,
                                              lines,
                                              bound, nbound, changed, err, errsz);
            if (!l) return NULL;
            Term *r = expand_defs_shallow_rec(t->as.app.right, env, last_output,
                                              lines,
                                              bound, nbound, changed, err, errsz);
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

            Term *body = expand_defs_shallow_rec(t->as.abs.body, env, last_output,
                                                 lines,
                                                 new_bound, nbound + 1,
                                                 changed, err, errsz);
            if (!body) return NULL;
            return term_abs(t->as.abs.param, body);
        }
    }

    return NULL;
}

static Term *expand_defs_shallow(const Term *t, const Env *env,
                                 const Term *last_output,
                                 const LineResults *lines,
                                 int *changed, char *err, size_t errsz)
{
    const char *bound[1];
    if (changed) *changed = 0;
    if (err && errsz) err[0] = '\0';
    return expand_defs_shallow_rec(t, env, last_output, lines, bound, 0,
                                   changed, err, errsz);
}

static Term *capture_last_output_refs_rec(const Term *t,
                                          const Term *last_output,
                                          const LineResults *lines,
                                          const char **bound, size_t nbound,
                                          char *err, size_t errsz)
{
    switch (t->type) {
        case TERM_VAR: {
            const char *name = t->as.var.name;
            size_t line_no = 0;

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
            Term *l = capture_last_output_refs_rec(t->as.app.left, last_output,
                                                   lines,
                                                   bound, nbound, err, errsz);
            if (!l) return NULL;
            Term *r = capture_last_output_refs_rec(t->as.app.right, last_output,
                                                   lines,
                                                   bound, nbound, err, errsz);
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

            Term *body = capture_last_output_refs_rec(t->as.abs.body,
                                                      last_output,
                                                      lines,
                                                      new_bound, nbound + 1,
                                                      err, errsz);
            if (!body) return NULL;
            return term_abs(t->as.abs.param, body);
        }
    }

    return NULL;
}

static Term *capture_last_output_refs(const Term *t, const Term *last_output,
                                      const LineResults *lines,
                                      char *err, size_t errsz)
{
    const char *bound[1];
    if (err && errsz) err[0] = '\0';
    return capture_last_output_refs_rec(t, last_output, lines, bound, 0,
                                        err, errsz);
}

static Term *reduce_expanded_normal_form(const Term *t, const Env *env,
                                         const Term *last_output,
                                         int max_steps, int eta_enabled,
                                         int *stopped_early,
                                         char *err, size_t errsz)
{
    Term *expanded = expand_defs(t, env, last_output, NULL, err, errsz);
    if (!expanded) return NULL;

    int steps = 0;
    Term *normal = term_reduce_normal_order_with_eta(expanded, max_steps,
                                                     &steps, stopped_early,
                                                     eta_enabled);
    (void)steps;
    term_free(expanded);
    return normal;
}

static void env_recompute_normals(Env *env, const Term *last_output,
                                  int max_steps, int eta_enabled)
{
    for (size_t i = 0; i < env->count; i++) {
        char err[256];
        int stopped_early = 0;

        term_free(env->items[i].normal);
        env->items[i].normal = reduce_expanded_normal_form(env->items[i].term,
                                                           env, last_output,
                                                           max_steps,
                                                           eta_enabled,
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

static int parse_positive_int_arg(const char *arg, const char *label,
                                  int *out, char *err, size_t errsz)
{
    const char *s = arg;
    while (isspace((unsigned char)*s)) s++;

    if (*s == '\0') {
        snprintf(err, errsz, "expected a number after %s", label);
        return 0;
    }

    errno = 0;
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (s == end || errno == ERANGE || value < 1 || value > INT_MAX) {
        snprintf(err, errsz, "expected a positive number after %s", label);
        return 0;
    }

    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') {
        snprintf(err, errsz, "unexpected text after %s value", label);
        return 0;
    }

    *out = (int)value;
    return 1;
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
    } else if (!parse_positive_int_arg(number, "/", step_limit, err, errsz)) {
        return 0;
    }

    *slash = '\0';
    while (slash > input && isspace((unsigned char)slash[-1])) {
        *--slash = '\0';
    }

    return 1;
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

static char *expand_load_path(const char *path, char *err, size_t errsz)
{
    if (strcmp(path, "std") == 0) {
        return xstrdup_main("std.lc");
    }

    if (path[0] != '~' || (path[1] != '\0' && path[1] != '/')) {
        return xstrdup_main(path);
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        snprintf(err, errsz, "cannot expand '~': HOME is not set");
        return NULL;
    }

    size_t home_len = strlen(home);
    size_t rest_len = strlen(path + 1);
    char *expanded = malloc(home_len + rest_len + 1);
    if (!expanded) {
        if (curses_started) endwin();
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }

    memcpy(expanded, home, home_len);
    memcpy(expanded + home_len, path + 1, rest_len + 1);
    return expanded;
}

static char *standard_library_path(void)
{
    const char suffix[] = "/std.lc";
    size_t base_len = strlen(LAMBDA_DATADIR);
    size_t suffix_len = strlen(suffix);
    char *path = malloc(base_len + suffix_len + 1);
    if (!path) {
        if (curses_started) endwin();
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }

    memcpy(path, LAMBDA_DATADIR, base_len);
    memcpy(path + base_len, suffix, suffix_len + 1);
    return path;
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
    int y = getmaxy(stdscr) - 1;

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
            buf[len] = '\0';
            if (active_output) active_output->scroll = 0;
            char *display = input_to_display_string(buf);
            output_printf("%s%s\n", prompt, display);
            free(display);
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

                case KEY_PPAGE:
                    if (active_output) output_scroll(active_output, output_page_size());
                    break;

                case KEY_NPAGE:
                    if (active_output) output_scroll(active_output, -output_page_size());
                    break;

                case KEY_RESIZE:
                    output_repaint();
                    y = getmaxy(stdscr) - 1;
                    break;

#ifdef KEY_MOUSE
                case KEY_MOUSE: {
                    MEVENT event;
                    if (getmouse(&event) == OK && active_output) {
#ifdef BUTTON4_PRESSED
                        if (event.bstate & BUTTON4_PRESSED) {
                            output_scroll(active_output, 3);
                        }
#endif
#ifdef BUTTON5_PRESSED
                        if (event.bstate & BUTTON5_PRESSED) {
                            output_scroll(active_output, -3);
                        }
#endif
                    }
                    break;
                }
#endif

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
    output_printf("Reduction uses normal-order beta reduction; :eta toggles eta reduction.\n\n");
    output_printf("Commands:\n");
    output_printf("  :q             quit\n");
    output_printf("  :clear         clear the terminal and scrollback\n");
    output_printf("  :def NAME      show how a saved definition is defined\n");
    output_printf("  :defs          show all saved definitions\n");
    output_printf("  :free NAME     forget a saved definition\n");
    output_printf("  :load FILE     load definitions from FILE\n");
    output_printf("  :load std      load std.lc: combinators, booleans, numbers, pairs, lists\n");
    output_printf("  :eta [on|off]  toggle or set eta reduction (default off)\n");
    output_printf("  :max-steps [N] show or set the reduction step limit (default %d)\n",
                  DEFAULT_MAX_STEPS);
    output_printf("  :help          show this help\n");
    output_printf("  :version       show version information\n");
    output_printf("\n");
    output_printf("Keys:\n");
    output_printf("  Left/Right     move within the current input\n");
    output_printf("  Up/Down        recall previous/next input\n");
    output_printf("  PageUp/PageDown scroll through previous output\n");
    output_printf("  Mouse wheel    scroll through previous output\n");
    output_printf("  Home/End       jump to start/end of input\n");
    output_printf("\n");
    output_printf("Syntax:\n");
    output_printf("  \\x.x           abstraction; displayed as λx.x while typing\n");
    output_printf("  x y z          application; left associative: (x y) z\n");
    output_printf("  f \\x.x         bare lambda arguments are accepted: f (λx.x)\n");
    output_printf("  xx             same as x x; lowercase letters split into variables\n");
    output_printf("  x1 x2          subscripted variables; displayed as x₁ x₂\n");
    output_printf("  KI, Ki         uppercase-starting names stay as one identifier\n");
    output_printf("  (\\x.x) y       reduces to y\n");
    output_printf("  M = \\x.x       save a named definition\n");
    output_printf("  M <- (\\x.x) y  reduce first, then save the result as M\n");
    output_printf("  %%              previous numbered output result\n");
    output_printf("  %%n             result from output line number n\n");
    output_printf("  M/             reduce M by one reduction step; (...) means more steps remain\n");
    output_printf("  M/n            show n reduction steps and stop\n");
    output_printf("  / or <Enter>   continue the previous gradual reduction\n");
    output_printf("  /n             continue with n reduction steps\n");
    output_printf("  M y            use a named definition\n");
    output_printf("  :def M         show how M is defined\n");
    output_printf("  :free M        remove a saved definition\n");
    output_printf("\n");
    output_printf("Other:\n");
    output_printf("  [M, N]         shown beside terms alpha-equivalent to saved definitions\n");
    output_printf("  [M*]           M reduces to the displayed term\n");
    output_printf("\n");
}

static void print_definition(const Definition *def, const char *indent)
{
    char *s = term_to_string(def->term, 1);
    output_printf("%s%s = %s\n", indent, def->name, s);
    free(s);
}

static void show_def(const Env *env, const char *name)
{
    ssize_t idx = env_find(env, name);
    if (idx < 0) {
        output_printf("No definition named %s.\n", name);
        return;
    }

    const Definition *def = &env->items[idx];
    if (def->reduced_first) {
        output_printf("%s <- %s  (as defined)\n",
               def->name, def->original_rhs ? def->original_rhs : "");
    }

    print_definition(def, "");
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
        output_printf("No definitions.\n");
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

        if (any_manual || printed_file_group) output_printf("\n");
        output_printf("%s:\n", source);
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

static void print_term_with_matches_suffix(const char *prefix, const Term *term,
                                           const Env *env, const char *suffix)
{
    char *s = term_to_string(term, 1);
    char *matches = alpha_matches_to_string(term, env);
    const char *tail = suffix ? suffix : "";

    if (curses_started) {
        int width = getmaxx(stdscr);

        if (matches[0] == '\0') {
            output_printf("%s%s%s\n", prefix, s, tail);
        } else {
            int base_width = utf8_width(prefix) + utf8_width(s) + utf8_width(tail);
            int match_width = utf8_width(matches);
            if (base_width + 1 + match_width < width) {
                int padding = width - base_width - match_width;
                output_printf("%s%s%s%*s%s\n", prefix, s, tail, padding, "", matches);
            } else {
                output_printf("%s%s%s %s\n", prefix, s, tail, matches);
            }
        }
    } else {
        printf("%s%s%s", prefix, s, tail);
        if (matches[0] != '\0') {
            printf("    %s", matches);
        }
        putchar('\n');
    }

    free(matches);
    free(s);
}

static void print_term_with_matches(const char *prefix, const Term *term, const Env *env)
{
    print_term_with_matches_suffix(prefix, term, env, "");
}

static int term_can_reduce(const Term *term, int eta_enabled)
{
    int changed = 0;
    Term *next = term_reduce_once_kind(term, &changed, NULL, eta_enabled);
    term_free(next);
    return changed;
}

static const char *reduction_prefix(ReductionKind kind)
{
    return kind == REDUCTION_ETA ? STEP_PREFIX_ETA : STEP_PREFIX_BETA;
}

static const char *reduction_marker(ReductionKind kind)
{
    return kind == REDUCTION_ETA ? "η" : "ᵦ";
}

static void print_numbered_term(LineResults *lines, const char *marker,
                                const Term *term, const Env *env,
                                const char *suffix)
{
    size_t line_no = line_results_add(lines, term);
    char prefix[64];
    snprintf(prefix, sizeof prefix, "[%zu]>%s ", line_no, marker);
    print_term_with_matches_suffix(prefix, term, env, suffix);
}

static int reduce_steps_and_print(Term **current, int step_limit,
                                  LineResults *lines, const Env *env,
                                  int eta_enabled)
{
    int steps_taken = 0;

    for (int step = 1; step <= step_limit; step++) {
        int changed = 0;
        ReductionKind kind = REDUCTION_NONE;
        Term *next = term_reduce_once_kind(*current, &changed, &kind,
                                           eta_enabled);
        if (!changed) {
            term_free(next);
            break;
        }

        term_free(*current);
        *current = next;
        steps_taken++;

        int more = term_can_reduce(*current, eta_enabled);
        if (more && step < step_limit) {
            print_term_with_matches(reduction_prefix(kind), *current, env);
        } else {
            print_numbered_term(lines, reduction_marker(kind), *current, env,
                                more ? "  (...)" : "");
            break;
        }
    }

    return steps_taken;
}

static int evaluate_and_print(Term *parsed, const Env *env,
                              const Term *last_output,
                              LineResults *lines,
                              int max_steps, int step_limit,
                              int eta_enabled,
                              Term **out_final)
{
    char err[256];

    int shallow_changed = 0;
    Term *shallow = expand_defs_shallow(parsed, env, last_output, lines,
                                        &shallow_changed, err, sizeof err);
    if (!shallow) {
        output_printf("Expansion error: %s\n", err);
        return 0;
    }

    Term *expanded = expand_defs(parsed, env, last_output, lines, err, sizeof err);
    if (!expanded) {
        output_printf("Expansion error: %s\n", err);
        term_free(shallow);
        return 0;
    }

    Term *current = expanded;
    if (shallow_changed) {
        print_term_with_matches("  ", shallow, env);
    } else if (!curses_started && !step_limit) {
        print_term_with_matches("  ", current, env);
    }

    if (shallow_changed && !term_alpha_equivalent(shallow, current)) {
        print_term_with_matches(STEP_PREFIX_BETA, current, env);
    }
    term_free(shallow);

    if (step_limit) {
        reduce_steps_and_print(&current, step_limit, lines, env, eta_enabled);
        if (out_final) *out_final = term_clone(current);
        term_free(current);
        return 1;
    }

    int numbered_printed = 0;
    for (int step = 1; step <= max_steps; step++) {
        int changed = 0;
        ReductionKind kind = REDUCTION_NONE;
        Term *next = term_reduce_once_kind(current, &changed, &kind,
                                           eta_enabled);
        if (!changed) {
            term_free(next);
            break;
        }

        term_free(current);
        current = next;

        int more = term_can_reduce(current, eta_enabled);
        if (more && step < max_steps) {
            print_term_with_matches(reduction_prefix(kind), current, env);
        } else {
            print_numbered_term(lines, reduction_marker(kind), current, env,
                                more ? "  (...)" : "");
            numbered_printed = 1;
        }

        if (step == max_steps) {
            output_printf("Stopped after %d steps; term may not have a normal form.\n",
                          max_steps);
        }
    }

    if (!numbered_printed) {
        print_numbered_term(lines, "", current, env, "");
    }
    if (out_final) *out_final = term_clone(current);
    term_free(current);
    return 1;
}

static int save_definition_from_input(Env *env, char *input, char *err, size_t errsz,
                                      const Term *last_output,
                                      const LineResults *lines,
                                      Term **saved,
                                      int *reduced_first, int *stopped_early,
                                      const char *source, int max_steps,
                                      int eta_enabled)
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

    Term *captured = capture_last_output_refs(t, last_output, lines, err, errsz);
    term_free(t);
    if (!captured) return 0;
    t = captured;

    if (reduce_first) {
        int stopped = 0;
        Term *normal = reduce_expanded_normal_form(t, env, last_output,
                                                   max_steps, eta_enabled,
                                                   &stopped, err, errsz);
        term_free(t);
        if (!normal) return 0;
        t = normal;
        if (stopped_early) *stopped_early = stopped;
    } else if (stopped_early) {
        *stopped_early = 0;
    }

    if (!env_set_raw(env, name, t, source, rhs, reduce_first)) {
        snprintf(err, errsz, "too many definitions; cannot add %s", name);
        return 0;
    }

    env_recompute_normals(env, last_output, max_steps, eta_enabled);

    if (saved) {
        ssize_t idx = env_find(env, name);
        *saved = idx >= 0 ? env->items[idx].term : NULL;
    }
    if (reduced_first) *reduced_first = reduce_first;
    return 1;
}

static int define_from_arg(Env *env, const char *arg, const Term *last_output,
                           const LineResults *lines, int max_steps,
                           int eta_enabled)
{
    char err[256];
    char *copy = xstrdup_main(arg);

    if (!save_definition_from_input(env, copy, err, sizeof err, last_output,
                                    lines, NULL, NULL, NULL, NULL, max_steps,
                                    eta_enabled)) {
        fprintf(stderr, "Definition error: %s\n", err);
        free(copy);
        return 1;
    }

    free(copy);
    return 0;
}

static int load_definitions_from_file(Env *env, const char *path,
                                      const Term *last_output,
                                      int max_steps, int eta_enabled,
                                      size_t *imported,
                                      char *err, size_t errsz)
{
    char *open_path = expand_load_path(path, err, errsz);
    if (!open_path) return 0;

    FILE *f = fopen(open_path, "r");
    if (!f && strcmp(path, "std") == 0) {
        char *installed_path = standard_library_path();
        f = fopen(installed_path, "r");
        if (f) {
            free(open_path);
            open_path = installed_path;
        } else {
            free(installed_path);
        }
    }
    if (!f) {
        snprintf(err, errsz, "%s: %s", open_path, strerror(errno));
        free(open_path);
        return 0;
    }
    free(open_path);

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
                                        last_output, NULL,
                                        NULL, NULL, NULL, path,
                                        max_steps, eta_enabled)) {
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

static int evaluate_source_stdout(const char *source, Env *env,
                                  Term **last_output,
                                  LineResults *lines,
                                  StepSession *session,
                                  int max_steps, int eta_enabled,
                                  Term **out_result)
{
    char err[256];
    char *copy = xstrdup_main(source);
    int step_limit = 0;

    if (!parse_step_suffix(copy, &step_limit, err, sizeof err)) {
        fprintf(stderr, "Parse error: %s\n", err);
        free(copy);
        return 1;
    }

    if (copy[0] == '\0') {
        if (!step_limit) {
            if (!session || !session->current) {
                free(copy);
                return 0;
            }
            step_limit = 1;
        }
        if (!session || !session->current) {
            fprintf(stderr, "No gradual reduction to continue.\n");
            free(copy);
            return 1;
        }

        Term *current = term_clone(session->current);
        int steps = reduce_steps_and_print(&current, step_limit, lines, env,
                                           eta_enabled);
        if (steps == 0) {
            fprintf(stderr, "Already in normal form.\n");
        }
        step_session_set(session, current);
        if (last_output) {
            replace_term(last_output, current);
            env_recompute_normals(env, *last_output, max_steps, eta_enabled);
        }
        if (out_result) *out_result = term_clone(current);
        term_free(current);
        free(copy);
        return 0;
    }

    Term *parsed = parse_lambda(copy, err, sizeof err);
    if (!parsed) {
        fprintf(stderr, "Parse error: %s\n", err);
        free(copy);
        return 1;
    }

    int shallow_changed = 0;
    Term *shallow = expand_defs_shallow(parsed, env,
                                        last_output ? *last_output : NULL,
                                        lines,
                                        &shallow_changed, err, sizeof err);
    if (!shallow) {
        fprintf(stderr, "Expansion error: %s\n", err);
        term_free(parsed);
        free(copy);
        return 1;
    }

    Term *current = expand_defs(parsed, env, last_output ? *last_output : NULL,
                                lines, err, sizeof err);
    if (!current) {
        fprintf(stderr, "Expansion error: %s\n", err);
        term_free(shallow);
        term_free(parsed);
        free(copy);
        return 1;
    }

    if (shallow_changed) {
        print_term_with_matches("  ", shallow, env);
    } else if (!step_limit) {
        print_term_with_matches("  ", current, env);
    }

    if (shallow_changed && !term_alpha_equivalent(shallow, current)) {
        print_term_with_matches(STEP_PREFIX_BETA, current, env);
    }
    term_free(shallow);
    term_free(parsed);

    if (step_limit) {
        reduce_steps_and_print(&current, step_limit, lines, env, eta_enabled);
        if (session) step_session_set(session, current);
        if (last_output) {
            replace_term(last_output, current);
            env_recompute_normals(env, *last_output, max_steps, eta_enabled);
        }
        if (out_result) *out_result = term_clone(current);
        term_free(current);
        free(copy);
        return 0;
    }

    if (session) step_session_clear(session);

    int numbered_printed = 0;
    for (int step = 1; step <= max_steps; step++) {
        int changed = 0;
        ReductionKind kind = REDUCTION_NONE;
        Term *next = term_reduce_once_kind(current, &changed, &kind,
                                           eta_enabled);
        if (!changed) {
            term_free(next);
            break;
        }

        term_free(current);
        current = next;

        int more = term_can_reduce(current, eta_enabled);
        if (more && step < max_steps) {
            print_term_with_matches(reduction_prefix(kind), current, env);
        } else {
            print_numbered_term(lines, reduction_marker(kind), current, env,
                                more ? "  (...)" : "");
            numbered_printed = 1;
        }

        if (step == max_steps) {
            printf("Stopped after %d steps; term may not have a normal form.\n",
                   max_steps);
        }
    }

    if (!numbered_printed) {
        print_numbered_term(lines, "", current, env, "");
    }
    if (last_output) {
        replace_term(last_output, current);
        env_recompute_normals(env, *last_output, max_steps, eta_enabled);
    }
    if (out_result) *out_result = term_clone(current);
    term_free(current);
    free(copy);
    return 0;
}

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out, "Usage: %s [OPTION...] [EXPR...]\n", prog);
    fprintf(out, "\n");
    fprintf(out, "Launch the interactive ncurses reducer when no EXPR is given.\n");
    fprintf(out, "With expressions, reduce them in order and print each step.\n");
    fprintf(out, "Reduction uses normal-order beta reduction; final results are numbered as [N]>.\n");
    fprintf(out, "Use %% for the previous numbered result, %%N for output line N, and trailing / or /N for gradual reduction.\n");
    fprintf(out, "\n");
    fprintf(out, "Options:\n");
    fprintf(out, "  -d, --define NAME=TERM  save a lazy definition before reducing expressions\n");
    fprintf(out, "      --define NAME<-TERM reduce first, then save the result\n");
    fprintf(out, "  -e, --eval EXPR         reduce EXPR\n");
    fprintf(out, "  -f, --free NAME         forget a saved command-line definition\n");
    fprintf(out, "  -l, --load FILE         load definitions from FILE\n");
    fprintf(out, "                          use --load std for the standard library\n");
    fprintf(out, "      --max-steps N       stop reducing after N steps (default %d)\n",
            DEFAULT_MAX_STEPS);
    fprintf(out, "  -h, --help              show this help\n");
    fprintf(out, "  -V, --version           show version information\n");
    fprintf(out, "\n");
    fprintf(out, "Interactive commands include :def NAME, :defs, :free NAME, :load FILE, :eta [on|off], :max-steps [N], :version, :clear, :help, and :q.\n");
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
    LineResults lines = {0};
    StepSession session = {0};
    Settings settings = { DEFAULT_MAX_STEPS, 0 };

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            for (i++; i < argc; i++) {
                Term *result = NULL;
                int rc = evaluate_source_stdout(argv[i], env, &last_output,
                                                &lines, &session,
                                                settings.max_steps,
                                                settings.eta_enabled,
                                                &result);
                status |= rc;
                term_free(result);
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
            printf("lambda %s\n", VERSION);
            term_free(last_output);
            step_session_clear(&session);
            line_results_free(&lines);
            return 0;
        }

        if (strcmp(arg, "-d") == 0 || strcmp(arg, "--define") == 0) {
            if (++i >= argc) {
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return missing_arg(arg);
            }
            status |= define_from_arg(env, argv[i], last_output, &lines,
                                      settings.max_steps,
                                      settings.eta_enabled);
            continue;
        }

        if (strncmp(arg, "--define=", 9) == 0) {
            status |= define_from_arg(env, arg + 9, last_output, &lines,
                                      settings.max_steps,
                                      settings.eta_enabled);
            continue;
        }

        if (strcmp(arg, "-l") == 0 || strcmp(arg, "--load") == 0) {
            if (++i >= argc) {
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return missing_arg(arg);
            }
            char err[512];
            size_t imported = 0;
            if (!load_definitions_from_file(env, argv[i], last_output,
                                            settings.max_steps,
                                            settings.eta_enabled,
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
                                            settings.max_steps,
                                            settings.eta_enabled,
                                            &imported, err, sizeof err)) {
                fprintf(stderr, "Load error: %s\n", err);
                status |= 1;
            }
            continue;
        }

        if (strcmp(arg, "-f") == 0 || strcmp(arg, "--free") == 0) {
            if (++i >= argc) {
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return missing_arg(arg);
            }
            if (!env_remove(env, argv[i])) {
                fprintf(stderr, "No definition named %s\n", argv[i]);
                status |= 1;
            } else {
                env_recompute_normals(env, last_output, settings.max_steps,
                                      settings.eta_enabled);
            }
            continue;
        }

        if (strncmp(arg, "--free=", 7) == 0) {
            if (!env_remove(env, arg + 7)) {
                fprintf(stderr, "No definition named %s\n", arg + 7);
                status |= 1;
            } else {
                env_recompute_normals(env, last_output, settings.max_steps,
                                      settings.eta_enabled);
            }
            continue;
        }

        if (strcmp(arg, "--max-steps") == 0) {
            if (++i >= argc) {
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return missing_arg(arg);
            }
            char err[256];
            if (!parse_positive_int_arg(argv[i], "--max-steps",
                                        &settings.max_steps, err, sizeof err)) {
                fprintf(stderr, "%s\n", err);
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return 2;
            }
            env_recompute_normals(env, last_output, settings.max_steps,
                                  settings.eta_enabled);
            continue;
        }

        if (strncmp(arg, "--max-steps=", 12) == 0) {
            char err[256];
            if (!parse_positive_int_arg(arg + 12, "--max-steps",
                                        &settings.max_steps, err, sizeof err)) {
                fprintf(stderr, "%s\n", err);
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return 2;
            }
            env_recompute_normals(env, last_output, settings.max_steps,
                                  settings.eta_enabled);
            continue;
        }

        if (strcmp(arg, "-e") == 0 || strcmp(arg, "--eval") == 0) {
            if (++i >= argc) {
                term_free(last_output);
                step_session_clear(&session);
                line_results_free(&lines);
                return missing_arg(arg);
            }
            Term *result = NULL;
            int rc = evaluate_source_stdout(argv[i], env, &last_output,
                                            &lines, &session,
                                            settings.max_steps,
                                            settings.eta_enabled,
                                            &result);
            status |= rc;
            term_free(result);
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

        Term *result = NULL;
        int rc = evaluate_source_stdout(arg, env, &last_output,
                                        &lines, &session,
                                        settings.max_steps,
                                        settings.eta_enabled,
                                        &result);
        status |= rc;
        term_free(result);
    }

    term_free(last_output);
    step_session_clear(&session);
    line_results_free(&lines);
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
#ifdef ALL_MOUSE_EVENTS
    mousemask(ALL_MOUSE_EVENTS, NULL);
#endif

    History history;
    history.count = 0;

    OutputLog output;
    memset(&output, 0, sizeof output);
    active_output = &output;

    output_printf("Lambda Calculus Beta Reduction %s\n", VERSION);
    output_printf("Type backslash (\\) to enter λ. Type :help for help, :q to quit.\n\n");
    refresh();

    char line[INPUT_CAP];
    Term *last_output = NULL;
    LineResults lines = {0};
    StepSession session = {0};
    Settings settings = { DEFAULT_MAX_STEPS, 0 };

    while (1) {
        if (!read_lambda_line("λ> ", line, sizeof line, &history)) break;

        char *input = trim_in_place(line);

        if (*input == '\0') {
            if (!session.current) continue;

            Term *current = term_clone(session.current);
            int steps = reduce_steps_and_print(&current, 1, &lines, env,
                                               settings.eta_enabled);
            if (steps == 0) {
                output_printf("Already in normal form.\n");
            }
            step_session_set(&session, current);
            replace_term(&last_output, current);
            env_recompute_normals(env, last_output, settings.max_steps,
                                  settings.eta_enabled);
            term_free(current);
            continue;
        }

        history_add(&history, input);

        if (strcmp(input, ":q") == 0 || strcmp(input, ":quit") == 0) {
            break;
        }

        if (strcmp(input, ":help") == 0) {
            print_help();
            continue;
        }

        if (strcmp(input, ":version") == 0) {
            output_printf("lambda %s\n", VERSION);
            continue;
        }

        if (strcmp(input, ":clear") == 0) {
            output_clear(&output);
            move(getmaxy(stdscr) - 1, 0);
            clrtoeol();
            refresh();
            continue;
        }

        if (strncmp(input, ":max-steps", 10) == 0 &&
            (input[10] == '\0' || isspace((unsigned char)input[10]))) {
            char *arg = trim_in_place(input + 10);
            if (*arg == '\0') {
                output_printf("max-steps = %d\n", settings.max_steps);
                continue;
            }

            char err[256];
            int max_steps = 0;
            if (!parse_positive_int_arg(arg, ":max-steps",
                                        &max_steps, err, sizeof err)) {
                output_printf("Usage: :max-steps N\n");
                output_printf("Settings error: %s\n", err);
                continue;
            }

            settings.max_steps = max_steps;
            env_recompute_normals(env, last_output, settings.max_steps,
                                  settings.eta_enabled);
            output_printf("max-steps = %d\n", settings.max_steps);
            continue;
        }

        if (strncmp(input, ":eta", 4) == 0 &&
            (input[4] == '\0' || isspace((unsigned char)input[4]))) {
            char *arg = trim_in_place(input + 4);

            if (*arg == '\0') {
                settings.eta_enabled = !settings.eta_enabled;
            } else if (strcmp(arg, "on") == 0) {
                settings.eta_enabled = 1;
            } else if (strcmp(arg, "off") == 0) {
                settings.eta_enabled = 0;
            } else {
                output_printf("Usage: :eta [on|off]\n");
                continue;
            }

            env_recompute_normals(env, last_output, settings.max_steps,
                                  settings.eta_enabled);
            output_printf("eta = %s\n", settings.eta_enabled ? "on" : "off");
            continue;
        }

        if (strcmp(input, ":defs") == 0) {
            show_defs(env);
            continue;
        }

        if (strncmp(input, ":def", 4) == 0 &&
            (input[4] == '\0' || isspace((unsigned char)input[4]))) {
            char *name = trim_in_place(input + 4);

            if (*name == '\0') {
                output_printf("Usage: :def NAME\n");
                continue;
            }

            if (!valid_def_name(name)) {
                output_printf("Invalid definition name: %s\n", name);
                continue;
            }

            show_def(env, name);
            continue;
        }

        if (strncmp(input, ":load", 5) == 0 &&
            (input[5] == '\0' || isspace((unsigned char)input[5]))) {
            char *path = NULL;
            char path_err[256];

            if (!parse_load_path_arg(input + 5, &path, path_err, sizeof path_err)) {
                output_printf("Usage: :load FILE\n");
                output_printf("Load error: %s\n", path_err);
                continue;
            }

            char err[512];
            size_t imported = 0;
            if (!load_definitions_from_file(env, path, last_output,
                                            settings.max_steps,
                                            settings.eta_enabled,
                                            &imported, err, sizeof err)) {
                output_printf("Load error: %s\n", err);
                continue;
            }

            output_printf("Imported %zu definition%s from %s, type :defs to see %s.\n",
                   imported, imported == 1 ? "" : "s", path,
                   imported == 1 ? "it" : "them");
            continue;
        }

        if (strncmp(input, ":free", 5) == 0 &&
            (input[5] == '\0' || isspace((unsigned char)input[5]))) {
            char *name = trim_in_place(input + 5);

            if (*name == '\0') {
                output_printf("Usage: :free NAME\n");
                continue;
            }

            if (!valid_def_name(name)) {
                output_printf("Invalid definition name: %s\n", name);
                continue;
            }

            if (env_remove(env, name)) {
                output_printf("Freed %s.\n", name);
                env_recompute_normals(env, last_output, settings.max_steps,
                                      settings.eta_enabled);
            } else {
                output_printf("No definition named %s.\n", name);
            }
            continue;
        }

        if (strchr(input, '=') || strstr(input, "<-")) {
            char err[256];
            Term *saved = NULL;
            int reduced_first = 0;
            int stopped_early = 0;

            if (!save_definition_from_input(env, input, err, sizeof err,
                                            last_output, &lines, &saved,
                                            &reduced_first, &stopped_early,
                                            NULL, settings.max_steps,
                                            settings.eta_enabled)) {
                output_printf("Definition error: %s\n", err);
                continue;
            }

            char *pretty = term_to_string(saved, 1);
            char *name = trim_in_place(input);
            output_printf("Saved %s %s %s\n", name, reduced_first ? "<-" : "=", pretty);
            if (reduced_first && stopped_early) {
                output_printf("Stopped after %d steps; term may not have a normal form.\n",
                              settings.max_steps);
            }
            free(pretty);
            continue;
        }

        char err[256];
        int step_limit = 0;
        if (!parse_step_suffix(input, &step_limit, err, sizeof err)) {
            output_printf("Parse error: %s\n", err);
            continue;
        }

        if (*input == '\0') {
            if (!session.current) {
                output_printf("No gradual reduction to continue.\n");
                continue;
            }

            Term *current = term_clone(session.current);
            int steps = reduce_steps_and_print(&current, step_limit, &lines,
                                               env, settings.eta_enabled);
            if (steps == 0) {
                output_printf("Already in normal form.\n");
            }
            step_session_set(&session, current);
            replace_term(&last_output, current);
            env_recompute_normals(env, last_output, settings.max_steps,
                                  settings.eta_enabled);
            term_free(current);
            continue;
        }

        Term *parsed = parse_lambda(input, err, sizeof err);
        if (!parsed) {
            output_printf("Parse error: %s\n", err);
            continue;
        }

        Term *new_last = NULL;
        if (evaluate_and_print(parsed, env, last_output, &lines,
                               settings.max_steps, step_limit,
                               settings.eta_enabled, &new_last)) {
            if (step_limit) {
                step_session_set(&session, new_last);
            } else {
                step_session_clear(&session);
            }
            replace_term(&last_output, new_last);
            env_recompute_normals(env, last_output, settings.max_steps,
                                  settings.eta_enabled);
        }
        term_free(new_last);
        term_free(parsed);
    }

    term_free(last_output);
    step_session_clear(&session);
    line_results_free(&lines);
    history_free(&history);
    output_free(&output);
    active_output = NULL;
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
