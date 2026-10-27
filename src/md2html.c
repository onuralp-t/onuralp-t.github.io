/*
 * mdhtml -- a dependency-free GitHub-flavored Markdown to HTML converter.
 *
 * Build:  gcc -std=c23 -O2 -Wall -Wextra -Wpedantic main.c -o mdhtml
 * Usage:  mdhtml post.md -o post.html
 *
 * The whole program lives in this file. It reads one Markdown document into
 * memory, normalizes it, parses blocks and inlines with arena allocation, and
 * renders a semantic HTML fragment (or a full document with --full).
 */
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#error "mdhtml requires C23; compile with -std=c23"
#endif

#if defined(MD_COVERAGE) && !defined(NDEBUG)
#define NDEBUG 1
#endif

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#endif

#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define MDHTML_VERSION "0.1.0"

/* ------------------------------------------------------------------ */
/* Test / hardening build modes                                        */
/* ------------------------------------------------------------------ */

/*
 * Release builds (NDEBUG set, no MD_* switches) keep only the production
 * logic. Test/debug/fuzz builds enable invariants; coverage builds remove
 * assertions and compile defensive ALWAYS/NEVER checks to constants, the
 * same technique SQLite uses so that defensive code does not pollute the
 * MC/DC measurement.
 */
#if !defined(MD_INVARIANTS) && !defined(NDEBUG) && !defined(MD_COVERAGE)
#define MD_INVARIANTS 1
#endif
#if defined(MD_COVERAGE)
#define MD_FAULT_INJECT 1
#endif
#if !defined(MD_FAULT_INJECT) && defined(MD_TEST)
#define MD_FAULT_INJECT 1
#endif

#if defined(MD_FAULT_INJECT) && MD_FAULT_INJECT
#define MD_FAULT_ENABLED 1
#else
#define MD_FAULT_ENABLED 0
#endif

[[maybe_unused, noreturn]] static void md_invariant_fail(const char *expr, const char *file, int line) {
    fprintf(stderr, "mdhtml invariant failed: %s (%s:%d)\n", expr, file, line);
    fflush(stderr);
    abort();
}

#if defined(MD_INVARIANTS) && MD_INVARIANTS
[[noreturn]] static void md_invariant_fail(const char *expr, const char *file, int line);
[[maybe_unused]] static int md_always_check(int x, const char *expr, const char *file, int line) {
    if (!x) md_invariant_fail(expr, file, line);
    return 1;
}
static int md_never_check(int x, const char *expr, const char *file, int line) {
    if (x) md_invariant_fail(expr, file, line);
    return 0;
}
#define MD_ASSERT(x) assert(x)
#define MD_ALWAYS(x) md_always_check(!!(x), #x, __FILE__, __LINE__)
#define MD_NEVER(x) md_never_check(!!(x), #x, __FILE__, __LINE__)
#else
#define MD_ASSERT(x) ((void)0)
#if defined(MD_COVERAGE)
#define MD_ALWAYS(x) (1)
#define MD_NEVER(x) (0)
#else
#define MD_ALWAYS(x) (x)
#define MD_NEVER(x) (x)
#endif
#endif

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

static constexpr size_t kArenaChunk = 64 * 1'024;
static constexpr size_t kOutInit = 512;
static constexpr int kMaxContainerDepth = 32;
static constexpr int kMaxInlineDepth = 64;
static constexpr size_t kMaxTableCols = 64;
[[maybe_unused]] static constexpr size_t kMaxValidateNodes = 4 * 1'000'000;

/* ------------------------------------------------------------------ */
/* Deterministic allocation fault injection (test/coverage builds)     */
/* ------------------------------------------------------------------ */

#if MD_FAULT_ENABLED
static uint64_t md_fault_after;      /* 1-based allocation number, 0 = disabled */
static bool md_fault_persistent;
static uint64_t md_fault_seen;

static bool md_fault_should_fail(void) {
    if (md_fault_after == 0) return false;
    md_fault_seen++;
    if (md_fault_seen >= md_fault_after) {
        if (!md_fault_persistent) md_fault_after = 0;
        return true;
    }
    return false;
}

[[maybe_unused]] static uint64_t md_fault_seen_count(void) {
    return md_fault_seen;
}
[[maybe_unused]] static void md_fault_reset(void) {
    md_fault_after = 0;
    md_fault_persistent = false;
    md_fault_seen = 0;
}

[[maybe_unused]] static void md_fault_set(uint64_t after, bool persistent) {
    md_fault_after = after;
    md_fault_persistent = persistent;
    md_fault_seen = 0;
}
#endif

/* ------------------------------------------------------------------ */
/* ASCII helpers (locale independent, byte oriented)                   */
/* ------------------------------------------------------------------ */

static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_alnum(unsigned char c) { return is_alpha(c) || is_digit(c); }
static bool is_hex(unsigned char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static bool is_sp_tab(unsigned char c) { return c == ' ' || c == '\t'; }
static bool is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
static bool is_punct(unsigned char c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}
static unsigned char to_lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

/* ------------------------------------------------------------------ */
/* Options and results                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    bool full_document;   /* wrap in <!DOCTYPE html> ... */
    bool allow_html;      /* pass raw HTML through instead of escaping */
    bool heading_ids;     /* emit id="..." on headings */
    bool hardbreaks;      /* render soft breaks as <br> */
    const char *title;    /* explicit full-document title, or nullptr */
    const char *title_fallback; /* used when no title and no H1, or nullptr */
    const char *lang;     /* full-document lang attribute, or nullptr for "en" */
} md_options;

typedef struct {
    char *data;
    size_t len;
} md_html;

typedef struct {
    int code;
    char message[192];
} md_error;

static void md_options_default(md_options *o) {
    *o = (md_options){
        .full_document = false,
        .allow_html = false,
        .heading_ids = true,
        .hardbreaks = false,
        .title = nullptr,
        .title_fallback = nullptr,
        .lang = nullptr,
    };
}

/* ------------------------------------------------------------------ */
/* Arena: bump allocator, OOM jumps back to md_render                  */
/* ------------------------------------------------------------------ */

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t used;
    size_t cap;
    max_align_t aligner;
    unsigned char data[];
} ArenaChunk;

static_assert(offsetof(ArenaChunk, data) % alignof(max_align_t) == 0,
              "arena payload must stay max-aligned");

typedef struct {
    ArenaChunk *head;
    size_t chunk_size;
    jmp_buf *oom;
} Arena;

static void arena_oom(Arena *a) {
    longjmp(*a->oom, 1);
}

static void arena_init(Arena *a, jmp_buf *oom, size_t chunk_size) {
    a->head = nullptr;
    a->chunk_size = chunk_size;
    a->oom = oom;
}

static void arena_free(Arena *a) {
    ArenaChunk *c = a->head;
    while (c) {
        ArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    a->head = nullptr;
}

static void arena_validate(const Arena *a) {
#if MD_INVARIANTS
    MD_ASSERT(a != nullptr);
    MD_ASSERT(a->oom != nullptr);
    size_t align = alignof(max_align_t);
    for (const ArenaChunk *c = a->head; c; c = c->next) {
        MD_ASSERT(c->used <= c->cap);
        MD_ASSERT(c->cap > 0);
        MD_ASSERT(((uintptr_t)c->data % align) == 0);
        MD_ASSERT(c->next != c);
    }
#else
    (void)a;
#endif
}

static void *arena_alloc(Arena *a, size_t n) {
    MD_ASSERT(a != nullptr);
    const size_t align = alignof(max_align_t);
    if (n == 0) n = 1;
    if (n > SIZE_MAX - align) arena_oom(a);
    n = (n + align - 1) & ~(align - 1);
    MD_ASSERT(n % align == 0);
    ArenaChunk *c = a->head;
    MD_ASSERT(!c || c->used <= c->cap);
    if (!c || c->cap - c->used < n) {
        size_t cap = a->chunk_size;
        if (cap < n) cap = n;
        if (cap > SIZE_MAX - sizeof(ArenaChunk)) arena_oom(a);
#if MD_FAULT_ENABLED
        if (md_fault_should_fail()) arena_oom(a);
#endif
        c = malloc(sizeof(ArenaChunk) + cap);
        if (!c) arena_oom(a);
        c->next = a->head;
        c->used = 0;
        c->cap = cap;
        a->head = c;
    }
    MD_ASSERT(c != nullptr);
    MD_ASSERT(c->used <= c->cap);
    MD_ASSERT(c->cap - c->used >= n);
    void *p = c->data + c->used;
    MD_ASSERT(((uintptr_t)p % align) == 0);
    c->used += n;
    MD_ASSERT(c->used <= c->cap);
    MD_ASSERT((unsigned char *)p >= c->data);
    MD_ASSERT((unsigned char *)p + n <= c->data + c->cap);
    return p;
}

static void *arena_alloc0(Arena *a, size_t n) {
    void *p = arena_alloc(a, n);
    memset(p, 0, n);
    return p;
}

static char *arena_strndup(Arena *a, const char *s, size_t n) {
    MD_ASSERT(s != nullptr || n == 0);
    char *p = arena_alloc(a, n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* Copy while removing backslash escapes of ASCII punctuation. */
static char *arena_unescape(Arena *a, const char *s, size_t n, size_t *out_len) {
    MD_ASSERT(s != nullptr || n == 0);
    MD_ASSERT(out_len != nullptr);
    char *p = arena_alloc(a, n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && is_punct((unsigned char)s[i + 1])) {
            p[j++] = s[++i];
        } else {
            p[j++] = s[i];
        }
    }
    MD_ASSERT(j <= n);
    p[j] = '\0';
    *out_len = j;
    return p;
}

/* ------------------------------------------------------------------ */
/* Output buffer                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    Arena *arena;
    char *data;
    size_t len;
    size_t cap;
} Out;

static void out_validate(const Out *o) {
#if MD_INVARIANTS
    MD_ASSERT(o != nullptr);
    MD_ASSERT(o->arena != nullptr);
    MD_ASSERT(o->len <= o->cap);
    MD_ASSERT(o->cap == 0 ? o->data == nullptr : o->data != nullptr);
#else
    (void)o;
#endif
}

static void out_init(Out *o, Arena *a) {
    MD_ASSERT(o != nullptr);
    MD_ASSERT(a != nullptr);
    o->arena = a;
    o->data = nullptr;
    o->len = 0;
    o->cap = 0;
    out_validate(o);
}

static void out_reserve(Out *o, size_t extra) {
    out_validate(o);
    if (extra > SIZE_MAX - o->len) arena_oom(o->arena);
    size_t need = o->len + extra;
    if (need <= o->cap) return;
    size_t cap = o->cap ? o->cap : kOutInit;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    MD_ASSERT(cap >= need);
    char *p = arena_alloc(o->arena, cap);
    if (o->len) memcpy(p, o->data, o->len);
    o->data = p;
    o->cap = cap;
    out_validate(o);
}

static void out_mem(Out *o, const char *s, size_t n) {
    out_validate(o);
    MD_ASSERT(s != nullptr || n == 0);
    if (!n) return;
    out_reserve(o, n);
    MD_ASSERT(o->len + n <= o->cap);
    memcpy(o->data + o->len, s, n);
    o->len += n;
    out_validate(o);
}

static void out_ch(Out *o, char c) {
    out_reserve(o, 1);
    MD_ASSERT(o->len < o->cap);
    o->data[o->len++] = c;
    out_validate(o);
}

static void out_lit(Out *o, const char *s) {
    MD_ASSERT(s != nullptr);
    out_mem(o, s, strlen(s));
}

static void out_count(Out *o, unsigned long v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%lu", v);
    MD_ASSERT(n > 0);
    if (n > 0) out_mem(o, tmp, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Escaping                                                            */
/* ------------------------------------------------------------------ */

static size_t entity_len(const char *s, size_t n) {
    if (n < 3 || s[0] != '&') return 0;
    size_t i = 1;
    if (s[i] == '#') {
        i++;
        MD_ASSERT(i < n);
        if (s[i] == 'x' || s[i] == 'X') {
            i++;
            size_t digits = 0;
            while (i < n && is_hex((unsigned char)s[i])) {
                i++;
                digits++;
            }
            if (digits == 0 || digits > 6) return 0;
        } else {
            size_t digits = 0;
            while (i < n && is_digit((unsigned char)s[i])) {
                i++;
                digits++;
            }
            if (digits == 0 || digits > 7) return 0;
        }
    } else {
        if (!is_alpha((unsigned char)s[i])) return 0;
        size_t start = i;
        while (i < n && is_alnum((unsigned char)s[i])) i++;
        if (i - start > 32) return 0;
    }
    if (i < n && s[i] == ';') return i + 1;
    return 0;
}

static void out_replacement(Out *o) {
    out_mem(o, "\xEF\xBF\xBD", 3);
}

/* Text: escape & < >, preserve valid entity references, replace NUL. */
static void out_text(Out *o, const char *s, size_t n) {
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '&') {
            size_t ent = entity_len(s + i, n - i);
            if (ent) {
                out_mem(o, s + run, i - run);
                out_mem(o, s + i, ent);
                i += ent - 1;
                run = i + 1;
            } else {
                out_mem(o, s + run, i - run);
                out_lit(o, "&amp;");
                run = i + 1;
            }
        } else if (c == '<') {
            out_mem(o, s + run, i - run);
            out_lit(o, "&lt;");
            run = i + 1;
        } else if (c == '>') {
            out_mem(o, s + run, i - run);
            out_lit(o, "&gt;");
            run = i + 1;
        } else if (c == 0) {
            out_mem(o, s + run, i - run);
            out_replacement(o);
            run = i + 1;
        }
    }
    out_mem(o, s + run, n - run);
}

static void out_code(Out *o, const char *s, size_t n) {
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *rep = nullptr;
        if (c == '&') rep = "&amp;";
        else if (c == '<') rep = "&lt;";
        else if (c == '>') rep = "&gt;";
        else if (c == 0) {
            out_mem(o, s + run, i - run);
            out_replacement(o);
            run = i + 1;
            continue;
        }
        if (rep) {
            out_mem(o, s + run, i - run);
            out_lit(o, rep);
            run = i + 1;
        }
    }
    out_mem(o, s + run, n - run);
}

static void out_attr(Out *o, const char *s, size_t n) {
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *rep = nullptr;
        if (c == '&') rep = "&amp;";
        else if (c == '"') rep = "&quot;";
        else if (c == '<') rep = "&lt;";
        else if (c == '>') rep = "&gt;";
        else if (c == 0) {
            out_mem(o, s + run, i - run);
            out_replacement(o);
            run = i + 1;
            continue;
        }
        if (rep) {
            out_mem(o, s + run, i - run);
            out_lit(o, rep);
            run = i + 1;
        }
    }
    out_mem(o, s + run, n - run);
}

/* ------------------------------------------------------------------ */
/* URL safety                                                          */
/* ------------------------------------------------------------------ */

static bool starts_with_ci(const char *s, size_t n, const char *pfx) {
    size_t m = strlen(pfx);
    if (n < m) return false;
    for (size_t i = 0; i < m; i++) {
        if (to_lower((unsigned char)s[i]) != (unsigned char)pfx[i]) return false;
    }
    return true;
}

