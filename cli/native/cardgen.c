/* cardgen.c — Nikon body-screen watermark card, C port of watermark.py (S11).
 *
 * Renders a "see-through camera" card: blurred background -> photo pasted
 * into the body's LCD window -> body overlay -> two EXIF text lines -> JPEG.
 *
 * Self-contained: stb_image/stb_image_resize/stb_image_write/stb_truetype
 * (public domain / MIT) + bundled OFL fonts. No Python, no system deps.
 *
 * EXIF is read from the input JPEG unless overridden with --model/--lens/
 * --focal-mm/--fnum/--expo/--iso (the Lightroom plugin uses these for
 * EXIF-less previews). Missing values are a hard error — never guessed.
 *
 * Build:  cc -O2 -o cardgen cardgen.c -lm     (see ../build.sh)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <dirent.h>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#    include <sys/types.h>
#  endif
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PSD
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

/* ------------------------------------------------------------------ utils */

static void die(const char *msg, const char *arg)
{
    if (arg) fprintf(stderr, "error: %s: %s\n", msg, arg);
    else     fprintf(stderr, "error: %s\n", msg);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory", NULL);
    return p;
}

static char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *s = xmalloc(la + lb + 2);
    memcpy(s, a, la);
    s[la] = '/';
    memcpy(s + la + 1, b, lb + 1);
    return s;
}

static char *read_file(const char *path, size_t *out_len)
{
#ifdef _WIN32
    /* UTF-8 -> UTF-16 so paths with non-ASCII (zh-CN usernames) work. */
    int wl = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wpath = xmalloc((size_t)wl * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wl);
    FILE *f = _wfopen(wpath, L"rb");
    free(wpath);
#else
    FILE *f = fopen(path, "rb");
#endif
    if (!f) die("cannot open file", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) die("short read", path);
    buf[n] = 0;
    fclose(f);
    if (out_len) *out_len = (size_t)n;
    return buf;
}

static int write_file(const char *path, const unsigned char *data, size_t len)
{
#ifdef _WIN32
    int wl = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wpath = xmalloc((size_t)wl * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wl);
    FILE *f = _wfopen(wpath, L"wb");
    free(wpath);
#else
    FILE *f = fopen(path, "wb");
#endif
    if (!f) return 0;
    int ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static void mkdir_p(const char *path)
{
#ifdef _WIN32
    int wl = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wpath = xmalloc((size_t)wl * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wl);
    CreateDirectoryW(wpath, NULL); /* EEXIST is fine */
    free(wpath);
#else
    mkdir(path, 0755);
#endif
}

/* mkdir for the parent of a file path ("a/b/c.jpg" -> "a/b") */
static void mkdirs_for_file(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) return;
    size_t n = (size_t)(slash - path);
    if (!n) return;
    char *dir = xmalloc(n + 1);
    memcpy(dir, path, n);
    dir[n] = 0;
    /* create each component */
    for (char *p = dir; *p; p++) {
        if (*p == '/' && p != dir) {
            *p = 0;
            mkdir_p(dir);
            *p = '/';
        }
    }
    mkdir_p(dir);
    free(dir);
}

typedef struct { char **items; int count, cap; } StrList;

static void sl_push(StrList *l, const char *s)
{
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, (size_t)l->cap * sizeof(char *));
        if (!l->items) die("out of memory", NULL);
    }
    l->items[l->count++] = strdup(s);
}

/* list sub-directories of dir (UTF-8 names in and out) */
static void list_dirs(const char *dir, StrList *out)
{
#ifdef _WIN32
    char *pat = xmalloc(strlen(dir) + 4);
    sprintf(pat, "%s/*", dir);
    int wl = MultiByteToWideChar(CP_UTF8, 0, pat, -1, NULL, 0);
    wchar_t *wpat = xmalloc((size_t)wl * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, pat, -1, wpat, wl);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    free(pat); free(wpat);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
                int bl = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, NULL, 0, NULL, NULL);
                char *name = xmalloc((size_t)bl);
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, bl, NULL, NULL);
                sl_push(out, name);
                free(name);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char *full = path_join(dir, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) sl_push(out, e->d_name);
        free(full);
    }
    closedir(d);
#endif
}

static int file_exists(const char *path)
{
#ifdef _WIN32
    int wl = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wpath = xmalloc((size_t)wl * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wl);
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
#endif
}

/* ------------------------------------------------------- mini JSON parser */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
struct JVal {
    JType type;
    double num;
    int boolean;
    char *str;          /* J_STR: decoded string (owned) */
    char **keys;        /* J_OBJ */
    JVal **vals;
    int n, cap;
};

typedef struct { const char *p, *end; } JParser;

static void jskip(JParser *j)
{
    while (j->p < j->end && isspace((unsigned char)*j->p)) j->p++;
}

static JVal *jnew(JType t)
{
    JVal *v = xmalloc(sizeof(JVal));
    memset(v, 0, sizeof(JVal));
    v->type = t;
    return v;
}

static JVal *jparse(JParser *j);

static char *jparse_string_raw(JParser *j)
{
    if (j->p >= j->end || *j->p != '"') die("JSON: expected string", NULL);
    j->p++;
    size_t cap = 16, len = 0;
    char *s = xmalloc(cap);
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '\\' && j->p < j->end) {
            char e = *j->p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': { /* \uXXXX -> UTF-8 (BMP only; profiles are ASCII) */
                unsigned cp = 0;
                for (int i = 0; i < 4 && j->p < j->end; i++) {
                    char h = *j->p++;
                    cp = cp * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                }
                if (len + 4 >= cap) { cap = cap * 2 + 4; s = realloc(s, cap); }
                if (cp < 0x80) s[len++] = (char)cp;
                else if (cp < 0x800) { s[len++] = (char)(0xC0 | cp >> 6); s[len++] = (char)(0x80 | (cp & 0x3F)); }
                else { s[len++] = (char)(0xE0 | cp >> 12); s[len++] = (char)(0x80 | ((cp >> 6) & 0x3F)); s[len++] = (char)(0x80 | (cp & 0x3F)); }
                continue;
            }
            default: c = (unsigned char)e; break;
            }
        }
        if (len + 2 >= cap) { cap = cap * 2; s = realloc(s, cap); if (!s) die("out of memory", NULL); }
        s[len++] = (char)c;
    }
    if (j->p < j->end) j->p++; /* closing quote */
    s[len] = 0;
    return s;
}

