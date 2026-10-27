/*
 * site_generator.c -- deterministic personal website single-page builder.
 *
 * Conforms strictly to STYLE.md:
 * - Safety, performance, developer experience in that order.
 * - Simple, elegant design; zero dynamic memory allocations after startup.
 * - All memory statically allocated in fixed BSS buffers.
 * - Explicitly-sized types (u32, i32, u8) used exclusively.
 * - No recursion, minimal abstractions.
 * - Centralized control flow and state manipulation. Pure leaf helpers.
 * - Full invariant and debugger-friendly assertions.
 *
 * Performance back-of-the-envelope calculations:
 * - Disk I/O: 100 articles (~1 MB total markdown / HTML). Sequential read of fragments
 *   and sequential write of index.html takes < 0.6 ms on standard NVMe SSDs.
 * - Memory: ~25 MB statically allocated arena in BSS; zero runtime malloc/free calls,
 *   zero heap churn or memory fragmentation.
 * - CPU: Single-core execution at 3.5 GHz takes < 20 ms to parse headings, rewrite
 *   anchors, and assemble 100 articles into a 3-column layout.
 * - Network: VPS Caddy server delivers compressed index.html (~85 KB over the wire with
 *   zstd/gzip) in ~7 ms, saturating 100 Mbps uplink.
 */

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#error "site_generator requires C23; compile with -std=c23"
#endif

#ifdef _WIN32
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#endif

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Explicitly-sized Types                                              */
/* ------------------------------------------------------------------ */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

/* ------------------------------------------------------------------ */
/* Static Capacity Limits                                              */
/* ------------------------------------------------------------------ */

#define MAX_ARTICLES             256u
#define MAX_HEADINGS_PER_ARTICLE 64u
#define MAX_PATH_LENGTH          512u
#define MAX_NAME_LENGTH          128u
#define MAX_TITLE_LENGTH         256u
#define MAX_FRAGMENT_BUFFER      (2u * 1024u * 1024u)   /* 2 MB per article fragment */
#define MAX_PAGE_BUFFER          (16u * 1024u * 1024u)  /* 16 MB total index.html */

/* ------------------------------------------------------------------ */
/* Domain Structures                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    u8   level;                        /* 1 for <h1>, 2 for <h2>, 3 for <h3> */
    char id[MAX_NAME_LENGTH];          /* Scoped heading ID: "art-<order>-<name>" */
    char text[MAX_TITLE_LENGTH];       /* Heading display text */
} heading_record;

typedef struct {
    u32  order;
    char directory_name[MAX_NAME_LENGTH];
    char article_slug[MAX_NAME_LENGTH];
    char title[MAX_TITLE_LENGTH];
    char fragment_path[MAX_PATH_LENGTH];
    u32  fragment_length;
    u32  heading_count;
    heading_record headings[MAX_HEADINGS_PER_ARTICLE];
} article_record;

/* Static memory buffers (zero heap allocation at runtime) */
static article_record g_articles[MAX_ARTICLES];
static u32            g_article_count = 0;
static char           g_fragment_buffer[MAX_FRAGMENT_BUFFER];
static char           g_rewritten_buffer[MAX_FRAGMENT_BUFFER];
static char           g_page_buffer[MAX_PAGE_BUFFER];

/* ------------------------------------------------------------------ */
/* Pure Leaf Helper Functions                                          */
/* ------------------------------------------------------------------ */

static inline bool char_is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static inline bool char_is_alpha(char c) {
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
}

static inline char char_to_upper(char c) {
    if (c >= 'a' && c <= 'z') {
        return (char)(c - ('a' - 'A'));
    }
    return c;
}

[[maybe_unused]] static u32 string_length(const char *str) {
    assert(str != nullptr);
    u32 len = 0;
    while (str[len] != '\0') {
        len++;
        assert(len < 0xFFFFFFFFu);
    }
    return len;
}

static u32 string_copy(char *dst, const char *src, u32 max_len) {
    assert(dst != nullptr);
    assert(src != nullptr);
    assert(max_len > 0);

    u32 i = 0;
    while (i + 1 < max_len && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

static u32 string_append(char *dst, const char *src, u32 current_len, u32 max_len) {
    assert(dst != nullptr);
    assert(src != nullptr);
    assert(current_len < max_len);

    u32 i = 0;
    while (current_len + 1 < max_len && src[i] != '\0') {
        dst[current_len] = src[i];
        current_len++;
        i++;
    }
    dst[current_len] = '\0';
    return current_len;
}

static u32 string_append_u32(char *dst, u32 value, u32 current_len, u32 max_len) {
    assert(dst != nullptr);
    assert(current_len < max_len);

    char digits[16];
    u32 num_digits = 0;
    u32 temp = value;

    if (temp == 0) {
        digits[num_digits++] = '0';
    } else {
        while (temp > 0 && num_digits < 16) {
            digits[num_digits++] = (char)('0' + (temp % 10));
            temp /= 10;
        }
    }

    while (num_digits > 0 && current_len + 1 < max_len) {
        dst[current_len++] = digits[--num_digits];
    }
    dst[current_len] = '\0';
    return current_len;
}

[[maybe_unused]] static bool string_equals(const char *a, const char *b) {
    assert(a != nullptr);
    assert(b != nullptr);
    u32 i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return false;
        }
        i++;
    }
    return (a[i] == '\0' && b[i] == '\0');
}

/*
 * Parses directory name in format: "<order>-<article_name>"
 * Example: "1-example" -> order = 1, slug = "example"
 */