static bool url_is_safe(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && (unsigned char)s[i] <= 0x20) i++;
    char scheme[16];
    size_t k = 0;
    while (i < n && k < sizeof scheme - 1) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20) {
            i++;
            continue;
        }
        if (c == ':') break;
        if (!is_alnum(c) && c != '+' && c != '-' && c != '.') break;
        scheme[k++] = (char)to_lower(c);
        i++;
    }
    scheme[k] = '\0';
    if (i >= n || s[i] != ':') return true;
    if (strcmp(scheme, "javascript") == 0 || strcmp(scheme, "vbscript") == 0) return false;
    if (strcmp(scheme, "data") != 0) return true;
    i++;
    while (i < n && (unsigned char)s[i] <= 0x20) i++;
    const char *mimes[] = {"image/png", "image/jpeg", "image/gif", "image/webp", "image/avif"};
    for (size_t m = 0; m < sizeof mimes / sizeof mimes[0]; m++) {
        size_t len = strlen(mimes[m]);
        if (n - i >= len && starts_with_ci(s + i, n - i, mimes[m])) {
            if (n - i == len) return true;
            unsigned char next = (unsigned char)s[i + len];
            if (next == ';' || next == ',' || next == ' ' || next == '\t') return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Spans and line helpers                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t off;
    size_t len;
} Span;

static bool span_blank(const char *buf, Span s) {
    for (size_t i = 0; i < s.len; i++) {
        if (!is_sp_tab((unsigned char)buf[s.off + i])) return false;
    }
    return true;
}

/* Return indentation in columns; *bytes gets the byte count consumed. */
static size_t span_indent(const char *buf, Span s, size_t *bytes) {
    size_t col = 0;
    size_t i = 0;
    while (i < s.len) {
        unsigned char c = (unsigned char)buf[s.off + i];
        if (c == ' ') {
            col++;
            i++;
        } else if (c == '\t') {
            col += 4 - (col % 4);
            i++;
        } else {
            break;
        }
    }
    if (bytes) *bytes = i;
    return col;
}

static Span span_trim(const char *buf, Span s) {
    size_t start = s.off;
    size_t end = s.off + s.len;
    while (start < end && is_sp_tab((unsigned char)buf[start])) start++;
    while (end > start && is_sp_tab((unsigned char)buf[end - 1])) end--;
    return (Span){start, end - start};
}

static Span span_strip_columns(const char *buf, Span s, size_t cols) {
    size_t i = 0;
    size_t col = 0;
    while (i < s.len && col < cols) {
        unsigned char c = (unsigned char)buf[s.off + i];
        if (c == ' ') {
            col++;
            i++;
        } else if (c == '\t') {
            size_t next = col + 4 - (col % 4);
            if (next > cols) break;
            col = next;
            i++;
        } else {
            break;
        }
    }
    return (Span){s.off + i, s.len - i};
}

static bool span_has_pipe(const char *buf, Span s) {
    size_t start = s.off;
    size_t end = s.off + s.len;
    for (size_t i = start; i < end; i++) {
        if (buf[i] != '|') continue;
        size_t back = 0;
        size_t j = i;
        while (j > start && buf[j - 1] == '\\') {
            back++;
            j--;
        }
        if ((back & 1) == 0) return true;
    }
    return false;
}

static bool is_escaped_at(const char *buf, size_t start, size_t idx) {
    size_t back = 0;
    while (idx > start && buf[idx - 1] == '\\') {
        back++;
        idx--;
    }
    return (back & 1) != 0;
}

/* ------------------------------------------------------------------ */
/* Line classifiers                                                    */
/* ------------------------------------------------------------------ */

static bool line_atx(const char *buf, Span s, int *level, Span *content) {
    size_t bytes = 0;
    if (span_indent(buf, s, &bytes) > 3) return false;
    size_t i = s.off + bytes;
    size_t end = s.off + s.len;
    size_t hashes = 0;
    while (i < end && buf[i] == '#') {
        hashes++;
        i++;
        if (hashes > 6) return false;
    }
    if (hashes == 0) return false;
    if (i < end && buf[i] != ' ' && buf[i] != '\t') return false;
    Span c = {i, end - i};
    c = span_trim(buf, c);
    size_t e2 = c.off + c.len;
    size_t j = e2;
    while (j > c.off && buf[j - 1] == '#') j--;
    if (j < e2) {
        if (j == c.off) {
            c.len = 0;
        } else if (buf[j - 1] == ' ' || buf[j - 1] == '\t') {
            c = span_trim(buf, (Span){c.off, j - c.off});
        }
    }
    *level = (int)hashes;
    *content = c;
    return true;
}

typedef struct {
    char ch;
    size_t run;
    size_t indent;
    Span info;
} FenceOpen;

static bool line_fence_open(const char *buf, Span s, FenceOpen *f) {
    size_t bytes = 0;
    size_t indent = span_indent(buf, s, &bytes);
    if (indent > 3) return false;
    size_t i = s.off + bytes;
    size_t end = s.off + s.len;
    if (i >= end) return false;
    char c = buf[i];
    if (c != '`' && c != '~') return false;
    size_t run = 0;
    while (i < end && buf[i] == c) {
        run++;
        i++;
    }
    if (run < 3) return false;
    Span info = span_trim(buf, (Span){i, end - i});
    if (c == '`') {
        for (size_t k = 0; k < info.len; k++) {
            if (buf[info.off + k] == '`') return false;
        }
    }
    f->ch = c;
    f->run = run;
    f->indent = indent;
    f->info = info;
    return true;
}

static bool line_fence_close(const char *buf, Span s, char ch, size_t open_run) {
    size_t bytes = 0;
    if (span_indent(buf, s, &bytes) > 3) return false;
    size_t i = s.off + bytes;
    size_t end = s.off + s.len;
    size_t run = 0;
    while (i < end && buf[i] == ch) {
        run++;
        i++;
    }
    if (run < open_run) return false;
    while (i < end) {
        if (!is_sp_tab((unsigned char)buf[i])) return false;
        i++;
    }
    return true;
}

static bool line_hr(const char *buf, Span s) {
    size_t bytes = 0;
    if (span_indent(buf, s, &bytes) > 3) return false;
    size_t i = s.off + bytes;
    size_t end = s.off + s.len;
    if (i >= end) return false;
    char c = buf[i];
    if (c != '-' && c != '*' && c != '_') return false;
    size_t count = 0;
    while (i < end) {
        char d = buf[i];
        if (d == c) {
            count++;
        } else if (!is_sp_tab((unsigned char)d)) {
            return false;
        }
        i++;
    }
    return count >= 3;
}

static bool line_setext(const char *buf, Span s, int *level) {
    size_t bytes = 0;
    if (span_indent(buf, s, &bytes) > 3) return false;
    Span t = span_trim(buf, (Span){s.off + bytes, s.len - bytes});
    if (t.len == 0) return false;
    char c = buf[t.off];
    if (c != '=' && c != '-') return false;
    for (size_t i = 0; i < t.len; i++) {
        if (buf[t.off + i] != c) return false;
    }
    *level = (c == '=') ? 1 : 2;
    return true;
}

static bool line_quote(const char *buf, Span s, Span *content) {
    size_t bytes = 0;
    if (span_indent(buf, s, &bytes) > 3) return false;
    size_t i = s.off + bytes;
    size_t end = s.off + s.len;
    if (i >= end || buf[i] != '>') return false;
    i++;
    if (i < end && (buf[i] == ' ' || buf[i] == '\t')) i++;
    *content = (Span){i, end - i};
    return true;
}

typedef struct {
    bool ordered;
    char delim;
    char bullet;
    long start;
    size_t marker_indent;
    size_t marker_width;
    size_t content_off;
    size_t content_indent;
    Span content;
} ListMarker;

static bool line_list_marker(const char *buf, Span s, ListMarker *m) {
    size_t bytes = 0;
    size_t indent = span_indent(buf, s, &bytes);
    if (indent > 3) return false;
    size_t i = s.off + bytes;
    size_t end = s.off + s.len;
    if (i >= end) return false;
    bool ordered = false;
    long start = 1;
    char delim = 0;
    char bullet = 0;
    size_t width = 0;
    if (buf[i] == '-' || buf[i] == '*' || buf[i] == '+') {
        bullet = buf[i];
        i++;
        width = 1;
    } else if (is_digit((unsigned char)buf[i])) {
        size_t digits = 0;
        long value = 0;
        while (i < end && is_digit((unsigned char)buf[i]) && digits < 9) {
            value = value * 10 + (buf[i] - '0');
            digits++;
            i++;
        }
        MD_ASSERT(digits > 0);
        if (i >= end) return false;
        if (buf[i] != '.' && buf[i] != ')') return false;
        delim = buf[i];
        ordered = true;
        start = value;
        width = digits + 1;
        i++;
    } else {
        return false;
    }
    if (i < end && !is_sp_tab((unsigned char)buf[i])) return false;
    size_t ws_start = i;
    size_t col = indent + width;
    size_t ws_cols = 0;
    while (i < end && is_sp_tab((unsigned char)buf[i])) {
        if (buf[i] == ' ') {
            ws_cols++;
            col++;
        } else {
            size_t adv = 4 - (col % 4);
            ws_cols += adv;
            col += adv;
        }
        i++;
    }
    size_t content_off;
    size_t content_indent;
    if (ws_cols == 0) {
        content_off = i;
        content_indent = indent + width + 1;
    } else if (ws_cols <= 4) {
        content_off = i;
        content_indent = indent + width + ws_cols;
    } else {
        content_off = ws_start + 1;
        content_indent = indent + width + 1;
    }
    m->ordered = ordered;
    m->delim = delim;
    m->bullet = bullet;
    m->start = start;
    m->marker_indent = indent;
    m->marker_width = width;
    m->content_off = content_off;
    m->content_indent = content_indent;
    m->content = (Span){content_off, end - content_off};
    return true;
}

static bool same_list(const ListMarker *a, const ListMarker *b) {
    if (a->ordered != b->ordered) return false;
    if (a->ordered) return a->delim == b->delim;
    return a->bullet == b->bullet;
}

static size_t split_cells(const char *buf, Span s, Span *out, size_t max) {
    size_t start = s.off;
    size_t end = s.off + s.len;
    while (start < end && is_sp_tab((unsigned char)buf[start])) start++;
    while (end > start && is_sp_tab((unsigned char)buf[end - 1])) end--;
    if (start < end && buf[start] == '|') start++;
    if (end > start && buf[end - 1] == '|' && !is_escaped_at(buf, s.off, end - 1)) end--;
    size_t count = 0;
    size_t cell_start = start;
    for (size_t i = start; i <= end; i++) {
        bool delim = (i == end) || (buf[i] == '|' && !is_escaped_at(buf, s.off, i));
        if (!delim) continue;
        size_t cs = cell_start;
        size_t ce = i;
        while (cs < ce && is_sp_tab((unsigned char)buf[cs])) cs++;
        while (ce > cs && is_sp_tab((unsigned char)buf[ce - 1])) ce--;
        if (count < max) out[count] = (Span){cs, ce - cs};
        count++;
        cell_start = i + 1;
    }
    return count;
}

static bool line_table_delim(const char *buf, Span s, unsigned char *aligns, size_t *ncols) {
    size_t bytes = 0;
    if (span_indent(buf, s, &bytes) > 3) return false;
    Span cells[65];
    size_t n = split_cells(buf, s, cells, 65);
    MD_ASSERT(n > 0);
    if (n > kMaxTableCols) return false;
    for (size_t c = 0; c < n; c++) {
        Span cell = cells[c];
        if (cell.len == 0) return false;
        bool left = false;
        bool right = false;
        size_t a = cell.off;
        size_t b = cell.off + cell.len;
        if (buf[a] == ':') {
            left = true;
            a++;
        }
        if (b > a && buf[b - 1] == ':') {
            right = true;
            b--;
        }
        if (a >= b) return false;
        for (size_t i = a; i < b; i++) {
            if (buf[i] != '-') return false;
        }
        aligns[c] = left && right ? 2 : left ? 1 : right ? 3 : 0;
    }
    *ncols = n;
    return true;
}

static const char *const kBlockTags[] = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body",
    "caption", "center", "col", "colgroup", "dd", "details", "dialog", "dir",
    "div", "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form",
    "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hr", "html",
    "iframe", "legend", "li", "link", "main", "menu", "menuitem", "nav",
    "ol", "p", "pre", "script", "section", "style", "summary", "table",
    "tbody", "td", "tfoot", "th", "thead", "title", "tr", "track", "ul",
    "video", "audio", "canvas", "svg", "math", "object", "embed", "source",
};