static JVal *jparse(JParser *j)
{
    jskip(j);
    if (j->p >= j->end) die("JSON: unexpected end", NULL);
    char c = *j->p;
    if (c == '{') {
        j->p++;
        JVal *v = jnew(J_OBJ);
        jskip(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return v; }
        for (;;) {
            jskip(j);
            char *key = jparse_string_raw(j);
            jskip(j);
            if (j->p >= j->end || *j->p != ':') die("JSON: expected ':'", NULL);
            j->p++;
            JVal *val = jparse(j);
            if (v->n == v->cap) {
                v->cap = v->cap ? v->cap * 2 : 8;
                v->keys = realloc(v->keys, (size_t)v->cap * sizeof(char *));
                v->vals = realloc(v->vals, (size_t)v->cap * sizeof(JVal *));
                if (!v->keys || !v->vals) die("out of memory", NULL);
            }
            v->keys[v->n] = key;
            v->vals[v->n] = val;
            v->n++;
            jskip(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == '}') { j->p++; return v; }
            die("JSON: expected ',' or '}'", NULL);
        }
    }
    if (c == '[') {
        j->p++;
        JVal *v = jnew(J_ARR);
        jskip(j);
        if (j->p < j->end && *j->p == ']') { j->p++; return v; }
        for (;;) {
            JVal *val = jparse(j);
            if (v->n == v->cap) {
                v->cap = v->cap ? v->cap * 2 : 8;
                v->vals = realloc(v->vals, (size_t)v->cap * sizeof(JVal *));
                if (!v->vals) die("out of memory", NULL);
            }
            v->vals[v->n++] = val;
            jskip(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == ']') { j->p++; return v; }
            die("JSON: expected ',' or ']'", NULL);
        }
    }
    if (c == '"') {
        JVal *v = jnew(J_STR);
        v->str = jparse_string_raw(j);
        return v;
    }
    if (!strncmp(j->p, "true", 4) && j->end - j->p >= 4)  { j->p += 4; JVal *v = jnew(J_BOOL); v->boolean = 1; return v; }
    if (!strncmp(j->p, "false", 5) && j->end - j->p >= 5) { j->p += 5; JVal *v = jnew(J_BOOL); return v; }
    if (!strncmp(j->p, "null", 4) && j->end - j->p >= 4)  { j->p += 4; return jnew(J_NULL); }
    /* number */
    {
        char *endp = NULL;
        double d = strtod(j->p, &endp);
        if (endp == j->p) die("JSON: bad value", NULL);
        j->p = endp;
        JVal *v = jnew(J_NUM);
        v->num = d;
        return v;
    }
}

static JVal *jget(JVal *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ) return NULL;
    for (int i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) return obj->vals[i];
    return NULL;
}

/* -------------------------------------------------------------- EXIF read */

typedef struct {
    char model[128], lens[128];
    double focal, fnum, expo;
    long iso;
    int orientation; /* 1..8, default 1 */
    int have_model, have_lens, have_focal, have_fnum, have_expo, have_iso;
} Exif;

static unsigned rd16(const unsigned char *p, int be)
{
    return be ? (unsigned)((p[0] << 8) | p[1]) : (unsigned)((p[1] << 8) | p[0]);
}
static unsigned rd32(const unsigned char *p, int be)
{
    return be ? ((unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3])
              : ((unsigned)p[3] << 24 | (unsigned)p[2] << 16 | (unsigned)p[1] << 8 | p[0]);
}

static void exif_copy_str(char *dst, size_t cap, const unsigned char *base,
                          const unsigned char *valp, unsigned count)
{
    size_t n = count > 0 ? (size_t)count - 1 : 0; /* drop NUL */
    if (n >= cap) n = cap - 1;
    memcpy(dst, valp, n);
    dst[n] = 0;
    /* trim */
    char *e = dst + strlen(dst);
    while (e > dst && isspace((unsigned char)e[-1])) *--e = 0;
    for (char *s = dst; *s && isspace((unsigned char)*s); s++) memmove(dst, s, strlen(s) + 1);
}

static void parse_ifd(Exif *ex, const unsigned char *base, size_t len,
                      const unsigned char *ifd, int be, int is_exif_ifd)
{
    if (ifd + 2 > base + len) return;
    unsigned n = rd16(ifd, be);
    for (unsigned i = 0; i < n; i++) {
        const unsigned char *e = ifd + 2 + (size_t)i * 12;
        if (e + 12 > base + len) return;
        unsigned tag = rd16(e, be);
        unsigned type = rd16(e + 2, be);
        unsigned count = rd32(e + 4, be);
        const unsigned char *valp = e + 8;
        unsigned bytes;
        switch (type) { /* sizes for types we use */
        case 1: case 2: case 6: case 7: bytes = count; break; /* BYTE/ASCII/UNDEFINED */
        case 3: bytes = count * 2; break;                     /* SHORT */
        case 4: case 9: bytes = count * 4; break;             /* LONG/SLONG */
        case 5: case 10: bytes = count * 8; break;            /* RATIONAL/SRATIONAL */
        default: bytes = 0; break;
        }
        const unsigned char *val;
        if (bytes > 4) {
            unsigned off = rd32(e + 8, be);
            val = base + off;
            if (val + bytes > base + len) continue;
        } else {
            val = valp;
        }
        if (!is_exif_ifd) {
            if (tag == 272) { exif_copy_str(ex->model, sizeof ex->model, base, val, count); ex->have_model = 1; }
            if (tag == 274) { ex->orientation = (int)rd16(val, be); }
            continue;
        }
        switch (tag) {
        case 37386: if (type == 5 && count >= 1) {
                        double num = rd32(val, be), den = rd32(val + 4, be);
                        if (den) { ex->focal = num / den; ex->have_focal = 1; } } break;
        case 33437: if (type == 5 && count >= 1) {
                        double num = rd32(val, be), den = rd32(val + 4, be);
                        if (den) { ex->fnum = num / den; ex->have_fnum = 1; } } break;
        case 33434: if (type == 5 && count >= 1) {
                        double num = rd32(val, be), den = rd32(val + 4, be);
                        if (den) { ex->expo = num / den; ex->have_expo = 1; } } break;
        case 34855: if ((type == 3 || type == 4) && count >= 1) {
                        ex->iso = (long)(type == 3 ? rd16(val, be) : rd32(val, be));
                        ex->have_iso = 1; } break;
        case 42036: exif_copy_str(ex->lens, sizeof ex->lens, base, val, count); ex->have_lens = 1; break;
        default: break;
        }
    }
}

static void read_exif_jpeg(const char *path, Exif *ex)
{
    memset(ex, 0, sizeof *ex);
    ex->orientation = 1;
    size_t len;
    unsigned char *buf = (unsigned char *)read_file(path, &len);
    size_t i = 2; /* skip FFD8 */
    while (i + 4 <= len) {
        if (buf[i] != 0xFF) break;
        unsigned char marker = buf[i + 1];
        if (marker == 0xE1 && i + 6 < len && !memcmp(buf + i + 4, "Exif\0\0", 6)) {
            const unsigned char *tiff = buf + i + 10;
            size_t tiff_len = len - (size_t)(tiff - buf);
            if (tiff_len < 8) break;
            int be;
            if (!memcmp(tiff, "II*\0", 4)) be = 0;
            else if (!memcmp(tiff, "MM\0*", 4)) be = 1;
            else break;
            unsigned ifd0_off = rd32(tiff + 4, be);
            const unsigned char *ifd0 = tiff + ifd0_off;
            parse_ifd(ex, tiff, tiff_len, ifd0, be, 0);
            /* find ExifIFD pointer (0x8769) */
            unsigned n = rd16(ifd0, be);
            for (unsigned k = 0; k < n && ifd0 + 2 + (size_t)k * 12 + 12 <= tiff + tiff_len; k++) {
                const unsigned char *e = ifd0 + 2 + (size_t)k * 12;
                if (rd16(e, be) == 0x8769) {
                    parse_ifd(ex, tiff, tiff_len, tiff + rd32(e + 8, be), be, 1);
                    break;
                }
            }
            break;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) { i += 2; continue; }
        if (marker == 0xDA || marker == 0xD9) break; /* SOS/EOI */
        unsigned seglen = (unsigned)((buf[i + 2] << 8) | buf[i + 3]);
        i += 2 + seglen;
    }
    free(buf);
}