static bool parse_article_dirname(const char *dirname, u32 *out_order, char *out_slug, u32 max_slug_len) {
    assert(dirname != nullptr);
    assert(out_order != nullptr);
    assert(out_slug != nullptr);
    assert(max_slug_len > 0);

    u32 idx = 0;
    if (!char_is_digit(dirname[idx])) {
        return false;
    }

    u32 parsed_order = 0;
    while (char_is_digit(dirname[idx])) {
        u32 digit = (u32)(dirname[idx] - '0');
        parsed_order = (parsed_order * 10u) + digit;
        idx++;
    }

    if (dirname[idx] != '-') {
        return false;
    }
    idx++; /* skip '-' */

    if (dirname[idx] == '\0') {
        return false;
    }

    u32 slug_idx = 0;
    while (dirname[idx] != '\0' && slug_idx + 1 < max_slug_len) {
        out_slug[slug_idx] = dirname[idx];
        slug_idx++;
        idx++;
    }
    out_slug[slug_idx] = '\0';

    *out_order = parsed_order;
    return true;
}

/*
 * Formats a fallback title from slug: "my-article-name" -> "My Article Name"
 */
static void format_title_from_slug(const char *slug, char *out_title, u32 max_title_len) {
    assert(slug != nullptr);
    assert(out_title != nullptr);
    assert(max_title_len > 0);

    u32 out_idx = 0;
    bool capitalize_next = true;
    for (u32 i = 0; slug[i] != '\0' && out_idx + 1 < max_title_len; i++) {
        char c = slug[i];
        if (c == '-' || c == '_') {
            out_title[out_idx++] = ' ';
            capitalize_next = true;
        } else if (capitalize_next && char_is_alpha(c)) {
            out_title[out_idx++] = char_to_upper(c);
            capitalize_next = false;
        } else {
            out_title[out_idx++] = c;
            capitalize_next = false;
        }
    }
    out_title[out_idx] = '\0';
}

/*
 * In-place iterative insertion sort to arrange articles in descending order.
 */
static void sort_articles_descending(article_record *records, u32 count) {
    assert(records != nullptr || count == 0);
    if (count <= 1) {
        return;
    }

    for (u32 i = 1; i < count; i++) {
        article_record key = records[i];
        i32 j = (i32)i - 1;
        while (j >= 0 && records[j].order < key.order) {
            records[j + 1] = records[j];
            j--;
        }
        records[j + 1] = key;
    }

    for (u32 i = 0; i + 1 < count; i++) {
        assert(records[i].order >= records[i + 1].order);
    }
}

/*
 * Scans an HTML fragment and extracts all <h1, <h2, <h3 headings.
 * Builds scoped IDs: "art-<order>-<original_id>".
 */
static void extract_article_headings(article_record *rec, const char *html) {
    assert(rec != nullptr);
    assert(html != nullptr);

    rec->heading_count = 0;
    const char *p = html;

    while (*p != '\0' && rec->heading_count < MAX_HEADINGS_PER_ARTICLE) {
        /* Search for '<h' followed by '1', '2', or '3' */
        if (p[0] == '<' && p[1] == 'h' && (p[2] >= '1' && p[2] <= '3')) {
            u8 level = (u8)(p[2] - '0');
            const char *tag_start = p;
            const char *tag_open_end = strchr(tag_start, '>');
            if (tag_open_end == nullptr) {
                break;
            }

            /* Extract original id="..." if present */
            char orig_id[MAX_NAME_LENGTH] = {0};
            const char *id_attr = strstr(tag_start, "id=\"");
            if (id_attr != nullptr && id_attr < tag_open_end) {
                const char *val_start = id_attr + 4;
                const char *val_end = strchr(val_start, '"');
                if (val_end != nullptr && val_end <= tag_open_end) {
                    u32 id_len = 0;
                    for (const char *q = val_start; q < val_end && id_len + 1 < MAX_NAME_LENGTH; q++) {
                        orig_id[id_len++] = *q;
                    }
                    orig_id[id_len] = '\0';
                }
            }

            /* Find closing tag: "</hX>" */
            char close_tag[8];
            close_tag[0] = '<';
            close_tag[1] = '/';
            close_tag[2] = 'h';
            close_tag[3] = (char)('0' + level);
            close_tag[4] = '>';
            close_tag[5] = '\0';

            const char *tag_close = strstr(tag_open_end, close_tag);
            if (tag_close != nullptr) {
                /* Extract inner text, stripping inner HTML tags */
                char text[MAX_TITLE_LENGTH] = {0};
                u32 text_len = 0;
                bool inside_tag = false;
                for (const char *q = tag_open_end + 1; q < tag_close && text_len + 1 < MAX_TITLE_LENGTH; q++) {
                    if (*q == '<') {
                        inside_tag = true;
                    } else if (*q == '>') {
                        inside_tag = false;
                    } else if (!inside_tag) {
                        text[text_len++] = *q;
                    }
                }
                text[text_len] = '\0';

                heading_record *h = &rec->headings[rec->heading_count];
                h->level = level;
                string_copy(h->text, text, MAX_TITLE_LENGTH);

                /* Build scoped ID: "art-<order>-<orig_id>" */
                u32 id_len = 0;
                id_len = string_append(h->id, "art-", id_len, MAX_NAME_LENGTH);
                id_len = string_append_u32(h->id, rec->order, id_len, MAX_NAME_LENGTH);
                id_len = string_append(h->id, "-", id_len, MAX_NAME_LENGTH);
                if (orig_id[0] != '\0') {
                    id_len = string_append(h->id, orig_id, id_len, MAX_NAME_LENGTH);
                } else {
                    id_len = string_append(h->id, "h", id_len, MAX_NAME_LENGTH);
                    id_len = string_append_u32(h->id, rec->heading_count + 1, id_len, MAX_NAME_LENGTH);
                }

                /* If rec->title is empty and level == 1, save as article title */
                if (rec->title[0] == '\0' && level == 1 && text_len > 0) {
                    string_copy(rec->title, text, MAX_TITLE_LENGTH);
                }

                rec->heading_count++;
                p = tag_close + 5;
                continue;
            }
        }
        p++;
    }

    if (rec->title[0] == '\0') {
        format_title_from_slug(rec->article_slug, rec->title, MAX_TITLE_LENGTH);
    }
}