static bool line_html_block_start(const char *buf, Span s) {
    size_t bytes = 0;
    if (span_indent(buf, s, &bytes) > 3) return false;
    size_t i = s.off + bytes;
    size_t end = s.off + s.len;
    if (i >= end || buf[i] != '<') return false;
    if (end - i >= 4 && memcmp(buf + i, "<!--", 4) == 0) return true;
    if (end - i >= 9 && memcmp(buf + i, "<![CDATA[", 9) == 0) return true;
    if (i + 1 < end && buf[i + 1] == '?') return true;
    if (i + 2 < end && buf[i + 1] == '!' && is_alpha((unsigned char)buf[i + 2])) return true;
    size_t j = i + 1;
    if (j < end && buf[j] == '/') j++;
    if (j >= end || !is_alpha((unsigned char)buf[j])) return false;
    size_t name = j;
    while (j < end && (is_alnum((unsigned char)buf[j]) || buf[j] == '-')) j++;
    size_t name_len = j - name;
    if (name_len == 0) return false;
    if (j < end) {
        char c = buf[j];
        if (c != ' ' && c != '\t' && c != '>' && c != '/') return false;
    }
    for (size_t k = 0; k < sizeof kBlockTags / sizeof kBlockTags[0]; k++) {
        if (strlen(kBlockTags[k]) == name_len && memcmp(kBlockTags[k], buf + name, name_len) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Reference definitions                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char *label;
    size_t label_len;
    char *url;
    size_t url_len;
    char *title;
    size_t title_len;
} Ref;

typedef struct {
    Ref *slots;
    size_t cap;
    size_t count;
    Arena *arena;
} Refs;

static uint64_t hash_bytes(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void refs_init(Refs *r, Arena *a) {
    MD_ASSERT(r != nullptr);
    MD_ASSERT(a != nullptr);
    r->slots = nullptr;
    r->cap = 0;
    r->count = 0;
    r->arena = a;
}

static void refs_grow(Refs *r) {
    MD_ASSERT(r != nullptr);
    MD_ASSERT(r->arena != nullptr);
    size_t cap = r->cap ? r->cap * 2 : 32;
    MD_ASSERT((cap & (cap - 1)) == 0);
    Ref *slots = arena_alloc0(r->arena, cap * sizeof *slots);
    for (size_t i = 0; i < r->cap; i++) {
        Ref *old = &r->slots[i];
        if (!old->label) continue;
        MD_ASSERT(old->label_len > 0);
        MD_ASSERT(old->url != nullptr);
        size_t idx = (size_t)(hash_bytes(old->label, old->label_len) & (cap - 1));
        while (slots[idx].label) idx = (idx + 1) & (cap - 1);
        slots[idx] = *old;
    }
    r->slots = slots;
    r->cap = cap;
    MD_ASSERT(r->count < r->cap);
}

static void refs_put(Refs *r, const Ref *ref) {
    MD_ASSERT(r != nullptr);
    MD_ASSERT(ref != nullptr);
    MD_ASSERT(ref->label != nullptr);
    MD_ASSERT(ref->label_len > 0);
    MD_ASSERT(ref->url != nullptr);
    if (r->cap == 0 || (r->count + 1) * 4 >= r->cap * 3) refs_grow(r);
    MD_ASSERT((r->cap & (r->cap - 1)) == 0);
    size_t idx = (size_t)(hash_bytes(ref->label, ref->label_len) & (r->cap - 1));
    while (r->slots[idx].label) {
        Ref *cur = &r->slots[idx];
        MD_ASSERT(cur->label_len > 0);
        if (cur->label_len == ref->label_len &&
            memcmp(cur->label, ref->label, ref->label_len) == 0) {
            return;
        }
        idx = (idx + 1) & (r->cap - 1);
    }
    r->slots[idx] = *ref;
    r->count++;
    MD_ASSERT(r->count <= r->cap);
}

static const Ref *refs_get(const Refs *r, const char *label, size_t len) {
    MD_ASSERT(r != nullptr);
    MD_ASSERT(label != nullptr || len == 0);
    if (r->cap == 0) return nullptr;
    MD_ASSERT((r->cap & (r->cap - 1)) == 0);
    MD_ASSERT(r->count <= r->cap);
    size_t idx = (size_t)(hash_bytes(label, len) & (r->cap - 1));
    for (;;) {
        const Ref *cur = &r->slots[idx];
        if (!cur->label) return nullptr;
        if (cur->label_len == len && memcmp(cur->label, label, len) == 0) return cur;
        idx = (idx + 1) & (r->cap - 1);
    }
}

static char *normalize_label(Arena *a, const char *s, size_t n, size_t *out_len) {
    MD_ASSERT(a != nullptr);
    MD_ASSERT(s != nullptr || n == 0);
    MD_ASSERT(out_len != nullptr);
    char *buf = arena_alloc(a, n + 1);
    size_t j = 0;
    bool started = false;
    bool pending_space = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < n && is_punct((unsigned char)s[i + 1])) {
            c = (unsigned char)s[++i];
        }
        if (is_ws(c)) {
            if (started) pending_space = true;
            continue;
        }
        if (pending_space) {
            buf[j++] = ' ';
            pending_space = false;
        }
        buf[j++] = (char)to_lower(c);
        started = true;
    }
    MD_ASSERT(j <= n);
    buf[j] = '\0';
    *out_len = j;
    return buf;
}

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */

typedef enum : unsigned char {
    B_DOCUMENT,
    B_HEADING,
    B_PARAGRAPH,
    B_CODE,
    B_HTML_BLOCK,
    B_BLOCKQUOTE,
    B_LIST,
    B_ITEM,
    B_THEMATIC_BREAK,
    B_TABLE,
    B_TABLE_ROW,
    B_TABLE_CELL,
} BlkKind;

typedef enum : unsigned char {
    I_TEXT,
    I_EMPH,
    I_STRONG,
    I_STRIKE,
    I_CODE,
    I_LINK,
    I_IMAGE,
    I_SOFTBREAK,
    I_HARDBREAK,
    I_HTML,
} InlKind;

typedef struct Inline Inline;
struct Inline {
    InlKind kind;
    Inline *next;
    Inline *prev;
    Inline *first;
    Inline *last;
    char *text;
    size_t text_len;
    char *url;
    size_t url_len;
    char *title;
    size_t title_len;
};

typedef struct {
    Inline *first;
    Inline *last;
} InlList;

typedef struct Block Block;
struct Block {
    BlkKind kind;
    Block *next;
    Block *prev;
    Block *parent;
    Block *first;
    Block *last;
    char *text;
    size_t text_len;
    char *info;
    size_t info_len;
    Inline *inl_first;
    Inline *inl_last;
    int level;
    long start;
    bool ordered;
    bool tight;
    bool task;
    bool checked;
    bool header;
    unsigned char align;
    unsigned char *aligns;
    size_t ncols;
};

typedef struct {
    const char *buf;
    size_t len;
    Arena *arena;
    Refs refs;
    bool allow_html;
    bool heading_ids;
    bool hardbreaks;
} Parser;

typedef struct {
    Span *v;
    size_t n;
    size_t cap;
} SpanVec;

static void md_validate_span(const Parser *p, Span s, const char *what);

static void sv_push(Parser *p, SpanVec *sv, Span s) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(sv != nullptr);
    MD_ASSERT(s.off <= p->len);
    MD_ASSERT(s.len <= p->len - s.off);
    if (sv->n == sv->cap) {
        size_t cap = sv->cap ? sv->cap * 2 : 8;
        if (cap > SIZE_MAX / sizeof(Span)) arena_oom(p->arena);
        Span *v = arena_alloc(p->arena, cap * sizeof *v);
        if (sv->n) memcpy(v, sv->v, sv->n * sizeof *v);
        sv->v = v;
        sv->cap = cap;
    }
    MD_ASSERT(sv->n < sv->cap);
    sv->v[sv->n++] = s;
}

static Block *blk_new(Parser *p, BlkKind kind) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(p->arena != nullptr);
    MD_ASSERT((unsigned)kind <= (unsigned)B_TABLE_CELL);
    Block *b = arena_alloc0(p->arena, sizeof *b);
    b->kind = kind;
    return b;
}

static void blk_append(Block *parent, Block *child) {
    MD_ASSERT(parent != nullptr);
    MD_ASSERT(child != nullptr);
    MD_ASSERT(child != parent);
    MD_ASSERT(child->parent == nullptr);
    MD_ASSERT(child->prev == nullptr);
    MD_ASSERT(child->next == nullptr);
    MD_ASSERT((parent->last == nullptr) == (parent->first == nullptr));
    child->parent = parent;
    child->prev = parent->last;
    child->next = nullptr;
    if (parent->last) {
        MD_ASSERT(parent->last->next == nullptr);
        parent->last->next = child;
    } else {
        parent->first = child;
    }
    parent->last = child;
    MD_ASSERT(parent->last == child);
    if (child->prev) MD_ASSERT(child->prev->next == child);
}

static Inline *inl_new(Arena *a, InlKind kind) {
    MD_ASSERT(a != nullptr);
    MD_ASSERT((unsigned)kind <= (unsigned)I_HTML);
    Inline *n = arena_alloc0(a, sizeof *n);
    n->kind = kind;
    return n;
}

static void inl_append(InlList *l, Inline *n) {
    MD_ASSERT(l != nullptr);
    MD_ASSERT(n != nullptr);
    MD_ASSERT(n->next == nullptr);
    MD_ASSERT(n->prev == nullptr);
    MD_ASSERT((l->last == nullptr) == (l->first == nullptr));
    n->prev = l->last;
    n->next = nullptr;
    if (l->last) {
        MD_ASSERT(l->last->next == nullptr);
        l->last->next = n;
    } else {
        l->first = n;
    }
    l->last = n;
    MD_ASSERT(l->last == n);
}

static void inl_remove(InlList *l, Inline *n) {
    MD_ASSERT(l != nullptr);
    MD_ASSERT(n != nullptr);
    MD_ASSERT(l->first != nullptr && l->last != nullptr);
    MD_ASSERT(n == l->first || n->prev != nullptr);
    if (n->prev) n->prev->next = n->next;
    else l->first = n->next;
    if (n->next) n->next->prev = n->prev;
    else l->last = n->prev;
    n->next = nullptr;
    n->prev = nullptr;
    MD_ASSERT((l->first == nullptr) == (l->last == nullptr));
}

/* ------------------------------------------------------------------ */
/* Block parser helpers                                                */
/* ------------------------------------------------------------------ */

static void parse_into(Parser *p, Span *lines, size_t n, Block *parent, int depth);
static bool parse_ref_definition(Parser *p, Span line, Ref *out);

static char *join_paragraph_text(Parser *p, Span *lines, size_t a, size_t b, size_t *out_len) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(lines != nullptr || a == b);
    MD_ASSERT(a <= b);
    MD_ASSERT(out_len != nullptr);
    size_t total = 0;
    for (size_t i = a; i < b; i++) {
        Span s = lines[i];
        MD_ASSERT(s.len < SIZE_MAX - 1);
        md_validate_span(p, s, "paragraph line");
        size_t skip = 0;
        while (skip < s.len && p->buf[s.off + skip] == ' ' && skip < 3) skip++;
        MD_ASSERT(skip <= s.len);
        total += (s.len - skip) + 1;
    }
    char *t = arena_alloc(p->arena, total ? total : 1);
    size_t j = 0;
    for (size_t i = a; i < b; i++) {
        Span s = lines[i];
        size_t skip = 0;
        while (skip < s.len && p->buf[s.off + skip] == ' ' && skip < 3) skip++;
        for (size_t k = skip; k < s.len; k++) t[j++] = p->buf[s.off + k];
        if (i + 1 < b) t[j++] = '\n';
    }
    /* CommonMark strips final paragraph whitespace, but internal trailing
       spaces before a newline remain significant for hard line breaks. */
    while (j > 0 && (t[j - 1] == ' ' || t[j - 1] == '\t')) j--;
    MD_ASSERT(j <= total);
    *out_len = j;
    return t;
}

static char *join_code_text(Parser *p, Span *lines, size_t a, size_t b, size_t strip_cols,
                            size_t *out_len) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(lines != nullptr || a == b);
    MD_ASSERT(a <= b);
    MD_ASSERT(out_len != nullptr);
    size_t total = 0;
    for (size_t i = a; i < b; i++) {
        Span s = lines[i];
        md_validate_span(p, s, "code line");
        if (strip_cols) s = span_strip_columns(p->buf, s, strip_cols);
        MD_ASSERT(s.len < SIZE_MAX - 1);
        total += s.len + 1;
    }
    char *t = arena_alloc(p->arena, total ? total : 1);
    size_t j = 0;
    for (size_t i = a; i < b; i++) {
        Span s = lines[i];
        if (strip_cols) s = span_strip_columns(p->buf, s, strip_cols);
        for (size_t k = 0; k < s.len; k++) t[j++] = p->buf[s.off + k];
        t[j++] = '\n';
    }
    MD_ASSERT(j <= total);
    *out_len = j;
    return t;
}

static char *join_html_text(Parser *p, Span *lines, size_t a, size_t b, size_t *out_len) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(lines != nullptr || a == b);
    MD_ASSERT(a <= b);
    MD_ASSERT(out_len != nullptr);
    size_t total = 0;
    for (size_t i = a; i < b; i++) {
        md_validate_span(p, lines[i], "html line");
        MD_ASSERT(lines[i].len < SIZE_MAX - 1);
        total += lines[i].len + 1;
    }
    char *t = arena_alloc(p->arena, total ? total : 1);
    size_t j = 0;
    for (size_t i = a; i < b; i++) {
        Span s = lines[i];
        for (size_t k = 0; k < s.len; k++) t[j++] = p->buf[s.off + k];
        if (i + 1 < b) t[j++] = '\n';
    }
    MD_ASSERT(j <= total);
    *out_len = j;
    return t;
}

static bool line_interrupts(Parser *p, Span s) {
    MD_ASSERT(p != nullptr);
    md_validate_span(p, s, "interrupt line");
    int level;
    Span content;
    if (line_atx(p->buf, s, &level, &content)) return true;
    FenceOpen f;
    if (line_fence_open(p->buf, s, &f)) return true;
    if (line_hr(p->buf, s)) return true;
    if (line_quote(p->buf, s, &content)) return true;
    ListMarker m;
    if (line_list_marker(p->buf, s, &m)) return true;
    if (p->allow_html && line_html_block_start(p->buf, s)) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Block parsers                                                       */
/* ------------------------------------------------------------------ */

static size_t parse_fenced(Parser *p, Span *lines, size_t n, size_t i, Block *parent,
                           const FenceOpen *f) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(f != nullptr);
    MD_ASSERT(i < n);
    size_t j = i + 1;
    size_t start = j;
    while (j < n && !line_fence_close(p->buf, lines[j], f->ch, f->run)) j++;
    MD_ASSERT(j <= n);
    size_t text_len = 0;
    char *text = join_code_text(p, lines, start, j, f->indent, &text_len);
    Block *code = blk_new(p, B_CODE);
    code->text = text;
    code->text_len = text_len;
    if (f->info.len) {
        Span info = f->info;
        size_t k = 0;
        while (k < info.len && !is_sp_tab((unsigned char)p->buf[info.off + k])) k++;
        code->info = arena_strndup(p->arena, p->buf + info.off, k);
        code->info_len = k;
    }
    blk_append(parent, code);
    return j < n ? j + 1 : n;
}

static size_t parse_indented_code(Parser *p, Span *lines, size_t n, size_t i, Block *parent) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(i < n);
    size_t j = i;
    size_t end = i;
    while (j < n) {
        if (span_blank(p->buf, lines[j])) {
            j++;
            continue;
        }
        size_t bytes = 0;
        if (span_indent(p->buf, lines[j], &bytes) >= 4) {
            j++;
            end = j;
        } else {
            break;
        }
    }
    size_t text_len = 0;
    char *text = join_code_text(p, lines, i, end, 4, &text_len);
    Block *code = blk_new(p, B_CODE);
    code->text = text;
    code->text_len = text_len;
    blk_append(parent, code);
    return j;
}

static size_t parse_html_block(Parser *p, Span *lines, size_t n, size_t i, Block *parent) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(i < n);
    size_t j = i + 1;
    while (j < n && !span_blank(p->buf, lines[j])) j++;
    MD_ASSERT(j <= n);
    size_t text_len = 0;
    char *text = join_html_text(p, lines, i, j, &text_len);
    Block *html = blk_new(p, B_HTML_BLOCK);
    html->text = text;
    html->text_len = text_len;
    blk_append(parent, html);
    return j;
}

static size_t parse_quote(Parser *p, Span *lines, size_t n, size_t i, Block *parent, int depth) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(lines != nullptr);
    MD_ASSERT(i < n);
    MD_ASSERT(n - i <= SIZE_MAX / sizeof(Span));
    Span *inner = arena_alloc(p->arena, (n - i) * sizeof *inner);
    size_t m = 0;
    size_t j = i;
    bool paragraph_open = false;
    while (j < n) {
        MD_ASSERT(m < n - i);
        Span content;
        if (line_quote(p->buf, lines[j], &content)) {
            inner[m++] = content;
            paragraph_open = true;
            j++;
        } else if (span_blank(p->buf, lines[j])) {
            size_t k = j;
            while (k < n && span_blank(p->buf, lines[k])) k++;
            Span tmp;
            if (k < n && line_quote(p->buf, lines[k], &tmp)) {
                while (j < k) {
                    MD_ASSERT(m < n - i);
                    inner[m++] = (Span){lines[j].off, 0};
                    j++;
                }
                paragraph_open = false;
                continue;
            }
            break;
        } else if (paragraph_open && !line_interrupts(p, lines[j])) {
            inner[m++] = lines[j];
            j++;
        } else {
            break;
        }
    }
    MD_ASSERT(j <= n);
    MD_ASSERT(m <= n - i);
    Block *quote = blk_new(p, B_BLOCKQUOTE);
    parse_into(p, inner, m, quote, depth + 1);
    blk_append(parent, quote);
    return j;
}