/* --------------------------------------------------------------- images */

typedef struct { int w, h, c; unsigned char *d; } Img;

static Img img_new(int w, int h, int c)
{
    Img im = { w, h, c, xmalloc((size_t)w * h * c) };
    memset(im.d, 0, (size_t)w * h * c);
    return im;
}

static Img img_load(const char *path)
{
    int w, h, c;
    unsigned char *d = stbi_load(path, &w, &h, &c, 0);
    if (!d) {
        /* retry via manual read for non-ASCII paths on Windows */
        size_t n;
        unsigned char *buf = (unsigned char *)read_file(path, &n);
        d = stbi_load_from_memory(buf, (int)n, &w, &h, &c, 0);
        free(buf);
    }
    if (!d) die("cannot decode image", path);
    Img im = { w, h, c, d };
    return im;
}

static Img img_load_memory(const unsigned char *buf, int len)
{
    int w, h, c;
    unsigned char *d = stbi_load_from_memory(buf, len, &w, &h, &c, 0);
    if (!d) die("cannot decode image", NULL);
    Img im = { w, h, c, d };
    return im;
}

/* High-quality resize: iterative halving (box) then final Catmull-Rom.
 * Approximates Pillow's LANCZOS for large downscales without aliasing.
 * Does NOT free (*src).d - the caller keeps ownership (photo is reused). */
static Img img_resize(const Img *src, int nw, int nh, int nc)
{
    Img work = *src;   /* copy of the header; caller owns work.d until replaced */
    int owned = 0;     /* whether the CURRENT work.d was allocated in here */
    if (nc == 4 && work.c == 3) {
        Img tmp = img_new(work.w, work.h, 4);
        for (size_t i = 0; i < (size_t)work.w * work.h; i++) {
            memcpy(tmp.d + i * 4, work.d + i * 3, 3);
            tmp.d[i * 4 + 3] = 255;
        }
        work = tmp;
        owned = 1;
    }
    while (work.w / 2 >= nw * 2 && work.h / 2 >= nh * 2 && work.w > 2 && work.h > 2) {
        int hw = work.w / 2, hh = work.h / 2;
        Img dst = img_new(hw, hh, work.c);
        if (!stbir_resize_uint8_generic(work.d, work.w, work.h, 0, dst.d, hw, hh, 0,
                work.c, work.c == 4 ? 3 : -1, 0, STBIR_EDGE_CLAMP,
                STBIR_FILTER_BOX, STBIR_COLORSPACE_SRGB, NULL))
            die("resize failed", NULL);
        if (owned) free(work.d);
        work = dst;
        owned = 1;
    }
    Img dst = img_new(nw, nh, nc);
    if (!stbir_resize_uint8_generic(work.d, work.w, work.h, 0, dst.d, nw, nh, 0,
            nc, nc == 4 ? 3 : -1, 0, STBIR_EDGE_CLAMP,
            STBIR_FILTER_CATMULLROM, STBIR_COLORSPACE_SRGB, NULL))
        die("resize failed", NULL);
    if (owned) free(work.d);
    return dst;
}

static Img img_rotate90ccw(Img src)
{
    Img dst = img_new(src.h, src.w, src.c); /* dims swapped */
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++)
            memcpy(dst.d + ((size_t)(src.w - 1 - x) * dst.w + y) * src.c,
                   src.d + ((size_t)y * src.w + x) * src.c, (size_t)src.c);
    stbi_image_free(src.d);
    return dst;
}

static Img img_rotate90cw(Img src)
{
    Img dst = img_new(src.h, src.w, src.c);
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++)
            memcpy(dst.d + ((size_t)x * dst.w + (src.h - 1 - y)) * src.c,
                   src.d + ((size_t)y * src.w + x) * src.c, (size_t)src.c);
    stbi_image_free(src.d);
    return dst;
}

static Img img_flip_h(Img src)
{
    Img dst = img_new(src.w, src.h, src.c);
    for (int y = 0; y < src.h; y++)
        for (int x = 0; x < src.w; x++)
            memcpy(dst.d + ((size_t)y * src.w + (src.w - 1 - x)) * src.c,
                   src.d + ((size_t)y * src.w + x) * src.c, (size_t)src.c);
    stbi_image_free(src.d);
    return dst;
}

static Img img_flip_v(Img src)
{
    Img dst = img_new(src.w, src.h, src.c);
    for (int y = 0; y < src.h; y++)
        memcpy(dst.d + (size_t)y * src.w * src.c,
               src.d + (size_t)(src.h - 1 - y) * src.w * src.c,
               (size_t)src.w * src.c);
    stbi_image_free(src.d);
    return dst;
}

/* EXIF orientation -> upright (Pillow exif_transpose semantics) */
static Img img_exif_transpose(Img src, int orientation)
{
    switch (orientation) {
    case 2: return img_flip_h(src);                       /* flip left-right */
    case 3: return img_rotate90cw(img_rotate90cw(src));   /* 180 */
    case 4: return img_flip_v(src);                       /* flip top-bottom */
    case 5: return img_flip_h(img_rotate90ccw(src));      /* transpose */
    case 6: return img_rotate90cw(src);                   /* 90 CW to fix */
    case 7: return img_flip_h(img_rotate90cw(src));       /* transverse */
    case 8: return img_rotate90ccw(src);                  /* 270 */
    default: return src;                                  /* 1 and unknown */
    }
}