/*
 * Rewrites an HTML fragment to:
 * 1. Inject scoped IDs and clickable section anchors (§) on <h1, <h2, <h3.
 * 2. Resolve relative image src paths to "articles/<dir>/<src>".
 * 3. Wrap images in Substack-style centered <figure> and <figcaption>.
 */
static u32 rewrite_fragment_headings(const article_record *rec, const char *src, char *dst, u32 max_len) {
    assert(rec != nullptr);
    assert(src != nullptr);
    assert(dst != nullptr);
    assert(max_len > 0);

    u32 dst_len = 0;
    const char *p = src;
    u32 heading_idx = 0;

    while (*p != '\0' && dst_len + 1 < max_len) {
        /* Headings: <h1, <h2, <h3 */
        if (p[0] == '<' && p[1] == 'h' && (p[2] >= '1' && p[2] <= '3')) {
            u8 level = (u8)(p[2] - '0');
            const char *tag_open_end = strchr(p, '>');
            char close_tag[8] = {'<', '/', 'h', (char)('0' + level), '>', '\0'};
            const char *tag_close = tag_open_end ? strstr(tag_open_end, close_tag) : nullptr;

            if (tag_open_end != nullptr && tag_close != nullptr && heading_idx < rec->heading_count) {
                const heading_record *h = &rec->headings[heading_idx++];

                /* <hN id="scoped_id" class="article-heading"> */
                dst_len = string_append(dst, "<h", dst_len, max_len);
                char lev_char[2] = {(char)('0' + level), '\0'};
                dst_len = string_append(dst, lev_char, dst_len, max_len);
                dst_len = string_append(dst, " id=\"", dst_len, max_len);
                dst_len = string_append(dst, h->id, dst_len, max_len);
                dst_len = string_append(dst, "\" class=\"article-heading\">", dst_len, max_len);

                /* Anchor link: <a class="heading-anchor" href="#..." onclick="...">§</a> */
                dst_len = string_append(dst, "<a class=\"heading-anchor\" href=\"#", dst_len, max_len);
                dst_len = string_append(dst, h->id, dst_len, max_len);
                dst_len = string_append(dst, "\" title=\"Copy link to section\" onclick=\"copyHeadingUrl(event, '", dst_len, max_len);
                dst_len = string_append(dst, h->id, dst_len, max_len);
                dst_len = string_append(dst, "')\">&sect;</a>", dst_len, max_len);

                /* Inner text / contents */
                for (const char *q = tag_open_end + 1; q < tag_close && dst_len + 1 < max_len; q++) {
                    dst[dst_len++] = *q;
                }
                dst[dst_len] = '\0';

                /* </hN> */
                dst_len = string_append(dst, "</h", dst_len, max_len);
                dst_len = string_append(dst, lev_char, dst_len, max_len);
                dst_len = string_append(dst, ">", dst_len, max_len);

                p = tag_close + 5;
                continue;
            }
        }

        /* Images: <img ...> */
        if (p[0] == '<' && p[1] == 'i' && p[2] == 'm' && p[3] == 'g') {
            const char *tag_end = strchr(p, '>');
            if (tag_end != nullptr) {
                char orig_src[MAX_PATH_LENGTH] = {0};
                char orig_alt[MAX_TITLE_LENGTH] = {0};

                /* Extract src="..." */
                const char *src_attr = strstr(p, "src=\"");
                if (src_attr != nullptr && src_attr < tag_end) {
                    const char *val_start = src_attr + 5;
                    const char *val_end = strchr(val_start, '"');
                    if (val_end != nullptr && val_end <= tag_end) {
                        u32 slen = 0;
                        for (const char *q = val_start; q < val_end && slen + 1 < MAX_PATH_LENGTH; q++) {
                            orig_src[slen++] = *q;
                        }
                        orig_src[slen] = '\0';
                    }
                }

                /* Extract alt="..." */
                const char *alt_attr = strstr(p, "alt=\"");
                if (alt_attr != nullptr && alt_attr < tag_end) {
                    const char *val_start = alt_attr + 5;
                    const char *val_end = strchr(val_start, '"');
                    if (val_end != nullptr && val_end <= tag_end) {
                        u32 alen = 0;
                        for (const char *q = val_start; q < val_end && alen + 1 < MAX_TITLE_LENGTH; q++) {
                            orig_alt[alen++] = *q;
                        }
                        orig_alt[alen] = '\0';
                    }
                }

                /* Resolve relative path if needed */
                char resolved_src[MAX_PATH_LENGTH] = {0};
                bool is_absolute = (strncmp(orig_src, "http://", 7) == 0 ||
                                    strncmp(orig_src, "https://", 8) == 0 ||
                                    orig_src[0] == '/');
                if (is_absolute) {
                    string_copy(resolved_src, orig_src, MAX_PATH_LENGTH);
                } else {
                    const char *rel = orig_src;
                    if (rel[0] == '.' && rel[1] == '/') {
                        rel += 2; /* skip "./" */
                    }
                    u32 rlen = 0;
                    rlen = string_append(resolved_src, "articles/", rlen, MAX_PATH_LENGTH);
                    rlen = string_append(resolved_src, rec->directory_name, rlen, MAX_PATH_LENGTH);
                    rlen = string_append(resolved_src, "/", rlen, MAX_PATH_LENGTH);
                    rlen = string_append(resolved_src, rel, rlen, MAX_PATH_LENGTH);
                }

                /* Check if directly preceded by <p> (with optional whitespace) */
                u32 check_idx = dst_len;
                while (check_idx > 0 && (dst[check_idx - 1] == ' ' || dst[check_idx - 1] == '\t' ||
                                         dst[check_idx - 1] == '\n' || dst[check_idx - 1] == '\r')) {
                    check_idx--;
                }
                bool had_enclosing_p = false;
                if (check_idx >= 3 && dst[check_idx - 3] == '<' && dst[check_idx - 2] == 'p' && dst[check_idx - 1] == '>') {
                    dst_len = check_idx - 3;
                    dst[dst_len] = '\0';
                    had_enclosing_p = true;
                }

                /* Emit Substack-style <figure class="article-figure"> */
                dst_len = string_append(dst, "\n<figure class=\"article-figure\">\n  <img src=\"", dst_len, max_len);
                dst_len = string_append(dst, resolved_src, dst_len, max_len);
                dst_len = string_append(dst, "\" alt=\"", dst_len, max_len);
                dst_len = string_append(dst, orig_alt, dst_len, max_len);
                dst_len = string_append(dst, "\" loading=\"lazy\">\n", dst_len, max_len);

                if (orig_alt[0] != '\0') {
                    dst_len = string_append(dst, "  <figcaption class=\"image-caption\">", dst_len, max_len);
                    dst_len = string_append(dst, orig_alt, dst_len, max_len);
                    dst_len = string_append(dst, "</figcaption>\n", dst_len, max_len);
                }

                dst_len = string_append(dst, "</figure>\n", dst_len, max_len);

                p = tag_end + 1;
                if (had_enclosing_p) {
                    /* Skip optional whitespace and matching </p> */
                    const char *after = p;
                    while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r') {
                        after++;
                    }
                    if (after[0] == '<' && after[1] == '/' && after[2] == 'p' && after[3] == '>') {
                        p = after + 4;
                    }
                }
                continue;
            }
        }

        dst[dst_len++] = *p++;
        dst[dst_len] = '\0';
    }

    return dst_len;
}