static size_t parse_list(Parser *p, Span *lines, size_t n, size_t i, Block *parent, int depth) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(lines != nullptr);
    MD_ASSERT(i < n);
    ListMarker first;
    if (!line_list_marker(p->buf, lines[i], &first)) return i + 1;
    Block *list = blk_new(p, B_LIST);
    list->ordered = first.ordered;
    list->start = first.start;
    list->tight = true;
    size_t j = i;
    bool end_list = false;
    for (;;) {
        if (j >= n) break;
        if (end_list) break;
        ListMarker m;
        if (!line_list_marker(p->buf, lines[j], &m)) break;
        MD_ASSERT(same_list(&m, &first));
        MD_ASSERT(m.marker_indent == first.marker_indent);
        SpanVec item = {nullptr, 0, 0};
        sv_push(p, &item, m.content);
        bool item_loose = false;
        j++;
        while (j < n) {
            if (span_blank(p->buf, lines[j])) {
                size_t k = j;
                while (k < n && span_blank(p->buf, lines[k])) k++;
                if (k >= n) {
                    j = k;
                    break;
                }
                ListMarker nm;
                bool next_same = line_list_marker(p->buf, lines[k], &nm) &&
                                 same_list(&nm, &first) &&
                                 nm.marker_indent == first.marker_indent;
                if (next_same) {
                    j = k;
                    list->tight = false;
                    break;
                }
                size_t next_cols = span_indent(p->buf, lines[k], nullptr);
                if (next_cols >= m.content_indent) {
                    while (j < k) {
                        sv_push(p, &item, (Span){lines[j].off, 0});
                        j++;
                    }
                    item_loose = true;
                    continue;
                }
                j = k;
                end_list = true;
                break;
            }
            size_t cols = span_indent(p->buf, lines[j], nullptr);
            if (cols >= m.content_indent) {
                sv_push(p, &item, span_strip_columns(p->buf, lines[j], m.content_indent));
                j++;
                continue;
            }
            ListMarker nm;
            if (line_list_marker(p->buf, lines[j], &nm) && same_list(&nm, &first) &&
                nm.marker_indent == first.marker_indent) {
                break;
            }
            if (!line_interrupts(p, lines[j])) {
                sv_push(p, &item, lines[j]);
                j++;
                continue;
            }
            break;
        }
        while (item.n > 0 && span_blank(p->buf, item.v[item.n - 1])) item.n--;

        bool task = false;
        bool checked = false;
        if (item.n > 0) {
            Span s = item.v[0];
            size_t off = 0;
            while (off < s.len && is_sp_tab((unsigned char)p->buf[s.off + off])) off++;
            size_t p0 = s.off + off;
            if (s.len - off >= 3 && p->buf[p0] == '[' &&
                (p->buf[p0 + 1] == ' ' || p->buf[p0 + 1] == 'x' || p->buf[p0 + 1] == 'X') &&
                p->buf[p0 + 2] == ']' &&
                (s.len - off == 3 || is_sp_tab((unsigned char)p->buf[p0 + 3]))) {
                task = true;
                checked = p->buf[p0 + 1] != ' ';
                size_t skip = off + 3;
                if (skip < s.len) {
                    MD_ASSERT(is_sp_tab((unsigned char)p->buf[s.off + skip]));
                    skip++;
                }
                item.v[0] = (Span){s.off + skip, s.len - skip};
                if (item.n > 1 && item.v[0].len == 0) {
                    memmove(item.v, item.v + 1, (item.n - 1) * sizeof item.v[0]);
                    item.n--;
                }
            }
        }
        Block *li = blk_new(p, B_ITEM);
        li->task = task;
        li->checked = checked;
        li->tight = !item_loose;
        if (task) list->task = true;
        if (item_loose) list->tight = false;
        parse_into(p, item.v, item.n, li, depth + 1);
        blk_append(list, li);
        if (end_list) break;
        if (j >= n) break;
        ListMarker nm2;
        if (!line_list_marker(p->buf, lines[j], &nm2)) break;
        if (!same_list(&nm2, &first) || nm2.marker_indent != first.marker_indent) break;
    }
    blk_append(parent, list);
    return j;
}

static bool try_table(Parser *p, Span *lines, size_t n, size_t i, Block *parent, size_t *next) {
    if (i + 1 >= n) return false;
    unsigned char aligns[kMaxTableCols];
    size_t ncols = 0;
    if (!line_table_delim(p->buf, lines[i + 1], aligns, &ncols)) return false;
    Span header_cells[kMaxTableCols];
    size_t hcount = split_cells(p->buf, lines[i], header_cells, kMaxTableCols);
    MD_ASSERT(ncols > 0);
    if (hcount != ncols) return false;
    if (!span_has_pipe(p->buf, lines[i])) return false;

    Block *table = blk_new(p, B_TABLE);
    table->ncols = ncols;
    table->aligns = arena_alloc(p->arena, ncols);
    memcpy(table->aligns, aligns, ncols);

    Block *row = blk_new(p, B_TABLE_ROW);
    row->header = true;
    for (size_t c = 0; c < ncols; c++) {
        Block *cell = blk_new(p, B_TABLE_CELL);
        cell->text = (char *)p->buf + header_cells[c].off;
        cell->text_len = header_cells[c].len;
        cell->align = aligns[c];
        cell->header = true;
        blk_append(row, cell);
    }
    blk_append(table, row);

    size_t j = i + 2;
    while (j < n) {
        if (span_blank(p->buf, lines[j])) break;
        if (line_interrupts(p, lines[j])) break;
        if (ncols > 1 && !span_has_pipe(p->buf, lines[j])) break;
        Span cells[kMaxTableCols];
        size_t cc = split_cells(p->buf, lines[j], cells, kMaxTableCols);
        if (cc == 0) break;
        Block *body_row = blk_new(p, B_TABLE_ROW);
        for (size_t c = 0; c < ncols; c++) {
            Block *cell = blk_new(p, B_TABLE_CELL);
            Span cs = c < cc ? cells[c] : (Span){0, 0};
            cell->text = cs.len ? (char *)p->buf + cs.off : (char *)"";
            cell->text_len = cs.len;
            cell->align = aligns[c];
            blk_append(body_row, cell);
        }
        blk_append(table, body_row);
        j++;
    }
    blk_append(parent, table);
    *next = j;
    return true;
}

static size_t parse_paragraph(Parser *p, Span *lines, size_t n, size_t i, Block *parent) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(i < n);
    size_t end = i;
    while (end < n) {
        if (span_blank(p->buf, lines[end])) break;
        if (end > i) {
            int level;
            if (line_setext(p->buf, lines[end], &level)) {
                MD_ASSERT(level == 1 || level == 2);
                size_t len = 0;
                char *text = join_paragraph_text(p, lines, i, end, &len);
                Block *h = blk_new(p, B_HEADING);
                h->level = level;
                h->text = text;
                h->text_len = len;
                blk_append(parent, h);
                return end + 1;
            }
            if (line_interrupts(p, lines[end])) break;
        }
        end++;
    }
    MD_ASSERT(end > i && end <= n);
    size_t len = 0;
    char *text = join_paragraph_text(p, lines, i, end, &len);
    Block *para = blk_new(p, B_PARAGRAPH);
    para->text = text;
    para->text_len = len;
    blk_append(parent, para);
    return end;
}

static void parse_into(Parser *p, Span *lines, size_t n, Block *parent, int depth) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(parent != nullptr);
    MD_ASSERT(lines != nullptr || n == 0);
    MD_ASSERT(depth >= 0);
    MD_ASSERT(depth <= kMaxContainerDepth + 1);
    if (depth > kMaxContainerDepth) {
        if (n == 0) return;
        size_t len = 0;
        char *text = join_html_text(p, lines, 0, n, &len);
        Block *para = blk_new(p, B_PARAGRAPH);
        para->text = text;
        para->text_len = len;
        blk_append(parent, para);
        return;
    }
    size_t i = 0;
    while (i < n) {
        MD_ASSERT(i < n);
        MD_ASSERT(lines[i].off <= p->len);
        MD_ASSERT(lines[i].len <= p->len - lines[i].off);
        if (span_blank(p->buf, lines[i])) {
            i++;
            continue;
        }
        size_t bytes = 0;
        size_t indent = span_indent(p->buf, lines[i], &bytes);
        MD_ASSERT(bytes <= lines[i].len);
        if (indent >= 4) {
            size_t next = parse_indented_code(p, lines, n, i, parent);
            MD_ASSERT(next > i && next <= n);
            i = next;
            continue;
        }
        int level = 0;
        Span content;
        if (line_atx(p->buf, lines[i], &level, &content)) {
            MD_ASSERT(level >= 1 && level <= 6);
            Block *h = blk_new(p, B_HEADING);
            h->level = level;
            h->text = (char *)p->buf + content.off;
            h->text_len = content.len;
            blk_append(parent, h);
            i++;
            continue;
        }
        FenceOpen f;
        if (line_fence_open(p->buf, lines[i], &f)) {
            size_t next = parse_fenced(p, lines, n, i, parent, &f);
            MD_ASSERT(next > i && next <= n);
            i = next;
            continue;
        }
        if (line_hr(p->buf, lines[i])) {
            blk_append(parent, blk_new(p, B_THEMATIC_BREAK));
            i++;
            continue;
        }
        if (line_quote(p->buf, lines[i], &content)) {
            size_t next = parse_quote(p, lines, n, i, parent, depth);
            MD_ASSERT(next > i && next <= n);
            i = next;
            continue;
        }
        ListMarker lm;
        if (line_list_marker(p->buf, lines[i], &lm)) {
            size_t next = parse_list(p, lines, n, i, parent, depth);
            MD_ASSERT(next > i && next <= n);
            i = next;
            continue;
        }
        Ref ref;
        if (parse_ref_definition(p, lines[i], &ref)) {
            refs_put(&p->refs, &ref);
            i++;
            continue;
        }
        if (p->allow_html && line_html_block_start(p->buf, lines[i])) {
            size_t next = parse_html_block(p, lines, n, i, parent);
            MD_ASSERT(next > i && next <= n);
            i = next;
            continue;
        }
        size_t next = 0;
        if (try_table(p, lines, n, i, parent, &next)) {
            MD_ASSERT(next > i && next <= n);
            i = next;
            continue;
        }
        size_t end = parse_paragraph(p, lines, n, i, parent);
        MD_ASSERT(end > i && end <= n);
        i = end;
    }
}

/* ------------------------------------------------------------------ */
/* Inline parser                                                       */
/* ------------------------------------------------------------------ */

typedef struct Delim {
    struct Delim *prev;
    struct Delim *next;
    Inline *node;
    int len;
    char ch;
    bool can_open;
    bool can_close;
} Delim;

static void delim_unlink(Delim **top, Delim *d) {
    MD_ASSERT(top != nullptr);
    MD_ASSERT(d != nullptr);
    MD_ASSERT(d->prev != nullptr || d->next != nullptr || *top == d);
    if (d->prev) d->prev->next = d->next;
    if (d->next) d->next->prev = d->prev;
    if (*top == d) *top = d->prev;
    d->prev = nullptr;
    d->next = nullptr;
}

static void parse_inline_into(Parser *p, const char *s, size_t n, InlList *out, int depth,
                              bool allow_links, bool allow_images);

static size_t find_matching_bracket(const char *s, size_t n, size_t i) {
    MD_ASSERT(s != nullptr);
    MD_ASSERT(i < n);
    MD_ASSERT(s[i] == '[');
    int depth = 0;
    size_t j = i;
    while (j < n) {
        char c = s[j];
        if (c == '\\' && j + 1 < n) {
            j += 2;
            continue;
        }
        if (c == '`') {
            size_t run = 0;
            size_t k = j;
            while (k < n && s[k] == '`') {
                run++;
                k++;
            }
            size_t m = k;
            bool found = false;
            while (m < n) {
                if (s[m] == '`') {
                    size_t r = 0;
                    size_t q = m;
                    while (q < n && s[q] == '`') {
                        r++;
                        q++;
                    }
                    if (r == run) {
                        m = q;
                        found = true;
                        break;
                    }
                    m = q;
                } else {
                    m++;
                }
            }
            j = found ? m : k;
            continue;
        }
        if (c == '[') depth++;
        else if (c == ']') {
            depth--;
            if (depth == 0) return j;
        }
        j++;
    }
    return 0;
}

static void delimiter_flanking(const char *s, size_t n, size_t start, size_t len, bool *open,
                               bool *close) {
    MD_ASSERT(s != nullptr);
    MD_ASSERT(open != nullptr);
    MD_ASSERT(close != nullptr);
    MD_ASSERT(len > 0);
    MD_ASSERT(start < n);
    MD_ASSERT(len <= n - start);
    unsigned char before = start > 0 ? (unsigned char)s[start - 1] : (unsigned char)'\n';
    unsigned char after = start + len < n ? (unsigned char)s[start + len] : (unsigned char)'\n';
    bool before_ws = is_ws(before);
    bool after_ws = is_ws(after);
    bool before_punct = is_punct(before);
    bool after_punct = is_punct(after);
    bool left = !after_ws && (!after_punct || before_ws || before_punct);
    bool right = !before_ws && (!before_punct || after_ws || after_punct);
    char c = s[start];
    if (c == '_') {
        *open = left && (!right || before_punct);
        *close = right && (!left || after_punct);
    } else {
        *open = left;
        *close = right;
    }
}

static Inline *make_text(Arena *a, const char *s, size_t len) {
    MD_ASSERT(a != nullptr);
    MD_ASSERT(s != nullptr || len == 0);
    Inline *node = inl_new(a, I_TEXT);
    MD_ASSERT(node->kind == I_TEXT);
    node->text = (char *)s;
    node->text_len = len;
    return node;
}

static char *code_span_copy(Arena *a, const char *s, size_t n, size_t *out_len) {
    MD_ASSERT(a != nullptr);
    MD_ASSERT(s != nullptr || n == 0);
    MD_ASSERT(out_len != nullptr);
    char *buf = arena_alloc(a, n ? n : 1);
    size_t j = 0;
    bool all_space = true;
    for (size_t i = 0; i < n; i++) {
        char c = s[i] == '\n' ? ' ' : s[i];
        if (c != ' ') all_space = false;
        buf[j++] = c;
    }
    size_t start = 0;
    size_t end = j;
    if (j >= 2 && buf[0] == ' ' && buf[j - 1] == ' ' && !all_space) {
        start = 1;
        end = j - 1;
    }
    *out_len = end - start;
    return buf + start;
}