/* separable gaussian blur (in place, RGB or RGBA) */
static void img_gaussian_blur(Img *im, double sigma)
{
    if (sigma < 0.3) return;
    int k = (int)ceil(sigma * 3);
    int kw = k * 2 + 1;
    double *ker = xmalloc((size_t)kw * sizeof(double));
    double sum = 0;
    for (int i = 0; i < kw; i++) {
        double x = i - k;
        ker[i] = exp(-(x * x) / (2 * sigma * sigma));
        sum += ker[i];
    }
    for (int i = 0; i < kw; i++) ker[i] /= sum;

    int c = im->c, W = im->w, H = im->h;
    double *tmp = xmalloc((size_t)W * H * c * sizeof(double));
    for (size_t i = 0; i < (size_t)W * H * c; i++) tmp[i] = im->d[i];

    /* horizontal */
    double *mid = xmalloc((size_t)W * H * c * sizeof(double));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int ch = 0; ch < c; ch++) {
                double acc = 0;
                for (int i = -k; i <= k; i++) {
                    int xx = x + i;
                    if (xx < 0) xx = 0; else if (xx >= W) xx = W - 1;
                    acc += tmp[(size_t)y * W * c + (size_t)xx * c + ch] * ker[i + k];
                }
                mid[(size_t)y * W * c + (size_t)x * c + ch] = acc;
            }
    /* vertical */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int ch = 0; ch < c; ch++) {
                double acc = 0;
                for (int i = -k; i <= k; i++) {
                    int yy = y + i;
                    if (yy < 0) yy = 0; else if (yy >= H) yy = H - 1;
                    acc += mid[(size_t)yy * W * c + (size_t)x * c + ch] * ker[i + k];
                }
                double v = acc + 0.5;
                if (v < 0) v = 0; if (v > 255) v = 255;
                im->d[(size_t)y * W * c + (size_t)x * c + ch] = (unsigned char)v;
            }
    free(tmp); free(mid); free(ker);
}

static void img_brightness(Img *im, double f)
{
    for (size_t i = 0; i < (size_t)im->w * im->h * im->c; i += im->c)
        for (int ch = 0; ch < 3 && ch < im->c; ch++) {
            int v = (int)(im->d[i + ch] * f + 0.5);
            im->d[i + ch] = (unsigned char)(v > 255 ? 255 : v);
        }
}

/* paste RGBA `src` over `dst` at (dx,dy), alpha-composited (Pillow paste w/ mask) */
static void paste_rgba(Img *dst, const Img *src, int dx, int dy)
{
    for (int y = 0; y < src->h; y++) {
        int dyy = dy + y;
        if (dyy < 0 || dyy >= dst->h) continue;
        for (int x = 0; x < src->w; x++) {
            int dxx = dx + x;
            if (dxx < 0 || dxx >= dst->w) continue;
            const unsigned char *s = src->d + ((size_t)y * src->w + x) * 4;
            unsigned char *d = dst->d + ((size_t)dyy * dst->w + dxx) * dst->c;
            int a = s[3];
            if (a == 0) continue;
            for (int ch = 0; ch < 3 && ch < dst->c; ch++)
                d[ch] = (unsigned char)((s[ch] * a + d[ch] * (255 - a) + 127) / 255);
        }
    }
}

/* paste RGB `src` at (dx,dy) only inside rect mask (x0,y0,x1,y1) of DST */
static void paste_rgb_masked(Img *dst, const Img *src, int dx, int dy,
                             int mx0, int my0, int mx1, int my1)
{
    for (int y = 0; y < src->h; y++) {
        int dyy = dy + y;
        if (dyy < my0 || dyy >= my1 || dyy >= dst->h) continue;
        for (int x = 0; x < src->w; x++) {
            int dxx = dx + x;
            if (dxx < mx0 || dxx >= mx1 || dxx >= dst->w) continue;
            const unsigned char *s = src->d + ((size_t)y * src->w + x) * src->c;
            unsigned char *d = dst->d + ((size_t)dyy * dst->w + dxx) * dst->c;
            for (int ch = 0; ch < 3 && ch < dst->c; ch++) d[ch] = s[ch];
        }
    }
}

/* cover(): resize so both axes covered, then center-crop to (w,h).
 * Does NOT free src.d (caller owns it). */
static Img img_cover(const Img *src, int w, int h)
{
    double s = (w > (double)src->w * h / src->h) ? (double)w / src->w : (double)h / src->h;
    double s2 = (double)h / src->h;
    if (s < s2) s = s2;
    int rw = (int)(src->w * s + 0.5), rh = (int)(src->h * s + 0.5);
    if (rw < w) rw = w;
    if (rh < h) rh = h;
    Img r = img_resize(src, rw, rh, src->c);
    Img out = img_new(w, h, r.c);
    int cx = (r.w - w) / 2, cy = (r.h - h) / 2;
    for (int y = 0; y < h; y++)
        memcpy(out.d + (size_t)y * w * r.c,
               r.d + ((size_t)(cy + y) * r.w + cx) * r.c,
               (size_t)w * r.c);
    free(r.d);
    return out;
}

/* average color of an image (Pillow resize-to-1x1 approximation) */
static void img_average_rgb(const Img *im, unsigned char out[3])
{
    double r = 0, g = 0, b = 0;
    size_t n = (size_t)im->w * im->h;
    for (size_t i = 0; i < n; i++) {
        r += im->d[i * im->c];
        g += im->d[i * im->c + (im->c > 1 ? 1 : 0)];
        b += im->d[i * im->c + (im->c > 2 ? 2 : 0)];
    }
    out[0] = (unsigned char)(r / n + 0.5);
    out[1] = (unsigned char)(g / n + 0.5);
    out[2] = (unsigned char)(b / n + 0.5);
}

/* ------------------------------------------------------------------ fonts */

typedef struct {
    unsigned char *data;
    stbtt_fontinfo info;
} Font;

static Font font_load(const char *path)
{
    size_t n;
    unsigned char *data = (unsigned char *)read_file(path, &n);
    Font f = { data };
    if (!stbtt_InitFont(&f.info, data, 0)) die("cannot load font", path);
    return f;
}

/* UTF-8 -> codepoint (advances *s) */
static unsigned utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    if (c >= 0xF0 && c <= 0xF7) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
    else if (c >= 0xE0 && c <= 0xEF) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
    else if (c >= 0xC0 && c <= 0xDF) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
    *s = (const char *)p;
    return c;
}

static double text_width(const Font *f, const char *text, float px)
{
    float scale = stbtt_ScaleForPixelHeight(&f->info, px);
    double w = 0;
    int prev = 0;
    const char *s = text;
    while (*s) {
        unsigned c = utf8_next(&s);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f->info, (int)c, &adv, &lsb);
        if (prev) w += stbtt_GetCodepointKernAdvance(&f->info, prev, (int)c) * scale;
        w += adv * scale;
        prev = (int)c;
    }
    return w;
}