/* ------------------------------------------------------------------ */
/* File I/O Operations                                                 */
/* ------------------------------------------------------------------ */

static bool read_entire_file(const char *path, char *buffer, u32 max_len, u32 *out_len) {
    assert(path != nullptr);
    assert(buffer != nullptr);
    assert(out_len != nullptr);
    assert(max_len > 0);

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    size_t read_bytes = fread(buffer, 1, max_len - 1, f);
    if (ferror(f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    buffer[read_bytes] = '\0';
    *out_len = (u32)read_bytes;
    return true;
}

static bool write_entire_file(const char *path, const char *content, u32 length) {
    assert(path != nullptr);
    assert(content != nullptr);

    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }

    size_t written = fwrite(content, 1, length, f);
    if (written != (size_t)length) {
        fclose(f);
        return false;
    }

    if (fclose(f) != 0) {
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* HTML Generation Helpers                                             */
/* ------------------------------------------------------------------ */

/*
 * Renders Left Sidebar: Site Title, Search Filter, Articles List.
 */
static u32 render_left_sidebar(const article_record *records, u32 count, char *dst, u32 current_len, u32 max_len) {
    assert(records != nullptr || count == 0);
    assert(dst != nullptr);

    u32 len = current_len;
    len = string_append(dst,
        "    <aside class=\"sidebar-articles\">\n"
        "      <header>\n"
        "        <h1 class=\"site-title\">Notes &amp; Articles</h1>\n"
        "      </header>\n\n"
        "      <div>\n"
        "        <input type=\"search\" id=\"article-filter\" class=\"filter-input\"\n"
        "               placeholder=\"Search articles...\"\n"
        "               autocomplete=\"off\"\n"
        "               oninput=\"handleSearch(this.value)\">\n"
        "      </div>\n\n"
        "      <nav aria-label=\"Articles\">\n"
        "        <div class=\"sidebar-heading\">Articles</div>\n"
        "        <ul class=\"article-list\" id=\"articles-nav\">\n",
        len, max_len);

    for (u32 i = 0; i < count; i++) {
        const article_record *rec = &records[i];
        len = string_append(dst, "          <li class=\"article-list-item", len, max_len);
        if (i == 0) {
            len = string_append(dst, " active", len, max_len);
        }
        len = string_append(dst, "\" id=\"nav-art-", len, max_len);
        len = string_append_u32(dst, rec->order, len, max_len);
        len = string_append(dst, "\" data-order=\"", len, max_len);
        len = string_append_u32(dst, rec->order, len, max_len);
        len = string_append(dst, "\" data-title=\"", len, max_len);
        len = string_append(dst, rec->title, len, max_len);
        len = string_append(dst, "\">\n", len, max_len);

        len = string_append(dst, "            <span class=\"order-badge\">#", len, max_len);
        len = string_append_u32(dst, rec->order, len, max_len);
        len = string_append(dst, "</span>\n", len, max_len);

        len = string_append(dst, "            <a class=\"article-list-link\" href=\"#article-", len, max_len);
        len = string_append_u32(dst, rec->order, len, max_len);
        len = string_append(dst, "-", len, max_len);
        len = string_append(dst, rec->article_slug, len, max_len);
        len = string_append(dst, "\" onclick=\"selectArticle(", len, max_len);
        len = string_append_u32(dst, rec->order, len, max_len);
        len = string_append(dst, ")\">", len, max_len);
        len = string_append(dst, rec->title, len, max_len);
        len = string_append(dst, "</a>\n", len, max_len);
        len = string_append(dst, "          </li>\n", len, max_len);
    }

    len = string_append(dst,
        "        </ul>\n"
        "      </nav>\n"
        "    </aside>\n\n",
        len, max_len);

    return len;
}

/*
 * Renders Right Sidebar: Active Article Outline (Headers & Subheaders TOC).
 */
static u32 render_right_sidebar(const article_record *records, u32 count, char *dst, u32 current_len, u32 max_len) {
    assert(records != nullptr || count == 0);
    assert(dst != nullptr);

    u32 len = current_len;
    len = string_append(dst,
        "    <aside class=\"sidebar-outline\">\n"
        "      <div class=\"sidebar-heading\">Outline</div>\n"
        "      <div class=\"outline-container\" id=\"outline-container\">\n",
        len, max_len);

    for (u32 i = 0; i < count; i++) {
        const article_record *rec = &records[i];
        len = string_append(dst, "        <div class=\"article-outline-block", len, max_len);
        if (i == 0) {
            len = string_append(dst, " active", len, max_len);
        }
        len = string_append(dst, "\" id=\"outline-art-", len, max_len);
        len = string_append_u32(dst, rec->order, len, max_len);
        len = string_append(dst, "\">\n", len, max_len);

        for (u32 h_idx = 0; h_idx < rec->heading_count; h_idx++) {
            const heading_record *h = &rec->headings[h_idx];
            len = string_append(dst, "          <div class=\"outline-item level-", len, max_len);
            char lev_str[2] = {(char)('0' + h->level), '\0'};
            len = string_append(dst, lev_str, len, max_len);
            len = string_append(dst, "\">\n", len, max_len);

            len = string_append(dst, "            <a class=\"outline-link\" href=\"#", len, max_len);
            len = string_append(dst, h->id, len, max_len);
            len = string_append(dst, "\">", len, max_len);
            len = string_append(dst, h->text, len, max_len);
            len = string_append(dst, "</a>\n", len, max_len);

            len = string_append(dst, "            <button class=\"outline-copy-btn\" onclick=\"copyHeadingUrl(event, '", len, max_len);
            len = string_append(dst, h->id, len, max_len);
            len = string_append(dst, "')\" title=\"Copy link to section\">&#x2398;</button>\n", len, max_len);
            len = string_append(dst, "          </div>\n", len, max_len);
        }

        len = string_append(dst, "        </div>\n", len, max_len);
    }

    len = string_append(dst,
        "      </div>\n"
        "    </aside>\n\n",
        len, max_len);

    return len;
}

/* ------------------------------------------------------------------ */
/* Centralized Coordinator Function                                    */
/* ------------------------------------------------------------------ */

int generate_site(const char *articles_dir, const char *output_file) {
    assert(articles_dir != nullptr);
    assert(output_file != nullptr);

    printf("site_generator: scanning articles directory '%s'...\n", articles_dir);

    DIR *dir = opendir(articles_dir);
    if (!dir) {
        fprintf(stderr, "site_generator: failed to open directory '%s': %s\n", articles_dir, strerror(errno));
        return 1;
    }

    g_article_count = 0;
    struct dirent *entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        u32 order = 0;
        char slug[MAX_NAME_LENGTH] = {0};
        if (!parse_article_dirname(entry->d_name, &order, slug, MAX_NAME_LENGTH)) {
            continue;
        }

        if (g_article_count >= MAX_ARTICLES) {
            fprintf(stderr, "site_generator: reached maximum article limit (%u)\n", MAX_ARTICLES);
            break;
        }

        article_record *rec = &g_articles[g_article_count];
        rec->order = order;
        rec->title[0] = '\0';
        string_copy(rec->directory_name, entry->d_name, MAX_NAME_LENGTH);
        string_copy(rec->article_slug, slug, MAX_NAME_LENGTH);

        /* Construct fragment path: "articles/<dir>/article.html" */
        u32 path_len = 0;
        path_len = string_append(rec->fragment_path, articles_dir, path_len, MAX_PATH_LENGTH);
        path_len = string_append(rec->fragment_path, "/", path_len, MAX_PATH_LENGTH);
        path_len = string_append(rec->fragment_path, entry->d_name, path_len, MAX_PATH_LENGTH);
        path_len = string_append(rec->fragment_path, "/article.html", path_len, MAX_PATH_LENGTH);

        /* Read fragment to extract headings and title */
        u32 frag_len = 0;
        if (read_entire_file(rec->fragment_path, g_fragment_buffer, MAX_FRAGMENT_BUFFER, &frag_len)) {
            rec->fragment_length = frag_len;
            extract_article_headings(rec, g_fragment_buffer);
            g_article_count++;
        } else {
            fprintf(stderr, "site_generator: warning - could not read '%s', skipping.\n", rec->fragment_path);
        }
    }
    closedir(dir);

    printf("site_generator: found %u valid articles.\n", g_article_count);
    if (g_article_count == 0) {
        fprintf(stderr, "site_generator: no articles found to generate.\n");
        return 1;
    }

    /* Sort descending by order (newest first) */
    sort_articles_descending(g_articles, g_article_count);

    /* Assemble Page */
    u32 page_len = 0;
    page_len = string_append(g_page_buffer,
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"UTF-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "  <link rel=\"icon\" type=\"image/x-icon\" href=\"./assets/favicon.ico\">\n"
        "  <title>Notes &amp; Articles</title>\n"
        "  <link rel=\"stylesheet\" href=\"assets/style.css\">\n"
        "  <link rel=\"stylesheet\" href=\"assets/highlight-dark.min.css\">\n"
        "  <script src=\"assets/highlight.min.js\"></script>\n"
        "</head>\n"
        "<body id=\"top\">\n"
        "  <div class=\"layout-container\">\n",
        page_len, MAX_PAGE_BUFFER);

    /* Render Left Sidebar */
    page_len = render_left_sidebar(g_articles, g_article_count, g_page_buffer, page_len, MAX_PAGE_BUFFER);

    /* Center Stream */
    page_len = string_append(g_page_buffer,
        "    <main class=\"main-stream\" id=\"articles-stream\">\n",
        page_len, MAX_PAGE_BUFFER);

    for (u32 i = 0; i < g_article_count; i++) {
        const article_record *rec = &g_articles[i];
        u32 frag_len = 0;
        if (read_entire_file(rec->fragment_path, g_fragment_buffer, MAX_FRAGMENT_BUFFER, &frag_len)) {
            page_len = string_append(g_page_buffer, "      <article class=\"article-card\" id=\"article-", page_len, MAX_PAGE_BUFFER);
            page_len = string_append_u32(g_page_buffer, rec->order, page_len, MAX_PAGE_BUFFER);
            page_len = string_append(g_page_buffer, "-", page_len, MAX_PAGE_BUFFER);
            page_len = string_append(g_page_buffer, rec->article_slug, page_len, MAX_PAGE_BUFFER);
            page_len = string_append(g_page_buffer, "\" data-order=\"", page_len, MAX_PAGE_BUFFER);
            page_len = string_append_u32(g_page_buffer, rec->order, page_len, MAX_PAGE_BUFFER);
            page_len = string_append(g_page_buffer, "\" data-title=\"", page_len, MAX_PAGE_BUFFER);
            page_len = string_append(g_page_buffer, rec->title, page_len, MAX_PAGE_BUFFER);
            page_len = string_append(g_page_buffer, "\">\n", page_len, MAX_PAGE_BUFFER);

            page_len = string_append(g_page_buffer, "        <div class=\"article-body\">\n", page_len, MAX_PAGE_BUFFER);

            /* Rewrite headings with scoped anchors */
            rewrite_fragment_headings(rec, g_fragment_buffer, g_rewritten_buffer, MAX_FRAGMENT_BUFFER);
            page_len = string_append(g_page_buffer, g_rewritten_buffer, page_len, MAX_PAGE_BUFFER);

            page_len = string_append(g_page_buffer, "\n        </div>\n", page_len, MAX_PAGE_BUFFER);
            page_len = string_append(g_page_buffer, "      </article>\n\n", page_len, MAX_PAGE_BUFFER);
        }
    }

    page_len = string_append(g_page_buffer,
        "    </main>\n\n",
        page_len, MAX_PAGE_BUFFER);

    /* Render Right Sidebar */
    page_len = render_right_sidebar(g_articles, g_article_count, g_page_buffer, page_len, MAX_PAGE_BUFFER);

    /* Toast Notification and Navigation Script */
    page_len = string_append(g_page_buffer,
        "  </div>\n\n"
        "  <div id=\"toast\">Copied URL to clipboard</div>\n\n"
        "  <script>\n"
        "    function showToast(msg) {\n"
        "      const t = document.getElementById('toast');\n"
        "      t.innerText = msg;\n"
        "      t.classList.add('show');\n"
        "      clearTimeout(window.__toastTimer);\n"
        "      window.__toastTimer = setTimeout(() => t.classList.remove('show'), 1800);\n"
        "    }\n"
        "    function copyHeadingUrl(e, id) {\n"
        "      if (e) e.preventDefault();\n"
        "      const url = window.location.origin + window.location.pathname + '#' + id;\n"
        "      navigator.clipboard.writeText(url).then(() => {\n"
        "        showToast('Copied URL to clipboard');\n"
        "        history.pushState(null, null, '#' + id);\n"
        "      }).catch(() => {\n"
        "        showToast('Failed to copy');\n"
        "      });\n"
        "    }\n"
        "    function selectArticle(order) {\n"
        "      document.querySelectorAll('.article-list-item').forEach(el => {\n"
        "        el.classList.toggle('active', el.id === 'nav-art-' + order);\n"
        "      });\n"
        "      document.querySelectorAll('.article-outline-block').forEach(el => {\n"
        "        el.classList.toggle('active', el.id === 'outline-art-' + order);\n"
        "      });\n"
        "    }\n"
        "    const observer = new IntersectionObserver((entries) => {\n"
        "      entries.forEach(entry => {\n"
        "        if (entry.isIntersecting) {\n"
        "          const order = entry.target.dataset.order;\n"
        "          if (order) selectArticle(order);\n"
        "        }\n"
        "      });\n"
        "    }, { rootMargin: '-20% 0px -70% 0px' });\n"
        "    document.querySelectorAll('.article-card').forEach(card => observer.observe(card));\n"
        "\n"
        "    /* Syntax highlighting */\n"
        "    hljs.highlightAll();\n"
        "\n"
        "    /* Copy button on code blocks */\n"
        "    document.querySelectorAll('pre > code').forEach(block => {\n"
        "      const wrapper = block.parentElement;\n"
        "      wrapper.style.position = 'relative';\n"
        "      const btn = document.createElement('button');\n"
        "      btn.className = 'code-copy-btn';\n"
        "      btn.textContent = 'Copy';\n"
        "      btn.addEventListener('click', () => {\n"
        "        navigator.clipboard.writeText(block.textContent).then(() => {\n"
        "          btn.textContent = 'Copied!';\n"
        "          setTimeout(() => btn.textContent = 'Copy', 1500);\n"
        "        }).catch(() => {\n"
        "          showToast('Failed to copy');\n"
        "        });\n"
        "      });\n"
        "      wrapper.appendChild(btn);\n"
        "    });\n"
        "\n"
        "    /* Full-text search with word highlighting */\n"
        "    function handleSearch(value) {\n"
        "      const q = value.trim().toLowerCase();\n"
        "      document.querySelectorAll('mark.search-highlight').forEach(m => {\n"
        "        const parent = m.parentNode;\n"
        "        parent.replaceChild(document.createTextNode(m.textContent), m);\n"
        "        parent.normalize();\n"
        "      });\n"
        "      document.querySelectorAll('.article-card').forEach(card => {\n"
        "        const text = card.innerText.toLowerCase();\n"
        "        const matched = q === '' || text.includes(q);\n"
        "        card.style.display = matched ? '' : 'none';\n"
        "        if (matched && q !== '') {\n"
        "          highlightText(card.querySelector('.article-body'), q);\n"
        "        }\n"
        "      });\n"
        "      document.querySelectorAll('.article-list-item').forEach(el => {\n"
        "        const order = el.dataset.order;\n"
        "        const link = el.querySelector('.article-list-link');\n"
        "        const href = link ? link.getAttribute('href') : '';\n"
        "        const cardId = href ? href.substring(1) : '';\n"
        "        const card = cardId ? document.getElementById(cardId) : null;\n"
        "        const matched = q === '' || (card && card.style.display !== 'none');\n"
        "        el.style.display = matched ? '' : 'none';\n"
        "      });\n"
        "    }\n"
        "    function highlightText(root, query) {\n"
        "      if (!root) return;\n"
        "      const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, {\n"
        "        acceptNode: function(node) {\n"
        "          const p = node.parentElement;\n"
        "          if (!p) return NodeFilter.FILTER_REJECT;\n"
        "          const tag = p.tagName;\n"
        "          if (tag === 'SCRIPT' || tag === 'STYLE' || tag === 'MARK'\n"
        "              || tag === 'CODE' || tag === 'PRE') return NodeFilter.FILTER_REJECT;\n"
        "          return NodeFilter.FILTER_ACCEPT;\n"
        "        }\n"
        "      });\n"
        "      const nodes = [];\n"
        "      while (walker.nextNode()) nodes.push(walker.currentNode);\n"
        "      nodes.forEach(function(node) {\n"
        "        const text = node.textContent;\n"
        "        const lower = text.toLowerCase();\n"
        "        const idx = lower.indexOf(query);\n"
        "        if (idx === -1) return;\n"
        "        const frag = document.createDocumentFragment();\n"
        "        let lastIdx = 0;\n"
        "        let pos = idx;\n"
        "        while (pos !== -1) {\n"
        "          frag.appendChild(document.createTextNode(text.slice(lastIdx, pos)));\n"
        "          const mark = document.createElement('mark');\n"
        "          mark.className = 'search-highlight';\n"
        "          mark.textContent = text.slice(pos, pos + query.length);\n"
        "          frag.appendChild(mark);\n"
        "          lastIdx = pos + query.length;\n"
        "          pos = lower.indexOf(query, lastIdx);\n"
        "        }\n"
        "        frag.appendChild(document.createTextNode(text.slice(lastIdx)));\n"
        "        node.parentNode.replaceChild(frag, node);\n"
        "      });\n"
        "    }\n"
        "  </script>\n"
        "</body>\n"
        "</html>\n",
        page_len, MAX_PAGE_BUFFER);

    printf("site_generator: writing %u bytes to '%s'...\n", page_len, output_file);
    if (!write_entire_file(output_file, g_page_buffer, page_len)) {
        fprintf(stderr, "site_generator: failed to write output file '%s': %s\n", output_file, strerror(errno));
        return 1;
    }

    printf("site_generator: success! Built single-page stream with %u articles.\n", g_article_count);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Test Harness (-DTEST_SITE_GENERATOR)                                */
/* ------------------------------------------------------------------ */

#ifdef TEST_SITE_GENERATOR

static void test_parse_article_dirname(void) {
    u32 order = 0;
    char slug[MAX_NAME_LENGTH] = {0};

    assert(parse_article_dirname("1-example", &order, slug, MAX_NAME_LENGTH));
    assert(order == 1);
    assert(string_equals(slug, "example"));

    assert(parse_article_dirname("10-my_great_post", &order, slug, MAX_NAME_LENGTH));
    assert(order == 10);
    assert(string_equals(slug, "my_great_post"));

    assert(parse_article_dirname("007-james-bond", &order, slug, MAX_NAME_LENGTH));
    assert(order == 7);
    assert(string_equals(slug, "james-bond"));

    assert(!parse_article_dirname("-example", &order, slug, MAX_NAME_LENGTH));
    assert(!parse_article_dirname("example", &order, slug, MAX_NAME_LENGTH));
    assert(!parse_article_dirname("abc-def", &order, slug, MAX_NAME_LENGTH));
    assert(!parse_article_dirname("", &order, slug, MAX_NAME_LENGTH));
    assert(!parse_article_dirname("42-", &order, slug, MAX_NAME_LENGTH));
    printf("  [PASS] test_parse_article_dirname\n");
}

static void test_extract_headings(void) {
    article_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.order = 5;
    string_copy(rec.article_slug, "c-and-htmx", MAX_NAME_LENGTH);

    const char *html =
        "<h1 id=\"main-title\">Main Title</h1>\n"
        "<p>Text</p>\n"
        "<h2 id=\"sub-one\">Sub One</h2>\n"
        "<p>More</p>\n"
        "<h3>Third Level</h3>\n";

    extract_article_headings(&rec, html);

    assert(rec.heading_count == 3);
    assert(string_equals(rec.title, "Main Title"));

    /* Heading 1 */
    assert(rec.headings[0].level == 1);
    assert(string_equals(rec.headings[0].text, "Main Title"));
    assert(string_equals(rec.headings[0].id, "art-5-main-title"));

    /* Heading 2 */
    assert(rec.headings[1].level == 2);
    assert(string_equals(rec.headings[1].text, "Sub One"));
    assert(string_equals(rec.headings[1].id, "art-5-sub-one"));

    /* Heading 3 (generated ID when missing) */
    assert(rec.headings[2].level == 3);
    assert(string_equals(rec.headings[2].text, "Third Level"));
    assert(string_equals(rec.headings[2].id, "art-5-h3"));

    printf("  [PASS] test_extract_headings\n");
}

static void test_rewrite_headings(void) {
    article_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.order = 2;
    string_copy(rec.article_slug, "example", MAX_NAME_LENGTH);

    const char *src = "<h2 id=\"overview\">Overview</h2><p>Content</p>";
    extract_article_headings(&rec, src);

    char dst[1024] = {0};
    rewrite_fragment_headings(&rec, src, dst, 1024);

    assert(strstr(dst, "id=\"art-2-overview\"") != nullptr);
    assert(strstr(dst, "class=\"heading-anchor\"") != nullptr);
    assert(strstr(dst, "copyHeadingUrl(event, 'art-2-overview')") != nullptr);
    printf("  [PASS] test_rewrite_headings\n");
}

static void test_rewrite_images(void) {
    article_record rec;
    memset(&rec, 0, sizeof(rec));
    rec.order = 1;
    string_copy(rec.directory_name, "1-example", MAX_NAME_LENGTH);
    string_copy(rec.article_slug, "example", MAX_NAME_LENGTH);

    /* Relative image path resolution and figure generation */
    const char *src1 = "<p><img src=\"Screenshot.png\" alt=\"My Screenshot\"></p>";
    char dst1[1024] = {0};
    rewrite_fragment_headings(&rec, src1, dst1, 1024);

    assert(strstr(dst1, "<figure class=\"article-figure\">") != nullptr);
    assert(strstr(dst1, "src=\"articles/1-example/Screenshot.png\"") != nullptr);
    assert(strstr(dst1, "<figcaption class=\"image-caption\">My Screenshot</figcaption>") != nullptr);

    /* Absolute image path remains unchanged */
    const char *src2 = "<img src=\"https://example.com/photo.jpg\" alt=\"External\">";
    char dst2[1024] = {0};
    rewrite_fragment_headings(&rec, src2, dst2, 1024);

    assert(strstr(dst2, "src=\"https://example.com/photo.jpg\"") != nullptr);

    /* Relative with ./ */
    const char *src3 = "<img src=\"./diagram.png\" alt=\"\">";
    char dst3[1024] = {0};
    rewrite_fragment_headings(&rec, src3, dst3, 1024);

    assert(strstr(dst3, "src=\"articles/1-example/diagram.png\"") != nullptr);
    /* No alt text -> no figcaption */
    assert(strstr(dst3, "<figcaption") == nullptr);

    printf("  [PASS] test_rewrite_images\n");
}

static void test_sort_articles_descending(void) {
    article_record records[5];
    memset(records, 0, sizeof(records));
    records[0].order = 3;
    records[1].order = 1;
    records[2].order = 10;
    records[3].order = 2;
    records[4].order = 7;

    sort_articles_descending(records, 5);

    assert(records[0].order == 10);
    assert(records[1].order == 7);
    assert(records[2].order == 3);
    assert(records[3].order == 2);
    assert(records[4].order == 1);

    sort_articles_descending(records, 1);
    assert(records[0].order == 10);

    sort_articles_descending(nullptr, 0);
    printf("  [PASS] test_sort_articles_descending\n");
}

static void test_string_helpers(void) {
    assert(string_length("") == 0);
    assert(string_length("Hello") == 5);
    assert(string_length("Antigravity") == 11);

    char buf[32] = {0};
    u32 len = 0;

    len = string_append(buf, "Hello", len, 32);
    assert(len == 5);
    assert(string_equals(buf, "Hello"));

    len = string_append(buf, " ", len, 32);
    len = string_append_u32(buf, 42, len, 32);
    assert(string_equals(buf, "Hello 42"));

    char small_buf[6] = {0};
    u32 small_len = 0;
    small_len = string_append(small_buf, "123456789", small_len, 6);
    assert(small_len == 5);
    assert(string_equals(small_buf, "12345"));

    printf("  [PASS] test_string_helpers\n");
}

int main(void) {
    printf("Running site_generator test suite...\n");
    test_string_helpers();
    test_parse_article_dirname();
    test_extract_headings();
    test_rewrite_headings();
    test_rewrite_images();
    test_sort_articles_descending();
    printf("All site_generator tests PASSED!\n");
    return 0;
}

#else

int main(int argc, char **argv) {
    const char *articles_dir = "articles";
    const char *output_file = "index.html";

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--articles") == 0 || strcmp(argv[i], "-a") == 0) && i + 1 < argc) {
            articles_dir = argv[++i];
        } else if ((strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0) && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: site_generator [options]\n");
            printf("Options:\n");
            printf("  -a, --articles DIR   Articles directory (default: articles)\n");
            printf("  -o, --output FILE    Output HTML file (default: index.html)\n");
            printf("  -h, --help           Show this help message\n");
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    return generate_site(articles_dir, output_file);
}

#endif