static bool parse_inline_dest(Parser *p, const char *s, size_t n, size_t i, size_t *after,
                              char **url, size_t *url_len, char **title, size_t *title_len) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(s != nullptr);
    MD_ASSERT(i < n);
    MD_ASSERT(s[i] == '(');
    MD_ASSERT(after != nullptr && url != nullptr && url_len != nullptr);
    MD_ASSERT(title != nullptr && title_len != nullptr);
    i++;
    while (i < n && is_sp_tab((unsigned char)s[i])) i++;
    char *u = nullptr;
    size_t ul = 0;
    if (i < n && s[i] == ')') {
        u = arena_strndup(p->arena, "", 0);
        *after = i + 1;
        *url = u;
        *url_len = 0;
        *title = nullptr;
        *title_len = 0;
        return true;
    }
    if (i < n && s[i] == '<') {
        size_t j = i + 1;
        size_t begin = j;
        while (j < n && s[j] != '>') {
            if (s[j] == '\\' && j + 1 < n) j += 2;
            else if (s[j] == '\n') return false;
            else j++;
        }
        if (j >= n) return false;
        u = arena_unescape(p->arena, s + begin, j - begin, &ul);
        i = j + 1;
    } else {
        size_t begin = i;
        size_t parens = 0;
        while (i < n) {
            char c = s[i];
            if (c == '\\' && i + 1 < n) {
                i += 2;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\n') break;
            if (c == '(') parens++;
            else if (c == ')') {
                if (parens == 0) break;
                parens--;
            } else if (c == '<') {
                return false;
            }
            i++;
        }
        if (parens != 0) return false;
        if (i == begin) return false;
        u = arena_unescape(p->arena, s + begin, i - begin, &ul);
    }
    while (i < n && is_sp_tab((unsigned char)s[i])) i++;
    char *t = nullptr;
    size_t tl = 0;
    if (i < n && s[i] == ')') {
        *after = i + 1;
        *url = u;
        *url_len = ul;
        *title = t;
        *title_len = tl;
        return true;
    }
    if (i < n && (s[i] == '"' || s[i] == '\'' || s[i] == '(')) {
        char open = s[i];
        char close = open == '(' ? ')' : open;
        i++;
        size_t begin = i;
        while (i < n && s[i] != close) {
            if (s[i] == '\\' && i + 1 < n) i += 2;
            else i++;
        }
        if (i >= n) return false;
        t = arena_unescape(p->arena, s + begin, i - begin, &tl);
        i++;
        while (i < n && is_sp_tab((unsigned char)s[i])) i++;
        if (i >= n || s[i] != ')') return false;
        *after = i + 1;
        *url = u;
        *url_len = ul;
        *title = t;
        *title_len = tl;
        return true;
    }
    return false;
}

static bool try_link(Parser *p, const char *s, size_t n, size_t i, bool image, InlList *out,
                     size_t *next, int depth, bool allow_links, bool allow_images) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(s != nullptr);
    MD_ASSERT(out != nullptr);
    MD_ASSERT(next != nullptr);
    MD_ASSERT(depth >= 0);
    if (i >= n || s[i] != '[') return false;
    size_t close = find_matching_bracket(s, n, i);
    if (close == 0) return false;
    size_t label_start = i + 1;
    size_t label_len = close - label_start;
    size_t pos = close + 1;
    char *url = nullptr;
    size_t url_len = 0;
    char *title = nullptr;
    size_t title_len = 0;
    size_t end = 0;
    bool ok = false;
    if (pos < n && s[pos] == '(') {
        ok = parse_inline_dest(p, s, n, pos, &end, &url, &url_len, &title, &title_len);
    } else {
        size_t text_label_len = 0;
        char *text_label = normalize_label(p->arena, s + label_start, label_len, &text_label_len);
        if (pos < n && s[pos] == '[') {
            size_t m = pos + 1;
            while (m < n && s[m] != ']') {
                if (s[m] == '\\' && m + 1 < n) m += 2;
                else m++;
            }
            if (m < n) {
                size_t rlab_len = m - (pos + 1);
                const char *rlab = s + pos + 1;
                size_t norm_len = 0;
                char *norm;
                if (rlab_len == 0) {
                    norm = text_label;
                    norm_len = text_label_len;
                } else {
                    norm = normalize_label(p->arena, rlab, rlab_len, &norm_len);
                }
                const Ref *ref = refs_get(&p->refs, norm, norm_len);
                if (ref) {
                    url = ref->url;
                    url_len = ref->url_len;
                    title = ref->title;
                    title_len = ref->title_len;
                    end = m + 1;
                    ok = true;
                }
            }
        }
        if (!ok) {
            const Ref *ref = refs_get(&p->refs, text_label, text_label_len);
            if (ref) {
                url = ref->url;
                url_len = ref->url_len;
                title = ref->title;
                title_len = ref->title_len;
                end = pos;
                ok = true;
            }
        }
    }
    if (!ok) return false;
    if (image && !allow_images) return false;
    if (!image && !allow_links) return false;
    InlList kids = {nullptr, nullptr};
    parse_inline_into(p, s + label_start, label_len, &kids, depth + 1, false, image ? false : true);
    Inline *node = inl_new(p->arena, image ? I_IMAGE : I_LINK);
    node->first = kids.first;
    node->last = kids.last;
    node->url = url;
    node->url_len = url_len;
    node->title = title;
    node->title_len = title_len;
    MD_ASSERT(url != nullptr);
    MD_ASSERT(end > i && end <= n);
    inl_append(out, node);
    *next = end;
    return true;
}

/* Returns end index of an inline HTML construct, or 0. */
static size_t scan_inline_html(const char *s, size_t n, size_t i) {
    MD_ASSERT(s != nullptr || n == 0);
    if (i >= n || s[i] != '<') return 0;
    if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
        for (size_t j = i + 4; j + 2 < n; j++) {
            if (s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>') return j + 3;
        }
        return 0;
    }
    if (i + 8 < n && memcmp(s + i, "<![CDATA[", 9) == 0) {
        for (size_t j = i + 9; j + 2 < n; j++) {
            if (s[j] == ']' && s[j + 1] == ']' && s[j + 2] == '>') return j + 3;
        }
        return 0;
    }
    if (i + 1 < n && s[i + 1] == '?') {
        for (size_t j = i + 2; j + 1 < n; j++) {
            if (s[j] == '?' && s[j + 1] == '>') return j + 2;
        }
        return 0;
    }
    if (i + 2 < n && s[i + 1] == '!' && (is_alpha((unsigned char)s[i + 2]))) {
        for (size_t j = i + 2; j < n; j++) {
            if (s[j] == '>') return j + 1;
        }
        return 0;
    }
    size_t j = i + 1;
    if (j < n && s[j] == '/') j++;
    if (j >= n || !is_alpha((unsigned char)s[j])) return 0;
    while (j < n && (is_alnum((unsigned char)s[j]) || s[j] == '-')) j++;
    while (j < n) {
        char c = s[j];
        if (c == '"' || c == '\'') {
            char q = c;
            j++;
            while (j < n && s[j] != q) j++;
            if (j >= n) return 0;
            j++;
            continue;
        }
        if (c == '>') return j + 1;
        if (c == '<') return 0;
        j++;
    }
    return 0;
}

static bool scan_email(const char *s, size_t n, size_t i, size_t *len) {
    MD_ASSERT(s != nullptr);
    MD_ASSERT(len != nullptr);
    MD_ASSERT(i < n);
    size_t j = i;
    while (j < n) {
        unsigned char c = (unsigned char)s[j];
        if (is_alnum(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-') j++;
        else break;
    }
    if (j >= n || s[j] != '@' || j == i) return false;
    size_t at = j;
    j++;
    bool saw_dot = false;
    while (j < n) {
        unsigned char c = (unsigned char)s[j];
        if (is_alnum(c) || c == '-') {
            j++;
        } else if (c == '.') {
            saw_dot = true;
            j++;
        } else {
            break;
        }
    }
    if (!saw_dot) return false;
    size_t k = j;
    while (MD_ALWAYS(k > i) && s[k - 1] == '.') k--;
    MD_ASSERT(k > i);
    if (k <= at + 1) return false;
    if (j < n && (s[j] == '_' || s[j] == '@')) return false;
    *len = k - i;
    return true;
}

static Inline *make_link(Arena *a, const char *label, size_t label_len, char *url,
                         size_t url_len) {
    MD_ASSERT(a != nullptr);
    MD_ASSERT(label != nullptr || label_len == 0);
    MD_ASSERT(url != nullptr);
    Inline *link = inl_new(a, I_LINK);
    link->url = url;
    link->url_len = url_len;
    Inline *text = make_text(a, label, label_len);
    link->first = text;
    link->last = text;
    return link;
}

static bool try_bare_autolink(Parser *p, const char *s, size_t n, size_t i, Inline **node,
                              size_t *next) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(s != nullptr);
    MD_ASSERT(node != nullptr);
    MD_ASSERT(next != nullptr);
    if (i >= n) return false;
    if (i > 0) {
        unsigned char prev = (unsigned char)s[i - 1];
        if (is_alnum(prev) || prev == '_' || prev == '@' || prev == '/' || prev == '.') return false;
    }
    size_t scheme_len = 0;
    if (n - i >= 7 && memcmp(s + i, "http://", 7) == 0) scheme_len = 7;
    else if (n - i >= 8 && memcmp(s + i, "https://", 8) == 0) scheme_len = 8;
    else if (n - i >= 4 && memcmp(s + i, "www.", 4) == 0) scheme_len = 4;
    if (scheme_len) {
        size_t j = i + scheme_len;
        if (j >= n) return false;
        while (j < n && !is_ws((unsigned char)s[j]) && s[j] != '<') j++;
        size_t end = j;
        while (end > i + scheme_len) {
            char c = s[end - 1];
            if (c == '?' || c == '!' || c == '.' || c == ',' || c == ':' || c == '*' ||
                c == '_' || c == '~' || c == '\'' || c == '"' || c == ';') {
                end--;
            } else {
                break;
            }
        }
        while (end > i + scheme_len && s[end - 1] == ')') {
            size_t open = 0;
            size_t close = 0;
            for (size_t k = i; k < end; k++) {
                if (s[k] == '(') open++;
                else if (s[k] == ')') close++;
            }
            if (close > open) end--;
            else break;
        }
        if (end <= i + scheme_len) return false;
        size_t label_len = end - i;
        if (scheme_len == 4) {
            size_t ul = 8 + label_len;
            char *u = arena_alloc(p->arena, ul + 1);
            memcpy(u, "https://", 8);
            memcpy(u + 8, s + i, label_len);
            u[ul] = '\0';
            *node = make_link(p->arena, s + i, label_len, u, ul);
        } else {
            char *u = arena_strndup(p->arena, s + i, label_len);
            *node = make_link(p->arena, s + i, label_len, u, label_len);
        }
        *next = end;
        return true;
    }
    if (!is_alnum((unsigned char)s[i])) return false;
    size_t elen = 0;
    if (!scan_email(s, n, i, &elen)) return false;
    size_t ul = 7 + elen;
    char *u = arena_alloc(p->arena, ul + 1);
    memcpy(u, "mailto:", 7);
    memcpy(u + 7, s + i, elen);
    u[ul] = '\0';
    *node = make_link(p->arena, s + i, elen, u, ul);
    *next = i + elen;
    return true;
}

static bool try_angle_autolink(Parser *p, const char *s, size_t n, size_t i, Inline **node,
                               size_t *next) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(s != nullptr);
    MD_ASSERT(node != nullptr);
    MD_ASSERT(next != nullptr);
    MD_ASSERT(i < n);
    size_t j = i + 1;
    while (j < n && s[j] != '>' && s[j] != '<' && !is_ws((unsigned char)s[j])) j++;
    if (j >= n || s[j] != '>') return false;
    size_t len = j - (i + 1);
    if (len == 0) return false;
    const char *body = s + i + 1;
    bool is_email = false;
    bool has_scheme = false;
    size_t colon = 0;
    for (size_t k = 0; k < len; k++) {
        if (body[k] == ':') {
            colon = k;
            has_scheme = true;
            break;
        }
        if (body[k] == '@') {
            is_email = true;
        }
    }
    if (has_scheme) {
        if (colon < 2 || colon > 32) return false;
        for (size_t k = 0; k < colon; k++) {
            unsigned char c = (unsigned char)body[k];
            if (!is_alnum(c) && c != '+' && c != '-' && c != '.') return false;
        }
        char *u = arena_strndup(p->arena, body, len);
        *node = make_link(p->arena, body, len, u, len);
        *next = j + 1;
        return true;
    }
    if (!is_email) return false;
    size_t elen = 0;
    if (!scan_email(body, len, 0, &elen) || elen != len) return false;
    size_t ul = 7 + len;
    char *u = arena_alloc(p->arena, ul + 1);
    memcpy(u, "mailto:", 7);
    memcpy(u + 7, body, len);
    u[ul] = '\0';
    *node = make_link(p->arena, body, len, u, ul);
    *next = j + 1;
    return true;
}

static void process_emphasis(Arena *a, InlList *list, Delim *top) {
    MD_ASSERT(a != nullptr);
    MD_ASSERT(list != nullptr);
    MD_ASSERT((list->first == nullptr) == (list->last == nullptr));
    MD_ASSERT(top == nullptr || top->next == nullptr);
    Delim *bottom_star = nullptr;
    Delim *bottom_under = nullptr;
    Delim *bottom_tilde = nullptr;
    Delim *closer = top;
    while (closer && closer->prev) closer = closer->prev;
    while (closer) {
        if (!closer->can_close || MD_NEVER(closer->len <= 0)) {
            closer = closer->next;
            continue;
        }
        char ch = closer->ch;
        if (ch == '~' && closer->len < 2) {
            closer = closer->next;
            continue;
        }
        Delim **bottom = ch == '*' ? &bottom_star : ch == '_' ? &bottom_under : &bottom_tilde;
        Delim *opener = closer->prev;
        Delim *found = nullptr;
        while (opener && opener != *bottom) {
            if (opener->ch == ch && opener->can_open && MD_ALWAYS(opener->len > 0)) {
                if (ch == '~' && opener->len < 2) {
                    opener = opener->prev;
                    continue;
                }
                bool odd = false;
                if (ch != '~' && (closer->can_open || opener->can_close)) {
                    int sum = opener->len + closer->len;
                    /*
                     * With sum % 3 == 0, opener and closer are either both
                     * divisible by 3 or both not, so the original
                     * !(op%3==0 && cl%3==0) test reduces to op%3 != 0.
                     */
                    odd = (sum % 3 == 0) && (opener->len % 3 != 0);
                }
                if (!odd) {
                    found = opener;
                    break;
                }
            }
            opener = opener->prev;
        }
        if (!found) {
            *bottom = closer->prev;
            Delim *next = closer->next;
            if (!closer->can_open) delim_unlink(&top, closer);
            closer = next;
            continue;
        }
        bool strong = found->len >= 2 && closer->len >= 2;
        int use = strong ? 2 : 1;
        Inline *first = found->node->next;
        Inline *last = closer->node->prev;
        if (first == closer->node) {
            Delim *next = closer->next;
            delim_unlink(&top, closer);
            closer = next;
            continue;
        }
        InlKind kind = ch == '~' ? I_STRIKE : (use == 2 ? I_STRONG : I_EMPH);
        Inline *wrap = inl_new(a, kind);
        Inline *before = found->node;
        Inline *after = closer->node;
        MD_ASSERT(before != after);
        MD_ASSERT(first->prev == before);
        MD_ASSERT(last->next == after);
        before->next = after;
        after->prev = before;
        first->prev = nullptr;
        last->next = nullptr;
        wrap->first = first;
        wrap->last = last;
        wrap->prev = before;
        before->next = wrap;
        wrap->next = after;
        after->prev = wrap;
        found->node->text_len -= (size_t)use;
        found->len -= use;
        closer->node->text += use;
        closer->node->text_len -= (size_t)use;
        closer->len -= use;
        Delim *d = found->next;
        while (d != closer) {
            MD_ASSERT(d != nullptr);
            Delim *next = d->next;
            delim_unlink(&top, d);
            d = next;
        }
        if (found->len == 0) {
            inl_remove(list, found->node);
            delim_unlink(&top, found);
        }
        if (closer->len == 0) {
            Delim *next = closer->next;
            inl_remove(list, closer->node);
            delim_unlink(&top, closer);
            closer = next;
        }
    }
}