static void draw_text(Img *dst, const Font *f, const char *text, float px,
                      double x, int y_top, const unsigned char rgba[4])
{
    float scale = stbtt_ScaleForPixelHeight(&f->info, px);
    int ascent, descent, linegap;
    stbtt_GetFontVMetrics(&f->info, &ascent, &descent, &linegap);
    int baseline = y_top + (int)(ascent * scale + 0.5);
    double cx = x;
    int prev = 0;
    const char *s = text;
    while (*s) {
        unsigned c = utf8_next(&s);
        if (c == ' ') { /* just advance */ }
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&f->info, (int)c, &adv, &lsb);
        if (prev) cx += stbtt_GetCodepointKernAdvance(&f->info, prev, (int)c) * scale;
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&f->info, (int)c, scale, scale, &x0, &y0, &x1, &y1);
        if (c != ' ' && x1 > x0) {
            int gw = x1 - x0, gh = y1 - y0;
            unsigned char *bm = xmalloc((size_t)gw * gh);
            stbtt_MakeCodepointBitmap(&f->info, bm, gw, gh, gw, scale, scale, (int)c);
            for (int yy = 0; yy < gh; yy++) {
                int dyy = baseline + y0 + yy;
                if (dyy < 0 || dyy >= dst->h) continue;
                for (int xx = 0; xx < gw; xx++) {
                    int dxx = (int)cx + x0 + xx;
                    if (dxx < 0 || dxx >= dst->w) continue;
                    int a8 = bm[(size_t)yy * gw + xx];
                    if (!a8) continue;
                    unsigned char *d = dst->d + ((size_t)dyy * dst->w + dxx) * dst->c;
                    int a = a8 * rgba[3] / 255;
                    for (int ch = 0; ch < 3 && ch < dst->c; ch++)
                        d[ch] = (unsigned char)((rgba[ch] * a + d[ch] * (255 - a) + 127) / 255);
                }
            }
            free(bm);
        }
        cx += adv * scale;
        prev = (int)c;
    }
}

/* encode JPEG into memory with a growing buffer (stb needs size upfront) */
#define JPEG_START_CAP (4u << 20)
typedef struct { unsigned char *buf; size_t len, cap; } JpgAcc;

static void jpg_acc(void *ctx, void *data, int size)
{
    JpgAcc *a = (JpgAcc *)ctx;
    if (a->len + (size_t)size > a->cap) {
        while (a->len + (size_t)size > a->cap) a->cap *= 2;
        a->buf = realloc(a->buf, a->cap);
        if (!a->buf) die("out of memory", NULL);
    }
    memcpy(a->buf + a->len, data, (size_t)size);
    a->len += (size_t)size;
}

static int write_jpeg_file(const char *path, int w, int h,
                           const unsigned char *rgb, int quality)
{
    JpgAcc a = { xmalloc(JPEG_START_CAP), 0, JPEG_START_CAP };
    int ok = stbi_write_jpg_to_func(jpg_acc, &a, w, h, 3, rgb, quality);
    if (ok && a.len) ok = write_file(path, a.buf, a.len);
    free(a.buf);
    return ok;
}

/* --------------------------------------------------------------- profiles */

typedef struct {
    char key[64];
    char png_path[512];
    double quad[4][2];
    double crop_factor;
    int gray;
} Profile;

static int load_profiles(const char *dir, Profile **out, int *out_n)
{
    StrList names = { 0 };
    list_dirs(dir, &names);
    /* sort names for deterministic "auto" matching */
    for (int i = 0; i < names.count; i++)
        for (int j = i + 1; j < names.count; j++)
            if (strcmp(names.items[i], names.items[j]) > 0) {
                char *t = names.items[i]; names.items[i] = names.items[j]; names.items[j] = t;
            }
    Profile *profs = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < names.count; i++) {
        char *pj = path_join(path_join(dir, names.items[i]), "profile.json");
        if (!file_exists(pj)) { free(pj); continue; }
        size_t len;
        char *txt = read_file(pj, &len);
        JParser jps = { txt, txt + len };
        JVal *root = jparse(&jps);
        JVal *quad = jget(root, "screen_quad");
        JVal *png = jget(root, "png");
        if (!quad || quad->type != J_ARR || quad->n < 4 || !png || png->type != J_STR) {
            fprintf(stderr, "warning: bad profile %s\n", names.items[i]);
            free(pj); free(txt); continue;
        }
        if (n == cap) { cap = cap ? cap * 2 : 8; profs = realloc(profs, (size_t)cap * sizeof(Profile)); }
        Profile *p = &profs[n];
        memset(p, 0, sizeof *p);
        snprintf(p->key, sizeof p->key, "%s", names.items[i]);
        JVal *pngv = jget(root, "png");
        char *pngfile = pngv->str;
        if (pngfile[0] == '/' || (pngfile[0] && pngfile[1] == ':'))
            snprintf(p->png_path, sizeof p->png_path, "%s", pngfile);
        else
            snprintf(p->png_path, sizeof p->png_path, "%s/%s/%s", dir, names.items[i], pngfile);
        for (int q = 0; q < 4; q++) {
            JVal *pt = quad->vals[q];
            p->quad[q][0] = pt->vals[0]->num;
            p->quad[q][1] = pt->vals[1]->num;
        }
        JVal *cf = jget(root, "crop_factor");
        p->crop_factor = cf ? cf->num : 1.0;
        JVal *gray = jget(root, "gray");
        p->gray = gray && gray->type == J_BOOL && gray->boolean;
        /* colorways: keep only if requested later; re-parse on demand */
        n++;
        free(pj); free(txt);
        (void)root;
    }
    *out = profs;
    *out_n = n;
    return n;
}

/* resolve body: --body key or auto-match model against aliases */
static int resolve_profile(const char *dir, const char *body, const char *model, Profile *out)
{
    Profile *profs; int n;
    if (!load_profiles(dir, &profs, &n)) return 0;
    if (strcmp(body, "auto")) {
        for (int i = 0; i < n; i++)
            if (!strcmp(profs[i].key, body)) { *out = profs[i]; return 1; }
        fprintf(stderr, "error: unknown body \"%s\"\n", body);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        char *pj = path_join(path_join(dir, profs[i].key), "profile.json");
        size_t len;
        char *txt = read_file(pj, &len);
        JParser jps = { txt, txt + len };
        JVal *root = jparse(&jps);
        JVal *aliases = jget(root, "aliases");
        if (aliases && aliases->type == J_ARR)
            for (int a = 0; a < aliases->n; a++)
                if (aliases->vals[a]->type == J_STR &&
                    !strcmp(aliases->vals[a]->str, model)) {
                    *out = profs[i];
                    /* apply colorway overrides if present in root */
                    free(pj); free(txt);
                    return 2; /* 2 = found via alias; caller may apply colorway */
                }
        free(pj); free(txt);
    }
    fprintf(stderr, "error: no profile matches model \"%s\"\n", model);
    return 0;
}

/* ------------------------------------------------------------------- main */

/* globals mirrored from watermark.py */
#define CANVAS_LONG 1350
#define BODY_WIDTH_FRAC 0.80
#define BODY_TOP_FRAC 0.065
#define BG_BLUR_FRAC 0.007
#define BG_RADIAL_HSCALE 2.2
#define BG_DARKEN 0.72
#define SCREEN_BRIGHTEN 1.06
#define LINE1_SIZE 34
#define LINE2_SIZE 30
#define GAP_LINE 52
#define TEXT_RGBA { 255, 255, 255, 235 }

static void fmt_g(double v, char *buf, size_t cap)
{
    snprintf(buf, cap, "%g", v);
}