static void parse_inline_into(Parser *p, const char *s, size_t n, InlList *out, int depth,
                              bool allow_links, bool allow_images) {
    MD_ASSERT(p != nullptr);
    MD_ASSERT(s != nullptr || n == 0);
    MD_ASSERT(out != nullptr);
    MD_ASSERT(depth >= 0);
    if (depth > kMaxInlineDepth) {
        if (n) inl_append(out, make_text(p->arena, s, n));
        return;
    }
    Delim *top = nullptr;
    size_t i = 0;
    size_t text_start = 0;
    while (i < n) {
        Inline *auto_node = nullptr;
        size_t auto_next = 0;
        if (allow_links && try_bare_autolink(p, s, n, i, &auto_node, &auto_next)) {
            if (i > text_start) {
                inl_append(out, make_text(p->arena, s + text_start, i - text_start));
            }
            inl_append(out, auto_node);
            i = auto_next;
            text_start = i;
            continue;
        }
        char c = s[i];
        bool special = c == '\\' || c == '\n' || c == '`' || c == '*' || c == '_' ||
                       c == '~' || c == '[' || c == '!' || c == '<';
        if (!special) {
            i++;
            continue;
        }
        if (i > text_start) inl_append(out, make_text(p->arena, s + text_start, i - text_start));
        text_start = i;
        if (c == '\\') {
            if (i + 1 < n && s[i + 1] == '\n') {
                inl_append(out, inl_new(p->arena, I_HARDBREAK));
                i += 2;
                text_start = i;
                continue;
            }
            if (i + 1 < n && is_punct((unsigned char)s[i + 1])) {
                Inline *t = make_text(p->arena, s + i + 1, 1);
                inl_append(out, t);
                i += 2;
                text_start = i;
                continue;
            }
            i++;
            continue;
        }
        if (c == '\n') {
            Inline *last = out->last;
            bool hard = false;
            if (last && last->kind == I_TEXT) {
                size_t k = last->text_len;
                while (k > 0 && last->text[k - 1] == ' ') k--;
                if (last->text_len - k >= 2) {
                    last->text_len = k;
                    hard = true;
                }
            }
            inl_append(out, inl_new(p->arena, hard ? I_HARDBREAK : I_SOFTBREAK));
            i++;
            text_start = i;
            continue;
        }
        if (c == '`') {
            size_t run = 0;
            size_t j = i;
            while (j < n && s[j] == '`') {
                run++;
                j++;
            }
            size_t m = j;
            size_t content_start = 0;
            size_t content_end = 0;
            bool found = false;
            while (m < n) {
                if (s[m] == '`') {
                    size_t r = 0;
                    size_t q = m;
                    while (q < n && s[q] == '`') {
                        r++;
                        q++;
                    }
                    if (r == run) {
                        content_start = j;
                        content_end = m;
                        m = q;
                        found = true;
                        break;
                    }
                    m = q;
                } else {
                    m++;
                }
            }
            if (!found) {
                inl_append(out, make_text(p->arena, s + i, run));
                i = j;
                text_start = i;
                continue;
            }
            size_t clen = 0;
            char *text = code_span_copy(p->arena, s + content_start, content_end - content_start,
                                        &clen);
            Inline *code = inl_new(p->arena, I_CODE);
            code->text = text;
            code->text_len = clen;
            inl_append(out, code);
            i = m;
            text_start = i;
            continue;
        }
        if (c == '*' || c == '_' || c == '~') {
            size_t run = 0;
            size_t j = i;
            while (j < n && s[j] == c) {
                run++;
                j++;
            }
            if (c == '~' && run < 2) {
                i = j;
                continue;
            }
            bool can_open = false;
            bool can_close = false;
            delimiter_flanking(s, n, i, run, &can_open, &can_close);
            Inline *node = make_text(p->arena, s + i, run);
            inl_append(out, node);
            Delim *d = arena_alloc0(p->arena, sizeof *d);
            d->node = node;
            d->len = (int)run;
            d->ch = c;
            d->can_open = can_open;
            d->can_close = can_close;
            d->prev = top;
            if (top) top->next = d;
            top = d;
            i = j;
            text_start = i;
            continue;
        }
        if (c == '!' && i + 1 < n && s[i + 1] == '[') {
            size_t next = 0;
            InlList tmp = {nullptr, nullptr};
            if (try_link(p, s, n, i + 1, true, &tmp, &next, depth, allow_links, allow_images)) {
                inl_append(out, tmp.first);
                i = next;
                text_start = i;
                continue;
            }
            inl_append(out, make_text(p->arena, s + i, 1));
            i++;
            text_start = i;
            continue;
        }
        if (c == '[') {
            size_t next = 0;
            InlList tmp = {nullptr, nullptr};
            if (try_link(p, s, n, i, false, &tmp, &next, depth, allow_links, allow_images)) {
                inl_append(out, tmp.first);
                i = next;
                text_start = i;
                continue;
            }
            inl_append(out, make_text(p->arena, s + i, 1));
            i++;
            text_start = i;
            continue;
        }
        if (c == '<') {
            Inline *node = nullptr;
            size_t next = 0;
            if (allow_links && try_angle_autolink(p, s, n, i, &node, &next)) {
                inl_append(out, node);
                i = next;
                text_start = i;
                continue;
            }
            size_t end = scan_inline_html(s, n, i);
            if (end) {
                if (p->allow_html) {
                    Inline *html = inl_new(p->arena, I_HTML);
                    html->text = arena_strndup(p->arena, s + i, end - i);
                    html->text_len = end - i;
                    inl_append(out, html);
                } else {
                    inl_append(out, make_text(p->arena, s + i, end - i));
                }
                i = end;
                text_start = i;
                continue;
            }
            i++;
            continue;
        }
        i++;
    }
    if (n > text_start) inl_append(out, make_text(p->arena, s + text_start, n - text_start));
    process_emphasis(p->arena, out, top);
}

static void inline_blocks(Parser *p, Block *b) {
    MD_ASSERT(p != nullptr);
    for (Block *cur = b; cur; cur = cur->next) {
        if (cur->kind == B_PARAGRAPH || cur->kind == B_HEADING || cur->kind == B_TABLE_CELL) {
            InlList inl = {nullptr, nullptr};
            parse_inline_into(p, cur->text, cur->text_len, &inl, 0, true, true);
            cur->inl_first = inl.first;
            cur->inl_last = inl.last;
            MD_ASSERT((cur->inl_first == nullptr) == (cur->inl_last == nullptr));
        }
        if (cur->first) inline_blocks(p, cur->first);
    }
}

/* ------------------------------------------------------------------ */
/* Structural validators (invariant builds only)                       */
/* ------------------------------------------------------------------ */

static void md_validate_span(const Parser *p, Span s, const char *what) {
#if MD_INVARIANTS
    MD_ASSERT(p != nullptr);
    MD_ASSERT(what != nullptr);
    MD_ASSERT(s.off <= p->len);
    MD_ASSERT(s.len <= p->len - s.off);
#else
    (void)p;
    (void)s;
    (void)what;
#endif
}

[[maybe_unused]] static void md_validate_lines(const Parser *p, const Span *lines, size_t n) {
#if MD_INVARIANTS
    MD_ASSERT(p != nullptr);
    MD_ASSERT(lines != nullptr || n == 0);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) {
        md_validate_span(p, lines[i], "line");
        for (size_t k = 0; k < lines[i].len; k++) {
            MD_ASSERT(p->buf[lines[i].off + k] != '\n');
        }
        if (i + 1 < n) {
            MD_ASSERT(lines[i].off + lines[i].len < p->len);
            MD_ASSERT(p->buf[lines[i].off + lines[i].len] == '\n');
            MD_ASSERT(lines[i + 1].off == lines[i].off + lines[i].len + 1);
        }
    }
#else
    (void)p;
    (void)lines;
    (void)n;
#endif
}

[[maybe_unused]] static void md_validate_refs(const Refs *refs) {
#if MD_INVARIANTS
    MD_ASSERT(refs != nullptr);
    if (refs->cap == 0) {
        MD_ASSERT(refs->count == 0);
        MD_ASSERT(refs->slots == nullptr);
        return;
    }
    MD_ASSERT((refs->cap & (refs->cap - 1)) == 0);
    MD_ASSERT(refs->count <= refs->cap);
    MD_ASSERT(refs->count * 4 <= refs->cap * 3 + 4);
    for (size_t i = 0; i < refs->cap; i++) {
        const Ref *r = &refs->slots[i];
        if (!r->label) {
            MD_ASSERT(r->label_len == 0);
            continue;
        }
        MD_ASSERT(r->label_len > 0);
        MD_ASSERT(r->url != nullptr);
        if (r->title_len) MD_ASSERT(r->title != nullptr);
        MD_ASSERT(refs_get(refs, r->label, r->label_len) == r);
    }
#else
    (void)refs;
#endif
}

[[maybe_unused]] static void md_validate_inline_list(const Inline *first, const Inline *last, int depth,
                                    size_t *budget) {
#if MD_INVARIANTS
    MD_ASSERT((first == nullptr) == (last == nullptr));
    MD_ASSERT(depth < 4096);
    if (!first) return;
    const Inline *prev = nullptr;
    const Inline *cur = first;
    const Inline *seen_last = nullptr;
    while (cur) {
        MD_ASSERT(cur->prev == prev);
        if (prev) MD_ASSERT(prev->next == cur);
        MD_ASSERT(*budget > 0);
        (*budget)--;
        MD_ASSERT(depth < 4096);
        switch (cur->kind) {
        case I_TEXT:
        case I_CODE:
        case I_HTML:
            MD_ASSERT(cur->text != nullptr || cur->text_len == 0);
            MD_ASSERT(cur->first == nullptr);
            MD_ASSERT(cur->last == nullptr);
            MD_ASSERT(cur->url == nullptr);
            break;
        case I_EMPH:
        case I_STRONG:
        case I_STRIKE:
            MD_ASSERT(cur->text == nullptr);
            MD_ASSERT(cur->first != nullptr);
            MD_ASSERT(cur->last != nullptr);
            break;
        case I_LINK:
            MD_ASSERT(cur->url != nullptr);
            MD_ASSERT(cur->text == nullptr);
            if (cur->title_len) MD_ASSERT(cur->title != nullptr);
            break;
        case I_IMAGE:
            MD_ASSERT(cur->url != nullptr);
            MD_ASSERT(cur->text == nullptr);
            break;
        case I_SOFTBREAK:
        case I_HARDBREAK:
            MD_ASSERT(cur->first == nullptr);
            MD_ASSERT(cur->last == nullptr);
            MD_ASSERT(cur->text == nullptr);
            break;
        default:
            MD_NEVER(1);
            break;
        }
        if (cur->first) md_validate_inline_list(cur->first, cur->last, depth + 1, budget);
        else MD_ASSERT(cur->last == nullptr);
        prev = cur;
        seen_last = cur;
        cur = cur->next;
    }
    MD_ASSERT(seen_last == last);
#else
    (void)first;
    (void)last;
    (void)depth;
    (void)budget;
#endif
}

[[maybe_unused]] static void md_validate_block_list(Parser *p, const Block *first, const Block *last,
                                   const Block *parent, int depth, size_t *budget) {
#if MD_INVARIANTS
    MD_ASSERT(p != nullptr);
    MD_ASSERT((first == nullptr) == (last == nullptr));
    MD_ASSERT(depth < 128);
    const Block *prev = nullptr;
    const Block *cur = first;
    const Block *seen_last = nullptr;
    while (cur) {
        MD_ASSERT(*budget > 0);
        (*budget)--;
        MD_ASSERT(cur->prev == prev);
        if (prev) MD_ASSERT(prev->next == cur);
        MD_ASSERT(cur->parent == parent);
        MD_ASSERT((cur->first == nullptr) == (cur->last == nullptr));
        MD_ASSERT((cur->inl_first == nullptr) == (cur->inl_last == nullptr));
        switch (cur->kind) {
        case B_DOCUMENT:
            MD_NEVER(1);
            break;
        case B_HEADING:
            MD_ASSERT(cur->level >= 1 && cur->level <= 6);
            MD_ASSERT(cur->text != nullptr);
            MD_ASSERT(cur->first == nullptr);
            break;
        case B_PARAGRAPH:
            MD_ASSERT(cur->text != nullptr);
            MD_ASSERT(cur->first == nullptr);
            break;
        case B_CODE:
            MD_ASSERT(cur->first == nullptr);
            MD_ASSERT(cur->inl_first == nullptr);
            if (cur->info_len) MD_ASSERT(cur->info != nullptr);
            break;
        case B_HTML_BLOCK:
            MD_ASSERT(p->allow_html);
            MD_ASSERT(cur->first == nullptr);
            MD_ASSERT(cur->inl_first == nullptr);
            break;
        case B_BLOCKQUOTE:
            MD_ASSERT(cur->text == nullptr);
            MD_ASSERT(cur->inl_first == nullptr);
            break;
        case B_LIST:
            MD_ASSERT(cur->text == nullptr);
            MD_ASSERT(cur->inl_first == nullptr);
            MD_ASSERT(cur->first != nullptr);
            MD_ASSERT(!cur->ordered || cur->start >= 0);
            break;
        case B_ITEM:
            MD_ASSERT(cur->parent != nullptr && cur->parent->kind == B_LIST);
            MD_ASSERT(!cur->checked || cur->task);
            break;
        case B_THEMATIC_BREAK:
            MD_ASSERT(cur->first == nullptr);
            MD_ASSERT(cur->text == nullptr);
            break;
        case B_TABLE: {
            MD_ASSERT(cur->ncols > 0 && cur->ncols <= kMaxTableCols);
            MD_ASSERT(cur->aligns != nullptr);
            MD_ASSERT(cur->first != nullptr);
            size_t row_index = 0;
            for (const Block *row = cur->first; row; row = row->next, row_index++) {
                MD_ASSERT(row->kind == B_TABLE_ROW);
                MD_ASSERT(row->header == (row_index == 0));
                size_t cells = 0;
                size_t cell_index = 0;
                for (const Block *cell = row->first; cell; cell = cell->next, cell_index++) {
                    MD_ASSERT(cell->kind == B_TABLE_CELL);
                    MD_ASSERT(cell->parent == row);
                    MD_ASSERT(cell->header == row->header);
                    MD_ASSERT(cell->align == cur->aligns[cell_index]);
                    cells++;
                }
                MD_ASSERT(cells == cur->ncols);
            }
            break;
        }
        case B_TABLE_ROW:
            MD_ASSERT(cur->parent != nullptr && cur->parent->kind == B_TABLE);
            MD_ASSERT(cur->first != nullptr);
            break;
        case B_TABLE_CELL:
            MD_ASSERT(cur->parent != nullptr && cur->parent->kind == B_TABLE_ROW);
            MD_ASSERT(cur->first == nullptr);
            MD_ASSERT(cur->text != nullptr);
            break;
        default:
            MD_NEVER(1);
            break;
        }
        if (cur->inl_first) md_validate_inline_list(cur->inl_first, cur->inl_last, 0, budget);
        if (cur->first) md_validate_block_list(p, cur->first, cur->last, cur, depth + 1, budget);
        prev = cur;
        seen_last = cur;
        cur = cur->next;
    }
    MD_ASSERT(seen_last == last);
#else
    (void)p;
    (void)first;
    (void)last;
    (void)parent;
    (void)depth;
    (void)budget;
#endif
}

[[maybe_unused]] static void md_validate_document(Parser *p, Block *doc) {
#if MD_INVARIANTS
    MD_ASSERT(p != nullptr);
    MD_ASSERT(doc != nullptr);
    MD_ASSERT(doc->kind == B_DOCUMENT);
    MD_ASSERT(doc->parent == nullptr);
    MD_ASSERT(doc->prev == nullptr);
    MD_ASSERT(doc->next == nullptr);
    MD_ASSERT(doc->inl_first == nullptr);
    size_t budget = kMaxValidateNodes;
    md_validate_block_list(p, doc->first, doc->last, doc, 0, &budget);
    md_validate_refs(&p->refs);
#else
    (void)p;
    (void)doc;
#endif
}

/* ------------------------------------------------------------------ */
/* Reference definition parsing (needs Parser)                         */
/* ------------------------------------------------------------------ */

static bool parse_ref_definition(Parser *p, Span line, Ref *out) {
    const char *buf = p->buf;
    size_t i = line.off;
    size_t end = line.off + line.len;
    size_t lead = 0;
    while (i + lead < end && buf[i + lead] == ' ') lead++;
    if (lead > 3) return false;
    i += lead;
    if (i >= end || buf[i] != '[') return false;
    size_t label_start = i + 1;
    size_t j = label_start;
    while (j < end) {
        if (buf[j] == '\\' && j + 1 < end) {
            j += 2;
            continue;
        }
        if (buf[j] == ']') break;
        if (buf[j] == '[' || buf[j] == '\n') return false;
        j++;
    }
    if (j >= end) return false;
    MD_ASSERT(buf[j] == ']');
    size_t label_end = j;
    i = j + 1;
    if (i >= end || buf[i] != ':') return false;
    i++;
    while (i < end && is_sp_tab((unsigned char)buf[i])) i++;
    size_t dest_start = 0;
    size_t dest_end = 0;
    if (i < end && buf[i] == '<') {
        i++;
        dest_start = i;
        while (i < end && buf[i] != '>') {
            if (buf[i] == '\\' && i + 1 < end) i += 2;
            else if (buf[i] == '\n') return false;
            else i++;
        }
        if (i >= end) return false;
        dest_end = i;
        i++;
    } else {
        dest_start = i;
        size_t parens = 0;
        while (i < end) {
            char c = buf[i];
            if (c == '\\' && i + 1 < end) {
                i += 2;
                continue;
            }
            if (c == ' ' || c == '\t') break;
            if (c == '(') parens++;
            else if (c == ')') {
                if (parens == 0) break;
                parens--;
            }
            i++;
        }
        if (parens != 0) return false;
        dest_end = i;
    }
    if (dest_end == dest_start) return false;
    size_t save = i;
    while (i < end && is_sp_tab((unsigned char)buf[i])) i++;
    size_t title_start = 0;
    size_t title_end = 0;
    if (i < end && (buf[i] == '"' || buf[i] == '\'' || buf[i] == '(')) {
        char open = buf[i];
        char close = open == '(' ? ')' : open;
        i++;
        title_start = i;
        while (i < end && buf[i] != close) {
            if (buf[i] == '\\' && i + 1 < end) i += 2;
            else i++;
        }
        if (i >= end) return false;
        title_end = i;
        i++;
    } else {
        i = save;
    }
    while (i < end && is_sp_tab((unsigned char)buf[i])) i++;
    if (i != end) return false;
    size_t label_len = 0;
    char *label = normalize_label(p->arena, buf + label_start, label_end - label_start, &label_len);
    if (label_len == 0) return false;
    out->label = label;
    out->label_len = label_len;
    out->url = arena_unescape(p->arena, buf + dest_start, dest_end - dest_start, &out->url_len);
    if (title_end > title_start) {
        out->title = arena_unescape(p->arena, buf + title_start, title_end - title_start,
                                    &out->title_len);
    } else {
        out->title = nullptr;
        out->title_len = 0;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Plain text extraction and slugs                                     */
/* ------------------------------------------------------------------ */

static void out_plain(Out *o, Inline *n) {
    MD_ASSERT(o != nullptr);
    for (Inline *cur = n; cur; cur = cur->next) {
        MD_ASSERT(cur->kind <= I_HTML);
        switch (cur->kind) {
        case I_TEXT:
        case I_CODE:
        case I_HTML:
            if (cur->kind != I_HTML) out_mem(o, cur->text, cur->text_len);
            break;
        case I_SOFTBREAK:
        case I_HARDBREAK:
            out_ch(o, ' ');
            break;
        default:
            out_plain(o, cur->first);
            break;
        }
    }
}

static char *make_slug(Arena *a, const char *s, size_t n, size_t *out_len) {
    MD_ASSERT(a != nullptr);
    MD_ASSERT(s != nullptr || n == 0);
    MD_ASSERT(out_len != nullptr);
    char *buf = arena_alloc(a, n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_alnum(c) || c == '_' || c == '-') {
            buf[j++] = (char)to_lower(c);
        } else if (c == ' ') {
            buf[j++] = '-';
        } else if (c >= 0x80) {
            buf[j++] = (char)c;
        }
    }
    MD_ASSERT(j <= n);
    buf[j] = '\0';
    *out_len = j;
    return buf;
}

typedef struct {
    char *s;
    size_t len;
    unsigned long next;
} IdEntry;

typedef struct {
    IdEntry *slots;
    size_t cap;
    size_t count;
    Arena *arena;
} IdSet;

static void ids_init(IdSet *set, Arena *arena) {
    MD_ASSERT(set != nullptr);
    MD_ASSERT(arena != nullptr);
    set->slots = nullptr;
    set->cap = 0;
    set->count = 0;
    set->arena = arena;
}

static void ids_grow(IdSet *set) {
    MD_ASSERT(set != nullptr);
    MD_ASSERT(set->arena != nullptr);
    size_t cap = set->cap ? set->cap * 2 : 64;
    MD_ASSERT((cap & (cap - 1)) == 0);
    IdEntry *slots = arena_alloc0(set->arena, cap * sizeof *slots);
    for (size_t i = 0; i < set->cap; i++) {
        IdEntry *old = &set->slots[i];
        if (!old->s) continue;
        size_t idx = (size_t)(hash_bytes(old->s, old->len) & (cap - 1));
        while (slots[idx].s) idx = (idx + 1) & (cap - 1);
        slots[idx] = *old;
    }
    set->slots = slots;
    set->cap = cap;
    MD_ASSERT(set->count < set->cap);
}

static IdEntry *ids_find(const IdSet *set, const char *s, size_t len) {
    MD_ASSERT(set != nullptr);
    MD_ASSERT(s != nullptr || len == 0);
    if (set->cap == 0) return nullptr;
    MD_ASSERT((set->cap & (set->cap - 1)) == 0);
    MD_ASSERT(set->count <= set->cap);
    size_t idx = (size_t)(hash_bytes(s, len) & (set->cap - 1));
    for (;;) {
        IdEntry *cur = &set->slots[idx];
        if (!cur->s) return nullptr;
        if (cur->len == len && memcmp(cur->s, s, len) == 0) return cur;
        idx = (idx + 1) & (set->cap - 1);
    }
}

static IdEntry *ids_insert(IdSet *set, char *s, size_t len) {
    MD_ASSERT(set != nullptr);
    MD_ASSERT(s != nullptr);
    if (set->cap == 0 || (set->count + 1) * 4 >= set->cap * 3) ids_grow(set);
    MD_ASSERT((set->cap & (set->cap - 1)) == 0);
    size_t idx = (size_t)(hash_bytes(s, len) & (set->cap - 1));
    while (set->slots[idx].s) idx = (idx + 1) & (set->cap - 1);
    IdEntry *entry = &set->slots[idx];
    entry->s = s;
    entry->len = len;
    entry->next = 1;
    set->count++;
    MD_ASSERT(set->count <= set->cap);
    return entry;
}

static char *unique_slug(Arena *a, IdSet *set, const char *text, size_t n, size_t *out_len) {
    size_t base_len = 0;
    char *base = make_slug(a, text, n, &base_len);
    if (base_len == 0) {
        base = arena_strndup(a, "section", 7);
        base_len = 7;
    }
    IdEntry *entry = ids_find(set, base, base_len);
    if (!entry) {
        ids_insert(set, base, base_len);
        *out_len = base_len;
        return base;
    }
    unsigned long suffix = entry->next;
    for (;;) {
        char tail[32];
        int tl = snprintf(tail, sizeof tail, "-%lu", suffix);
        if (tl <= 0) tl = 0;
        size_t cand_len = base_len + (size_t)tl;
        char *cand = arena_alloc(a, cand_len + 1);
        memcpy(cand, base, base_len);
        memcpy(cand + base_len, tail, (size_t)tl);
        cand[cand_len] = '\0';
        if (!ids_find(set, cand, cand_len)) {
            ids_insert(set, cand, cand_len);
            IdEntry *be = ids_find(set, base, base_len);
            if (be) be->next = suffix + 1;
            *out_len = cand_len;
            return cand;
        }
        suffix++;
    }
}

/* ------------------------------------------------------------------ */
/* Renderer                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    Out out;
    Arena *arena;
    const md_options *opts;
    IdSet ids;
} Render;

static void render_inlines(Render *r, Inline *n);

static void render_image(Render *r, Inline *img) {
    MD_ASSERT(r != nullptr);
    MD_ASSERT(img != nullptr);
    MD_ASSERT(img->kind == I_IMAGE);
    MD_ASSERT(img->url != nullptr);
    Out alt;
    out_init(&alt, r->arena);
    out_plain(&alt, img->first);
    if (!url_is_safe(img->url, img->url_len)) {
        out_text(&r->out, alt.data, alt.len);
        return;
    }
    out_lit(&r->out, "<img src=\"");
    out_attr(&r->out, img->url, img->url_len);
    out_lit(&r->out, "\" alt=\"");
    out_attr(&r->out, alt.data, alt.len);
    out_lit(&r->out, "\"");
    if (img->title && img->title_len) {
        out_lit(&r->out, " title=\"");
        out_attr(&r->out, img->title, img->title_len);
        out_lit(&r->out, "\"");
    }
    out_lit(&r->out, ">");
}

static void render_inlines(Render *r, Inline *n) {
    MD_ASSERT(r != nullptr);
    for (Inline *cur = n; cur; cur = cur->next) {
        MD_ASSERT(cur->kind <= I_HTML);
        switch (cur->kind) {
        case I_TEXT:
            out_text(&r->out, cur->text, cur->text_len);
            break;
        case I_CODE:
            out_lit(&r->out, "<code>");
            out_code(&r->out, cur->text, cur->text_len);
            out_lit(&r->out, "</code>");
            break;
        case I_EMPH:
            out_lit(&r->out, "<em>");
            render_inlines(r, cur->first);
            out_lit(&r->out, "</em>");
            break;
        case I_STRONG:
            out_lit(&r->out, "<strong>");
            render_inlines(r, cur->first);
            out_lit(&r->out, "</strong>");
            break;
        case I_STRIKE:
            out_lit(&r->out, "<del>");
            render_inlines(r, cur->first);
            out_lit(&r->out, "</del>");
            break;
        case I_LINK:
            if (!url_is_safe(cur->url, cur->url_len)) {
                render_inlines(r, cur->first);
            } else {
                out_lit(&r->out, "<a href=\"");
                out_attr(&r->out, cur->url, cur->url_len);
                out_lit(&r->out, "\"");
                if (cur->title && cur->title_len) {
                    out_lit(&r->out, " title=\"");
                    out_attr(&r->out, cur->title, cur->title_len);
                    out_lit(&r->out, "\"");
                }
                out_lit(&r->out, ">");
                render_inlines(r, cur->first);
                out_lit(&r->out, "</a>");
            }
            break;
        case I_IMAGE:
            render_image(r, cur);
            break;
        case I_SOFTBREAK:
            if (r->opts->hardbreaks) out_lit(&r->out, "<br>\n");
            else out_ch(&r->out, '\n');
            break;
        case I_HARDBREAK:
            out_lit(&r->out, "<br>\n");
            break;
        case I_HTML:
            out_mem(&r->out, cur->text, cur->text_len);
            break;
        }
    }
}

static void render_block(Render *r, Block *cur);
static void render_blocks(Render *r, Block *b, bool tight);

static void render_item(Render *r, Block *item, bool tight) {
    MD_ASSERT(r != nullptr);
    MD_ASSERT(item != nullptr);
    MD_ASSERT(item->kind == B_ITEM);
    MD_ASSERT(item->parent != nullptr && item->parent->kind == B_LIST);
    out_lit(&r->out, "<li");
    if (item->task) out_lit(&r->out, " class=\"task-list-item\"");
    out_ch(&r->out, '>');
    if (item->task) {
        out_lit(&r->out, "<input type=\"checkbox\" disabled");
        if (item->checked) out_lit(&r->out, " checked");
        out_lit(&r->out, "> ");
    }
    for (Block *c = item->first; c; c = c->next) {
        if (tight && c->kind == B_PARAGRAPH) {
            render_inlines(r, c->inl_first);
            if (c->next) out_ch(&r->out, '\n');
        } else {
            render_block(r, c);
        }
    }
    out_lit(&r->out, "</li>\n");
}

static void render_table(Render *r, Block *table) {
    MD_ASSERT(r != nullptr);
    MD_ASSERT(table != nullptr);
    MD_ASSERT(table->kind == B_TABLE);
    MD_ASSERT(table->ncols > 0 && table->aligns != nullptr);
    out_lit(&r->out, "<table>\n");
    bool body_open = false;
    bool header_open = false;
    size_t row_index = 0;
    for (Block *row = table->first; row; row = row->next, row_index++) {
        bool header = row_index == 0;
        if (header) {
            out_lit(&r->out, "<thead>\n");
            header_open = true;
        } else if (!body_open) {
            out_lit(&r->out, "<tbody>\n");
            body_open = true;
        }
        out_lit(&r->out, "<tr>\n");
        for (Block *cell = row->first; cell; cell = cell->next) {
            out_lit(&r->out, header ? "<th" : "<td");
            const char *align = "";
            if (cell->align == 1) align = " align=\"left\"";
            else if (cell->align == 2) align = " align=\"center\"";
            else if (cell->align == 3) align = " align=\"right\"";
            if (*align) out_lit(&r->out, align);
            out_ch(&r->out, '>');
            render_inlines(r, cell->inl_first);
            out_lit(&r->out, header ? "</th>\n" : "</td>\n");
        }
        out_lit(&r->out, "</tr>\n");
        if (header) {
            out_lit(&r->out, "</thead>\n");
            header_open = false;
        }
    }
    if (header_open) out_lit(&r->out, "</thead>\n");
    if (body_open) out_lit(&r->out, "</tbody>\n");
    out_lit(&r->out, "</table>\n");
}

static void render_block(Render *r, Block *cur) {
    MD_ASSERT(r != nullptr);
    MD_ASSERT(cur != nullptr);
    switch (cur->kind) {
        case B_DOCUMENT:
            render_blocks(r, cur->first, false);
            break;
        case B_HEADING: {
            char *id = nullptr;
            size_t id_len = 0;
            if (r->opts->heading_ids) {
                Out plain;
                out_init(&plain, r->arena);
                out_plain(&plain, cur->inl_first);
                id = unique_slug(r->arena, &r->ids, plain.data, plain.len, &id_len);
            }
            char tag[16];
            int level = cur->level < 1 ? 1 : cur->level > 6 ? 6 : cur->level;
            int tl = snprintf(tag, sizeof tag, "<h%d", level);
            out_mem(&r->out, tag, (size_t)tl);
            if (id) {
                out_lit(&r->out, " id=\"");
                out_attr(&r->out, id, id_len);
                out_lit(&r->out, "\"");
            }
            out_ch(&r->out, '>');
            render_inlines(r, cur->inl_first);
            tl = snprintf(tag, sizeof tag, "</h%d>\n", level);
            out_mem(&r->out, tag, (size_t)tl);
            break;
        }
        case B_PARAGRAPH:
            out_lit(&r->out, "<p>");
            render_inlines(r, cur->inl_first);
            out_lit(&r->out, "</p>\n");
            break;
        case B_CODE:
            out_lit(&r->out, "<pre><code");
            if (cur->info_len) {
                out_lit(&r->out, " class=\"language-");
                out_attr(&r->out, cur->info, cur->info_len);
                out_lit(&r->out, "\"");
            }
            out_ch(&r->out, '>');
            out_code(&r->out, cur->text, cur->text_len);
            if (cur->text_len && cur->text[cur->text_len - 1] != '\n') out_ch(&r->out, '\n');
            out_lit(&r->out, "</code></pre>\n");
            break;
        case B_HTML_BLOCK:
            out_mem(&r->out, cur->text, cur->text_len);
            out_ch(&r->out, '\n');
            break;
        case B_BLOCKQUOTE:
            out_lit(&r->out, "<blockquote>\n");
            render_blocks(r, cur->first, false);
            out_lit(&r->out, "</blockquote>\n");
            break;
        case B_LIST:
            out_lit(&r->out, cur->ordered ? "<ol" : "<ul");
            if (cur->ordered && cur->start != 1) {
                out_lit(&r->out, " start=\"");
                out_count(&r->out, (unsigned long)cur->start);
                out_ch(&r->out, '"');
            }
            if (cur->task) out_lit(&r->out, " class=\"contains-task-list\"");
            out_lit(&r->out, ">\n");
            for (Block *item = cur->first; item; item = item->next) {
                render_item(r, item, cur->tight);
            }
            out_lit(&r->out, cur->ordered ? "</ol>\n" : "</ul>\n");
            break;
        case B_ITEM:
            render_item(r, cur, false);
            break;
        case B_THEMATIC_BREAK:
            out_lit(&r->out, "<hr>\n");
            break;
        case B_TABLE:
            render_table(r, cur);
            break;
        case B_TABLE_ROW:
        case B_TABLE_CELL:
            break;
    }
}

static void render_blocks(Render *r, Block *b, bool tight) {
    MD_ASSERT(r != nullptr);
    for (Block *cur = b; cur; cur = cur->next) {
        MD_ASSERT(cur->kind <= B_TABLE_CELL);
        if (tight && cur->kind == B_PARAGRAPH) {
            render_inlines(r, cur->inl_first);
            out_ch(&r->out, '\n');
        } else {
            render_block(r, cur);
        }
    }
}

static char *document_title(Render *r, Block *doc, size_t *out_len) {
    if (r->opts->title) {
        *out_len = strlen(r->opts->title);
        return (char *)r->opts->title;
    }
    for (Block *b = doc->first; b; b = b->next) {
        if (b->kind == B_HEADING && b->level == 1) {
            Out plain;
            out_init(&plain, r->arena);
            out_plain(&plain, b->inl_first);
            if (plain.len) {
                *out_len = plain.len;
                return plain.data;
            }
            break;
        }
    }
    if (r->opts->title_fallback) {
        *out_len = strlen(r->opts->title_fallback);
        return (char *)r->opts->title_fallback;
    }
    *out_len = strlen("Untitled");
    return (char *)"Untitled";
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

static void set_error(md_error *err, int code, const char *msg) {
    MD_ASSERT(err != nullptr);
    MD_ASSERT(msg != nullptr);
    err->code = code;
    snprintf(err->message, sizeof err->message, "%s", msg);
}

[[nodiscard]] static bool md_render(const char *src, size_t len, const md_options *opts,
                                    md_html *out, md_error *err) {
    MD_ASSERT(out != nullptr);
    MD_ASSERT(err != nullptr);
    MD_ASSERT(opts != nullptr);
    out->data = nullptr;
    out->len = 0;
    if (!src && len != 0) {
        set_error(err, 3, "null input pointer");
        return false;
    }
#if MD_FAULT_ENABLED
    if (md_fault_should_fail()) {
        set_error(err, 1, "out of memory");
        return false;
    }
#endif
    Arena *arena = malloc(sizeof *arena);
    if (!arena) {
        set_error(err, 1, "out of memory");
        return false;
    }
    jmp_buf jb;
    arena_init(arena, &jb, kArenaChunk);
    if (setjmp(jb) != 0) {
        arena_free(arena);
        free(arena);
        set_error(err, 1, "out of memory");
        return false;
    }

    char *buf = arena_alloc(arena, len ? len : 1);
    size_t n = 0;
    size_t start = 0;
    if (len >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
        (unsigned char)src[2] == 0xBF) {
        start = 3;
    }
    for (size_t i = start; i < len; i++) {
        char c = src[i];
        if (c == '\r') {
            if (i + 1 < len && src[i + 1] == '\n') i++;
            c = '\n';
        }
        buf[n++] = c;
    }
    size_t nlines = 0;
    if (n > 0) {
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') nlines++;
        }
        if (buf[n - 1] != '\n') nlines++;
    }
    Span *lines = nlines ? arena_alloc(arena, nlines * sizeof *lines) : nullptr;
    size_t li = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            lines[li++] = (Span){line_start, i - line_start};
            line_start = i + 1;
        }
    }
    if (line_start < n) lines[li++] = (Span){line_start, n - line_start};
    nlines = li;

    Parser p = {
        .buf = buf,
        .len = n,
        .arena = arena,
        .allow_html = opts->allow_html,
        .heading_ids = opts->heading_ids,
        .hardbreaks = opts->hardbreaks,
    };
    refs_init(&p.refs, arena);
    MD_ASSERT(nlines == li);
    md_validate_lines(&p, lines, nlines);
    Block *doc = blk_new(&p, B_DOCUMENT);
    parse_into(&p, lines, nlines, doc, 0);
    md_validate_document(&p, doc);
    inline_blocks(&p, doc->first);
    md_validate_document(&p, doc);

    Render r = {
        .arena = arena,
        .opts = opts,
    };
    out_init(&r.out, arena);
    ids_init(&r.ids, arena);
    if (opts->full_document) {
        out_lit(&r.out, "<!DOCTYPE html>\n<html lang=\"");
        const char *lang = opts->lang ? opts->lang : "en";
        out_attr(&r.out, lang, strlen(lang));
        out_lit(&r.out, "\">\n<head>\n<meta charset=\"utf-8\">\n"
                        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                        "<title>");
        size_t title_len = 0;
        char *title = document_title(&r, doc, &title_len);
        out_text(&r.out, title, title_len);
        out_lit(&r.out, "</title>\n</head>\n<body>\n");
        render_blocks(&r, doc->first, false);
        out_lit(&r.out, "</body>\n</html>\n");
    } else {
        render_blocks(&r, doc->first, false);
    }

    size_t html_len = r.out.len;
#if MD_FAULT_ENABLED
    if (md_fault_should_fail()) {
        arena_free(arena);
        free(arena);
        set_error(err, 1, "out of memory");
        return false;
    }
#endif
    char *result = malloc(html_len + 1);
    if (!result) {
        arena_free(arena);
        free(arena);
        set_error(err, 1, "out of memory");
        return false;
    }
    if (html_len) memcpy(result, r.out.data, html_len);
    result[html_len] = '\0';
    out_validate(&r.out);
    arena_validate(arena);
    arena_free(arena);
    free(arena);
    out->data = result;
    out->len = html_len;
    return true;
}

static void md_html_free(md_html *h) {
    free(h->data);
    h->data = nullptr;
    h->len = 0;
}

/* ------------------------------------------------------------------ */
/* CLI (compiled out in test builds)                                   */
/* ------------------------------------------------------------------ */

#ifndef MD_TEST

[[noreturn]] static void cli_usage_error(const char *msg) {
    if (msg) fprintf(stderr, "mdhtml: %s\n", msg);
    fprintf(stderr, "Try 'mdhtml --help' for usage.\n");
    exit(2);
}

static void print_help(FILE *f) {
    fprintf(f,
            "mdhtml %s - GitHub-flavored Markdown to HTML\n"
            "\n"
            "Usage: mdhtml [options] [file.md]\n"
            "Reads stdin when no file (or \"-\") is given.\n"
            "\n"
            "Options:\n"
            "  -o, --output FILE   write to FILE (default stdout; \"-\" = stdout)\n"
            "  -f, --full          emit a complete HTML5 document\n"
            "  -t, --title TITLE   document title (default: first H1, then file stem)\n"
            "  -l, --lang LANG     lang attribute for full mode (default \"en\")\n"
            "      --allow-html    pass raw HTML through (default: escape it)\n"
            "      --no-ids        omit heading ids\n"
            "      --hardbreaks    render soft line breaks as <br>\n"
            "  -h, --help          show this help\n"
            "  -v, --version       show version\n",
            MDHTML_VERSION);
}

static char *read_stream(FILE *f, size_t *out_len, md_error *err) {
    size_t cap = 64 * 1024;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        set_error(err, 1, "out of memory");
        return nullptr;
    }
    for (;;) {
        if (len == cap) {
            if (cap > SIZE_MAX / 2) {
                free(buf);
                set_error(err, 1, "input too large");
                return nullptr;
            }
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                set_error(err, 1, "out of memory");
                return nullptr;
            }
            buf = nb;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) {
            if (ferror(f)) {
                free(buf);
                set_error(err, 4, "read error");
                return nullptr;
            }
            break;
        }
    }
    *out_len = len;
    return buf;
}