static void fmt_shutter(double sec, char *buf, size_t cap)
{
    if (sec >= 1.0) snprintf(buf, cap, "%g s", sec);
    else snprintf(buf, cap, "1/%d s", (int)(1.0 / sec + 0.5));
}

static void usage(void)
{
    fputs(
"cardgen - Nikon body-screen card renderer (C port of watermark.py)\n"
"usage: cardgen <input.jpg> -o <out.jpg> [--body auto|key] [--bg soft|radial|solid]\n"
"  [--colorway key] [--focal actual|eq35] [--profiles DIR] [--fonts DIR]\n"
"  [--model S] [--lens S] [--focal-mm F] [--fnum F] [--expo F] [--iso N]\n"
"  [--rotate 0|90|180|270]   (EXIF overrides for EXIF-less previews)\n",
    stderr);
}

/* find exe directory (for bundled fonts/profiles fallback) */
static char *exe_dir(void)
{
#ifdef _WIN32
    wchar_t wbuf[1024];
    DWORD n = GetModuleFileNameW(NULL, wbuf, 1024);
    if (!n) return NULL;
    int bl = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, NULL, 0, NULL, NULL);
    char *buf = xmalloc((size_t)bl + 1);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)n, buf, bl, NULL, NULL);
    buf[bl] = 0;
#else
    char buf[1024];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n < 0) {
        uint32_t size = sizeof buf;
        if (_NSGetExecutablePath(buf, &size) != 0) return NULL;
    } else buf[n] = 0;
#endif
    char *slash = strrchr(buf, '/');
    if (slash) *slash = 0;
    return strdup(buf);
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* convert wide argv to UTF-8 */
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        char **uargv = xmalloc((size_t)argc * sizeof(char *));
        for (int i = 0; i < argc; i++) {
            int bl = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
            uargv[i] = xmalloc((size_t)bl);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, uargv[i], bl, NULL, NULL);
        }
        argv = uargv;
    }