static char *file_stem(const char *path) {
    const char *base = path;
    for (const char *q = path; *q; q++) {
        if (*q == '/' || *q == '\\') base = q + 1;
    }
    size_t len = strlen(base);
    size_t cut = len;
    for (size_t i = len; i > 0; i--) {
        if (base[i - 1] == '.') {
            cut = i - 1;
            break;
        }
    }
    if (cut == 0) cut = len;
    char *stem = malloc(cut + 1);
    if (!stem) return nullptr;
    memcpy(stem, base, cut);
    stem[cut] = '\0';
    return stem;
}

int main(int argc, char **argv) {
    const char *input = nullptr;
    const char *output = nullptr;
    md_options opts;
    md_options_default(&opts);
    bool want_help = false;
    bool want_version = false;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) {
            for (i++; i < argc; i++) {
                if (input) cli_usage_error("multiple input files");
                input = argv[i];
            }
            break;
        }
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            want_help = true;
        } else if (strcmp(arg, "-v") == 0 || strcmp(arg, "--version") == 0) {
            want_version = true;
        } else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--full") == 0) {
            opts.full_document = true;
        } else if (strcmp(arg, "--allow-html") == 0) {
            opts.allow_html = true;
        } else if (strcmp(arg, "--no-ids") == 0) {
            opts.heading_ids = false;
        } else if (strcmp(arg, "--hardbreaks") == 0) {
            opts.hardbreaks = true;
        } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) cli_usage_error("missing value for --output");
            output = argv[++i];
        } else if (strncmp(arg, "--output=", 9) == 0) {
            output = arg + 9;
        } else if (strcmp(arg, "-t") == 0 || strcmp(arg, "--title") == 0) {
            if (i + 1 >= argc) cli_usage_error("missing value for --title");
            opts.title = argv[++i];
        } else if (strncmp(arg, "--title=", 8) == 0) {
            opts.title = arg + 8;
        } else if (strcmp(arg, "-l") == 0 || strcmp(arg, "--lang") == 0) {
            if (i + 1 >= argc) cli_usage_error("missing value for --lang");
            opts.lang = argv[++i];
        } else if (strncmp(arg, "--lang=", 7) == 0) {
            opts.lang = arg + 7;
        } else if (arg[0] == '-' && arg[1] != '\0') {
            fprintf(stderr, "mdhtml: unknown option '%s'\n", arg);
            cli_usage_error(nullptr);
        } else {
            if (input) cli_usage_error("multiple input files");
            input = arg;
        }
    }
    if (want_help) {
        print_help(stdout);
        return 0;
    }
    if (want_version) {
        printf("mdhtml %s\n", MDHTML_VERSION);
        return 0;
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    md_error err = {0, {0}};
    bool use_stdin = !input || strcmp(input, "-") == 0;
    FILE *in = stdin;
    if (!use_stdin) {
        in = fopen(input, "rb");
        if (!in) {
            fprintf(stderr, "mdhtml: cannot open '%s': %s\n", input, strerror(errno));
            return 1;
        }
    }
    size_t src_len = 0;
    char *src = read_stream(in, &src_len, &err);
    if (!use_stdin) fclose(in);
    if (!src) {
        fprintf(stderr, "mdhtml: %s\n", err.message);
        return 1;
    }
    char *stem = nullptr;
    if (opts.full_document && !opts.title) {
        if (use_stdin) {
            opts.title_fallback = "Untitled";
        } else {
            stem = file_stem(input);
            if (!stem) {
                free(src);
                fprintf(stderr, "mdhtml: out of memory\n");
                return 1;
            }
            opts.title_fallback = stem;
        }
    }
    md_html html = {nullptr, 0};
    if (!md_render(src, src_len, &opts, &html, &err)) {
        free(src);
        free(stem);
        fprintf(stderr, "mdhtml: %s\n", err.message);
        return 1;
    }
    free(src);
    free(stem);
    FILE *outf = stdout;
    if (output && strcmp(output, "-") != 0) {
        outf = fopen(output, "wb");
        if (!outf) {
            md_html_free(&html);
            fprintf(stderr, "mdhtml: cannot open '%s': %s\n", output, strerror(errno));
            return 1;
        }
    }
    bool ok = true;
    if (html.len && fwrite(html.data, 1, html.len, outf) != html.len) ok = false;
    if (fflush(outf) != 0) ok = false;
    if (outf != stdout && fclose(outf) != 0) ok = false;
    if (!ok) {
        fprintf(stderr, "mdhtml: write error%s%s\n", output ? " to " : "",
                output ? output : "");
        md_html_free(&html);
        return 1;
    }
    md_html_free(&html);
    return 0;
}

#endif /* MD_TEST */