#endif

    const char *input = NULL, *output = NULL;
    const char *body_key = "auto", *bg_mode = "soft", *colorway = NULL;
    const char *focal_mode = "actual";
    const char *a_model = NULL, *a_lens = NULL;
    double a_focal = -1, a_fnum = -1, a_expo = -1;
    long a_iso = -1;
    int a_rotate = 0;
    const char *profiles_dir = NULL, *fonts_dir = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) { input = a; continue; }
        #define NEXT() (i + 1 < argc ? argv[++i] : (usage(), exit(2), (const char *)NULL))
        if (!strcmp(a, "-o") || !strcmp(a, "--output")) output = NEXT();
        else if (!strcmp(a, "--body")) body_key = NEXT();
        else if (!strcmp(a, "--bg")) bg_mode = NEXT();
        else if (!strcmp(a, "--colorway")) colorway = NEXT();
        else if (!strcmp(a, "--focal")) focal_mode = NEXT();
        else if (!strcmp(a, "--model")) a_model = NEXT();
        else if (!strcmp(a, "--lens")) a_lens = NEXT();
        else if (!strcmp(a, "--focal-mm")) a_focal = atof(NEXT());
        else if (!strcmp(a, "--fnum")) a_fnum = atof(NEXT());
        else if (!strcmp(a, "--expo")) a_expo = atof(NEXT());
        else if (!strcmp(a, "--iso")) a_iso = atol(NEXT());
        else if (!strcmp(a, "--rotate")) a_rotate = atoi(NEXT());
        else if (!strcmp(a, "--profiles")) profiles_dir = NEXT();
        else if (!strcmp(a, "--fonts")) fonts_dir = NEXT();
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else { fprintf(stderr, "error: unknown option %s\n", a); usage(); return 2; }
    }
    if (!input) { usage(); return 2; }

    /* --- EXIF: file first, then overrides ------------------------------- */
    Exif ex;
    read_exif_jpeg(input, &ex);
    if (a_model) { snprintf(ex.model, sizeof ex.model, "%s", a_model); ex.have_model = 1; }
    if (a_lens)  { snprintf(ex.lens, sizeof ex.lens, "%s", a_lens); ex.have_lens = 1; }
    if (a_focal >= 0) ex.focal = a_focal, ex.have_focal = 1;
    if (a_fnum >= 0) ex.fnum = a_fnum, ex.have_fnum = 1;
    if (a_expo >= 0) ex.expo = a_expo, ex.have_expo = 1;
    if (a_iso >= 0) ex.iso = a_iso, ex.have_iso = 1;
    if (!ex.have_model) die("EXIF Model missing - refusing to guess", input);
    if (!ex.have_focal) die("EXIF FocalLength missing - refusing to guess", input);
    if (!ex.have_fnum) die("EXIF FNumber missing - refusing to guess", input);
    if (!ex.have_expo) die("EXIF ExposureTime missing - refusing to guess", input);
    if (!ex.have_iso) die("EXIF ISO missing - refusing to guess", input);

    /* --- profiles dir ---------------------------------------------------- */
    char *exe = exe_dir();
    if (!profiles_dir) {
        /* candidate roots: cwd, exe dir, exe dir/.. (dist layout), exe/../.. */
        char *roots[4] = { NULL, NULL, NULL, NULL };
        int nr = 0;
        roots[nr++] = NULL; /* cwd */
        if (exe) {
            roots[nr++] = exe;
            roots[nr++] = path_join(exe, "..");
            roots[nr++] = path_join(exe, "../..");
        }
        for (int i = 0; i < nr && !profiles_dir; i++) {
            char *d = roots[i] ? path_join(roots[i], "body_profiles") : strdup("body_profiles");
            if (file_exists(path_join(d, "lego")) || file_exists(path_join(d, "z63")))
                profiles_dir = d;
            else
                free(d);
        }
    }
    if (!profiles_dir) profiles_dir = "body_profiles";
    {
        char *probe = path_join(profiles_dir, "lego");
        if (!file_exists(probe) && !file_exists(path_join(profiles_dir, "z63")))
            die("no body profiles found in", profiles_dir);
        free(probe);
    }

    /* --- resolve body profile ------------------------------------------- */
    Profile prof;
    int found = resolve_profile(profiles_dir, body_key, ex.model, &prof);
    if (!found) return 1;
    if (colorway) {
        /* re-parse profile.json and apply colorway png/screen_quad override */
        char *pj = path_join(path_join(profiles_dir, prof.key), "profile.json");
        size_t len;
        char *txt = read_file(pj, &len);
        JParser jps = { txt, txt + len };
        JVal *root = jparse(&jps);
        JVal *cws = jget(root, "colorways");
        JVal *cw = cws ? jget(cws, colorway) : NULL;
        if (!cw) {
            fprintf(stderr, "error: no colorway \"%s\" for %s\n", colorway, prof.key);
            return 1;
        }
        JVal *png = jget(cw, "png");
        if (png && png->type == J_STR)
            snprintf(prof.png_path, sizeof prof.png_path, "%s/%s/%s",
                     profiles_dir, prof.key, png->str);
        JVal *q = jget(cw, "screen_quad");
        if (q && q->type == J_ARR && q->n >= 4)
            for (int i = 0; i < 4; i++) {
                prof.quad[i][0] = q->vals[i]->vals[0]->num;
                prof.quad[i][1] = q->vals[i]->vals[1]->num;
            }
        free(pj); free(txt);
    }

    /* --- fonts dir -------------------------------------------------------- */
    if (!fonts_dir) {
        /* candidates: env override, cwd/native/fonts, exe/fonts, exe/../native/fonts */
        if (getenv("CARDGEN_FONTS")) fonts_dir = getenv("CARDGEN_FONTS");
        else {
            char *cands[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
            int n = 0;
            cands[n++] = "fonts";                     /* cwd/native build layout */
            cands[n++] = "native/fonts";              /* repo root cwd */
            if (exe) {
                cands[n++] = path_join(exe, "fonts");           /* dist: bin/ + fonts/ */
                cands[n++] = path_join(exe, "../native/fonts"); /* build dir next to native/ */
            }
            for (int i = 0; i < n && !fonts_dir; i++) {
                char *probe = path_join(cands[i], "JetBrainsMono-Regular.ttf");
                if (file_exists(probe)) fonts_dir = cands[i];
                else free(probe);
            }
        }
    }
    if (!fonts_dir) die("fonts dir not found (set CARDGEN_FONTS)", NULL);
    char *sans_path = path_join(fonts_dir, "InterVariable.ttf");
    char *mono_path = path_join(fonts_dir, "JetBrainsMono-Regular.ttf");
    if (!file_exists(sans_path)) die("font missing", sans_path);
    if (!file_exists(mono_path)) die("font missing", mono_path);
    Font font_sans = font_load(sans_path);
    Font font_mono = font_load(mono_path);

    /* --- photo ------------------------------------------------------------ */
    Img photo = img_load(input);
    if (photo.c < 3) die("input is not an RGB image", input);
    if (a_rotate) {
        /* clockwise degrees to restore upright (LR preview semantics) */
        if (a_rotate == 90) photo = img_rotate90cw(photo);
        else if (a_rotate == 180) photo = img_flip_v(photo);
        else if (a_rotate == 270) photo = img_rotate90ccw(photo);
    } else {
        photo = img_exif_transpose(photo, ex.orientation);
    }

    /* canvas mirrors the photo's aspect ratio exactly */
    int canvas_w, canvas_h;
    if (photo.w >= photo.h) {
        canvas_w = CANVAS_LONG;
        canvas_h = (int)(CANVAS_LONG * (double)photo.h / photo.w + 0.5);
        if (canvas_h < 1) canvas_h = 1;
    } else {
        canvas_h = CANVAS_LONG;
        canvas_w = (int)(CANVAS_LONG * (double)photo.w / photo.h + 0.5);
        if (canvas_w < 1) canvas_w = 1;
    }
    Img canvas = img_new(canvas_w, canvas_h, 3);

    /* --- body ------------------------------------------------------------ */
    Img body = img_load(prof.png_path);
    if (body.c != 4) {
        /* convert to RGBA */
        Img t = img_new(body.w, body.h, 4);
        for (size_t i = 0; i < (size_t)body.w * body.h; i++) {
            memcpy(t.d + i * 4, body.d + i * body.c, 3);
            t.d[i * 4 + 3] = 255;
        }
        stbi_image_free(body.d);
        body = t;
    }
    if (prof.gray) {
        /* low-contrast gray look (Pillow: Color .25, Contrast .72, Brightness .96) */
        double lum_sum = 0;
        size_t npx = (size_t)body.w * body.h;
        for (size_t i = 0; i < npx; i++) {
            const unsigned char *p = body.d + i * 4;
            lum_sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
        }
        double mean = lum_sum / (double)npx;
        for (size_t i = 0; i < npx; i++) {
            unsigned char *p = body.d + i * 4;
            for (int ch = 0; ch < 3; ch++) {
                double lum = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                double v = lum + (p[ch] - lum) * 0.25;   /* saturation 0.25 */
                v = (v - mean) * 0.72 + mean;             /* contrast 0.72 */
                v *= 0.96;                                /* brightness 0.96 */
                if (v < 0) v = 0; if (v > 255) v = 255;
                p[ch] = (unsigned char)(v + 0.5);
            }
        }
    }

    /* punch the screen window out of the body alpha (EXACTLY the quad) */
    double ql = 1e9, qt = 1e9, qr = -1e9, qb = -1e9;
    for (int i = 0; i < 4; i++) {
        if (prof.quad[i][0] < ql) ql = prof.quad[i][0];
        if (prof.quad[i][0] > qr) qr = prof.quad[i][0];
        if (prof.quad[i][1] < qt) qt = prof.quad[i][1];
        if (prof.quad[i][1] > qb) qb = prof.quad[i][1];
    }
    int iql = (int)ql, iqt = (int)qt, iqr = (int)qr - 1, iqb = (int)qb - 1;
    for (int y = iqt; y <= iqb; y++)
        for (int x = iql; x <= iqr; x++)
            if (x >= 0 && x < body.w && y >= 0 && y < body.h)
                body.d[((size_t)y * body.w + x) * 4 + 3] = 0;

    /* alpha bbox (silhouette) for the bleed clamp */
    int bl = body.w, bt = body.h, br = -1, bb = -1;
    for (int y = 0; y < body.h; y++)
        for (int x = 0; x < body.w; x++)
            if (body.d[((size_t)y * body.w + x) * 4 + 3] > 10) {
                if (x < bl) bl = x;
                if (x > br) br = x;
                if (y < bt) bt = y;
                if (y > bb) bb = y;
            }
    if (br < 0) { bl = 0; bt = 0; br = body.w - 1; bb = body.h - 1; }

    /* paste quad: quad + anti-seam bleed, clamped to the silhouette */
    int exp = 2;
    double el = exp < ql - bl ? exp : ql - bl;
    double et = exp < qt - bt ? exp : qt - bt;
    double er = exp < (br + 1) - qr ? exp : (br + 1) - qr;
    double eb = exp < (bb + 1) - qb ? exp : (bb + 1) - qb;
    double quad[4][2] = {
        { ql - el, qt - et }, { qr + er, qt - et },
        { qr + er, qb + eb }, { ql - el, qb + eb },
    };
    double window_quad[4][2] = {
        { ql, qt }, { qr, qt }, { qr, qb }, { ql, qb },
    };

    /* portrait canvas: stand the body upright (rotate CCW, grip at top) */
    int bw0 = body.w, bh0 = body.h;
    if (canvas_h >= canvas_w) {
        body = img_rotate90ccw(body);
        for (int i = 0; i < 4; i++) {
            double x = quad[i][0], y = quad[i][1];
            quad[i][0] = y;
            quad[i][1] = bw0 - x;
            double wx = window_quad[i][0], wy = window_quad[i][1];
            window_quad[i][0] = wy;
            window_quad[i][1] = bw0 - wx;
        }
        int t = bw0; bw0 = bh0; bh0 = t;
    }
    int orig_w = bw0, orig_h = bh0;

    /* fit: 80% width cap + height cap reserving the text zone */
    int top_margin = (int)(canvas_h * BODY_TOP_FRAC + 0.5);
    int text_zone = 56 + GAP_LINE + LINE2_SIZE + 30;
    double s_body = (canvas_w * BODY_WIDTH_FRAC) / orig_w;
    {
        double sh = (double)(canvas_h - top_margin - text_zone) / body.h;
        if (sh < s_body) s_body = sh;
    }
    int bw = (int)(orig_w * s_body + 0.5);
    int bh = (int)(body.h * s_body + 0.5);
    Img body_r = img_resize(&body, bw, bh, 4);
    stbi_image_free(body.d);
    body = body_r;
    int bx = (canvas_w - bw) / 2;
    int by = top_margin + (canvas_h - top_margin - text_zone - bh) / 2;

    /* --- background ------------------------------------------------------- */
    Img bg = img_cover(&photo, canvas_w, canvas_h);
    if (!strcmp(bg_mode, "solid")) {
        unsigned char avg[3];
        img_average_rgb(&photo, avg);
        for (size_t i = 0; i < (size_t)canvas_w * canvas_h; i++)
            memcpy(bg.d + i * 3, avg, 3);
    } else if (!strcmp(bg_mode, "radial")) {
        int sw = canvas_w / BG_RADIAL_HSCALE > 1 ? (int)(canvas_w / BG_RADIAL_HSCALE + 0.5) : 1;
        Img squeezed = img_resize(&bg, sw, canvas_h, 3);
        img_gaussian_blur(&squeezed, canvas_w * BG_BLUR_FRAC);
        Img blurred = img_resize(&squeezed, canvas_w, canvas_h, 3);
        stbi_image_free(squeezed.d);
        /* radial mask: 0 at center region, 255 at rim; composite blurred over bg */
        double cx = bx + bw / 2.0, cy = by + bh / 2.0;
        double r_max = hypot(cx > canvas_w - cx ? cx : canvas_w - cx,
                             cy > canvas_h - cy ? cy : canvas_h - cy);
        double inner = 0.42, outer = 0.85;
        for (int y = 0; y < canvas_h; y++)
            for (int x = 0; x < canvas_w; x++) {
                double d = hypot(x - cx, y - cy) / r_max;
                double t = (d - inner) / (outer - inner);
                if (t < 0) t = 0; if (t > 1) t = 1;
                int m = (int)(255 * t + 0.5);
                unsigned char *b = blurred.d + ((size_t)y * canvas_w + x) * 3;
                unsigned char *g = bg.d + ((size_t)y * canvas_w + x) * 3;
                for (int ch = 0; ch < 3; ch++)
                    g[ch] = (unsigned char)((b[ch] * m + g[ch] * (255 - m) + 127) / 255);
            }
        stbi_image_free(blurred.d);
    } else { /* soft */
        img_gaussian_blur(&bg, canvas_w * BG_BLUR_FRAC);
    }
    img_brightness(&bg, BG_DARKEN);
    memcpy(canvas.d, bg.d, (size_t)canvas_w * canvas_h * 3);
    stbi_image_free(bg.d);

    /* --- middle layer: screen paste -------------------------------------- */
    {
        double minx = 1e9, miny = 1e9, maxx = -1e9, maxy = -1e9;
        for (int i = 0; i < 4; i++) {
            if (quad[i][0] < minx) minx = quad[i][0];
            if (quad[i][0] > maxx) maxx = quad[i][0];
            if (quad[i][1] < miny) miny = quad[i][1];
            if (quad[i][1] > maxy) maxy = quad[i][1];
        }
        double s = (double)bw / orig_w;   /* render scale, pre-resize width */
        double left = minx * s + bx, top = miny * s + by;
        double right = maxx * s + bx, bottom = maxy * s + by;
        int qw = (int)(right - left + 0.5), qh = (int)(bottom - top + 0.5);
        if (qw < 1) qw = 1;
        if (qh < 1) qh = 1;
        Img screen = img_cover(&photo, qw, qh);
        img_brightness(&screen, SCREEN_BRIGHTEN);
        paste_rgb_masked(&canvas, &screen, (int)(left + 0.5), (int)(top + 0.5),
                         (int)(left + 0.5), (int)(top + 0.5),
                         (int)(left + 0.5) + qw, (int)(top + 0.5) + qh);
        stbi_image_free(screen.d);
    }

    /* --- top layer: body --------------------------------------------------- */
    paste_rgba(&canvas, &body, bx, by);
    stbi_image_free(body.d);

    /* --- text lines -------------------------------------------------------- */
    double focal = ex.focal;
    if (!strcmp(focal_mode, "eq35")) focal = ex.focal * prof.crop_factor;
    {
        char fnum_s[32], focal_s[32], shutter_s[32];
        fmt_g(focal, focal_s, sizeof focal_s);
        fmt_g(ex.fnum, fnum_s, sizeof fnum_s);
        fmt_shutter(ex.expo, shutter_s, sizeof shutter_s);
        char line1[320], line2[320];
        if (ex.lens[0]) snprintf(line1, sizeof line1, "%s \xc2\xb7 %s", ex.model, ex.lens);
        else snprintf(line1, sizeof line1, "%s", ex.model);
        snprintf(line2, sizeof line2, "%s mm \xc2\xb7 %s \xc2\xb7 f/%s \xc2\xb7 ISO %ld",
                 focal_s, shutter_s, fnum_s, ex.iso);

        const unsigned char rgba[4] = TEXT_RGBA;
        int max_text_w = canvas_w - 48;
        int y = canvas_h - 24 - LINE2_SIZE - GAP_LINE;
        struct { const char *text; int size; Font *font; } lines[2] = {
            { line1, LINE1_SIZE, &font_sans },
            { line2, LINE2_SIZE, &font_mono },
        };
        for (int li = 0; li < 2; li++) {
            float size_fit = (float)lines[li].size;
            double tw = text_width(lines[li].font, lines[li].text, size_fit);
            while (tw > max_text_w && size_fit > 14) {
                size_fit -= 1;
                tw = text_width(lines[li].font, lines[li].text, size_fit);
            }
            draw_text(&canvas, lines[li].font, lines[li].text, size_fit,
                      (canvas_w - tw) / 2.0, y, rgba);
            y += GAP_LINE;
        }
        printf("%s | %s\n%s\n", ex.model, line1, line2);
    }

    /* --- write -------------------------------------------------------------- */
    if (!output) die("no output path (-o) given", NULL);
    /* encoded via write_jpeg_file: memory-encode + UTF-8 safe write, so
     * non-ASCII output paths (zh-CN usernames etc.) work everywhere */
    if (!write_jpeg_file(output, canvas_w, canvas_h, canvas.d, 93))
        die("cannot write output", output);
    printf("-> %s\n", output);
    return 0;
}
