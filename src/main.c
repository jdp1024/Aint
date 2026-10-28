/* AINT - (A)IN (I)s (N)ow (T)hreaded
 * by jdp1024
 * Distributed under GPLv3
 */

/* main.c - ain: AIN 2.32 archive tool.
 * commands: a d e l s t u v x y  switches: -f -g -m -o -r -s -u -x -y (also /x form)
 */
#define _CRT_SECURE_NO_WARNINGS 1
#include "fs.h"
#include "ain.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

static void usage(void)
{
    fprintf(stderr,
            "AINT 3.00  Pasteleft (p) 2026 jdp1024\n"
            "  (A)IN (I)s (N)ow (T)hreaded is a RE work of\n"
            "    AIN 2.32 Copyright (c) 1993-96 Transas Marine Ltd.\n"
            "\n"
            "A new archiver.  In  some cases it compresses  twice better than the well-\n"
            "known ARJ 2.41, which it is similar to by the features it provides.  It is\n"
            "also faster than any other archiver [back in 1996 - jdp1024].\n"
            "\n"
            "Revived  in  2026, it is rewritten:  64-bit,  SIMD, multithreaded,  modern\n"
            "enhancements   for   even   better   compression   ratio.   100%%  backward\n"
            "compatibility  is  carefully kept.  AIN 2.32 can open all archives created\n"
            "by AINT 3.0.\n"
            "\n"
            "usage: aint <command> {<switch>} <archive>[.AIN]\n"
            "       [<base dir>\\] [@list|wild ...]\n"
            "\n"
            "commands:\n"
            "  a  Add files to archive (creates new archive)\n"
            "  d  Delete files from archive\n"
            "  e  Extract files from archive (base names only, flat)\n"
            "  l  List contents of archive\n"
            "  s  Dump detailed stream analysis and archive header fields\n"
            "  t  Test integrity of archive\n"
            "  u  Update files to archive (replace newer, add new)\n"
            "  v  Verbosely list contents of archive\n"
            "  x  eXtract files with full pathname\n"
            "  y  copY archive, reproducing it with new options\n"
            "\n"
            "switches ( '-' prefix; '/' also accepted on Windows):\n"
            "  -f<n>[K|M]|-fa  split archive into <name>.AIN, .001..NNN volumes\n"
            "                  of n KB (plain or K) / MB (M); a = no limit\n"
            "  -m1..-m4        compression method (m1 = slow/best, m2 =\n"
            "                  normal, m3 = fast, m4 = stored; default -m2)\n"
            "  -o<dir>         output dir (a,u: where the archive is written;\n"
            "                  x: where files are extracted)\n"
            "  -r              Recurse subdirectories (a,u)\n"
            "  -s              set archive timestamp to its newest member\n"
            "  -u1..-u3        update/search speed: 1 slow, higher\n"
            "                  compression; 2 normal; 3 fast, looser.\n"
            "                  overrides the method's inherent speed\n"
            "  -j0..-j256      creation workers: 0 automatic (default),\n"
            "                  1 serial; rewrites remain serial\n"
            "  -q0..-q2        quiet level: 0 normal, 1 no per-file status/progress,\n"
            "                  2 no status/progress/summary; prompts/errors remain\n"
            "  -x<wild|@file>  eXclude files from processing (a,u)\n"
            "  -y              answer Yes to requests (archive/file overwrite)\n"
            "  -g<pass>[=<new>]  Garble with password (=new to change,\n"
            "                  bare '=' to cancel); bare -g or -g? prompts\n"
            "                  for the password on stdin\n"
            "\n"
            "notes:\n"
            "  -m1/-m2/-m3 differ in encoder search effort (see -u); all\n"
            "     write the same solid-stream format; -u<n> overrides\n"
            "  names are stored as given on the command line\n"
            "  x: on Windows drive-qualified names restore to that exact\n"
            "     path; with -o or on POSIX the drive prefix is stripped\n"
            "     and members nest under the output/current directory\n"
            "  e/x: existing targets prompt for Y/N/AY/AN/Q (yes/no/all yes/\n"
            "     all no/quit); -y overwrites without prompting\n"
            "  garble (-g): the data region is keystream-encrypted with the\n"
            "     password (index and member checksums stay in the clear);\n"
            "     prompts read stdin and mask input with '\"'; at stdin EOF\n"
            "     the tool aborts (the original re-prompts forever)\n");
}

/* ---------------- small string list ---------------- */

typedef struct { char **v; int n, cap; } Strs;

static void str_push(Strs *s, const char *str)
{
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 8;
        char **nv = (char **)realloc(s->v, (size_t)nc * sizeof(char *));
        if (!nv)
            exit(3);
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n] = (char *)malloc(strlen(str) + 1);
    if (!s->v[s->n])
        exit(3);
    strcpy(s->v[s->n], str);
    s->n++;
}

/* trim CR/LF/space in place, skip empty/comment lines; returns NULL if empty */
static char *str_trim(char *s)
{
    size_t n;
    while (*s == ' ' || *s == '\t')
        s++;
    n = strlen(s);
    while (n && (unsigned char)s[n - 1] <= ' ')
        n--;
    s[n] = 0;
    if (!n || s[0] == ';')
        return NULL;
    return s;
}

static int read_list_file(const char *path, Strs *out)
{
    FILE *f = fopen(path, "rb");
    char line[1024];
    if (!f) {
        fprintf(stderr, "error: cannot open list %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        char *t = str_trim(line);
        if (t)
            str_push(out, t);
    }
    fclose(f);
    return 0;
}

/* ---------------- paths ---------------- */

static int has_wild(const char *s)
{
    return strchr(s, '*') || strchr(s, '?');
}

static int is_dirpath(const char *s)
{
    int isdir;
    size_t n = strlen(s);
    if (n && (s[n - 1] == '\\' || s[n - 1] == '/'))
        return 1;
    return ain_fs_probe(s, &isdir) == 1 && isdir;
}

static void join_path(char *out, size_t n, const char *dir, const char *name)
{
    if (!dir || !*dir)
        snprintf(out, n, "%s", name);
    else {
        size_t nd = strlen(dir);
        snprintf(out, n, "%s%s%s", dir,
                 (nd && dir[nd - 1] != '\\' && dir[nd - 1] != '/') ? AIN_PATH_SEP : "",
                 name);
    }
}

/* ---------------- spec expansion (a, u) ---------------- */

static int excluded(const Strs *xcl, const char *path)
{
    int i;
    if (!xcl)
        return 0;
    for (i = 0; i < xcl->n; i++)
        if (ain_filter_match(xcl->v[i], NULL, path))
            return 1;
    return 0;
}

static int enum_dir(const char *dir, const char *pat, int recurse,
                     Strs *files, const Strs *xcl)
{
    AinDirEntry entry;
    AinDir *reader = ain_fs_dir_open(dir);
    char full[1024];
    int rc;
    if (!reader)
        return -1;
    while ((rc = ain_fs_dir_read(reader, &entry)) > 0) {
        if (!strcmp(entry.name, ".") || !strcmp(entry.name, ".."))
            continue;
        join_path(full, sizeof full, dir, entry.name);
        if (entry.isdir) {
            if (recurse && !entry.symlink && !has_wild(entry.name)) {
                if (enum_dir(full, pat, 1, files, xcl)) {
                    rc = -1;
                    break;
                }
            }
            continue;
        }
        if ((!strcmp(pat, "*.*") || ain_match(pat, entry.name)) && !excluded(xcl, full))
            str_push(files, full);
    }
    ain_fs_dir_close(reader);
    return rc < 0 ? -1 : 0;
}

static int expand_spec(const char *spec, const char *base, int recurse,
                        Strs *files, const Strs *xcl)
{
    char full[1024];
    if (spec[0] == '@') {
        Strs lines;
        int i, rc = 0;
        memset(&lines, 0, sizeof lines);
        if (read_list_file(spec + 1, &lines))
            return -1;
        for (i = 0; i < lines.n; i++)
            if (expand_spec(lines.v[i], base, recurse, files, xcl)) {
                rc = -1;
                break;
            }
        for (i = 0; i < lines.n; i++)
            free(lines.v[i]);
        free(lines.v);
        return rc;
    }
    join_path(full, sizeof full, base, spec);
    if (has_wild(full)) {
        char dir[1024], pat[1024];
        char *p = (char *)ain_fs_basename(full);
        if (p > full)
            --p;
        else
            p = NULL;
        if (p) {
            size_t nd = (size_t)(p - full);
            memcpy(dir, full, nd);
            dir[nd] = 0;
            if (!nd) {
                dir[0] = full[0];
                dir[1] = 0;
            }
            snprintf(pat, sizeof pat, "%s", p + 1);
        } else {
            dir[0] = 0;
            snprintf(pat, sizeof pat, "%s", full);
        }
        return enum_dir(dir, pat, recurse, files, xcl);
    } else if (is_dirpath(full)) {
        return enum_dir(full, "*.*", recurse, files, xcl);
    } else {
        if (!excluded(xcl, full))
            str_push(files, full);
    }
    return 0;
}

/* The original tool groups the create scan by extension before compressing
 * (better solid-stream ratio: similar files become adjacent). Captured from
 * the original under DOSBox:
 *   - stable grouping: scan order is preserved inside each extension group;
 *   - extension = text after the last '.' of the BASE name, compared
 *     case-insensitively;
 *   - extensionless files come first (empty key), groups sort ascending;
 *   - explicit file lists are re-grouped too; recursion groups globally
 *     across directories;
 *   - u and y do NOT re-order existing members (new files are appended).  */
static const char *ext_key(const char *name)
{
    const char *b = name + strlen(name);
    while (b > name) {
        b--;
        if (*b == '.')
            return b + 1;
        if (*b == '\\' || *b == '/' || *b == ':')
            break;                        /* base name has no dot */
    }
    return "";
}

typedef struct { char *name; const char *ext; size_t idx; } SortName;

static int cmp_ext_group(const void *pa, const void *pb)
{
    const SortName *a = (const SortName *)pa, *b = (const SortName *)pb;
    int c = ain_stricmp(a->ext, b->ext);
    if (c)
        return c;
    return a->idx < b->idx ? -1 : (a->idx > b->idx ? 1 : 0);
}

static void group_by_ext(Strs *files)
{
    SortName *it = (SortName *)malloc((size_t)(files->n ? files->n : 1) * sizeof *it);
    int i;
    if (!it)
        return;                           /* non-fatal: keep scan order */
    for (i = 0; i < files->n; i++) {
        it[i].name = files->v[i];
        it[i].ext  = ext_key(files->v[i]);
        it[i].idx  = (size_t)i;
    }
    qsort(it, (size_t)files->n, sizeof *it, cmp_ext_group);
    for (i = 0; i < files->n; i++)
        files->v[i] = it[i].name;
    free(it);
}

/* ---------------- switches ---------------- */

static long switch_number(const char *v)
{
    char *end;
    long number;

    if (v[0] < '0' || v[0] > '9')
        return -1;
    errno = 0;
    number = strtol(v, &end, 10);
    if (errno == ERANGE || *end)
        return -1;
    return number;
}

static int frag_parse(const char *v, unsigned long long *out)
{
    double num = 0.0;
    int digits = 0;
    *out = 0;
    if (!*v)
        return -1;
    if (tolower((unsigned char)v[0]) == 'a')
        return 0;                       /* fa = all free space = no limit */
    while (isdigit((unsigned char)*v)) {
        num = num * 10 + (*v - '0');
        digits++;
        v++;
    }
    if (*v == '.') {
        double f = 0.1;
        v++;
        while (isdigit((unsigned char)*v)) {
            num += (*v - '0') * f;
            f *= 0.1;
            v++;
        }
    }
    if (!digits)
        return -1;
    /* plain number = kilobytes (like the original: f360 = 360K floppy) */
    if (*v) {
        char c = (char)tolower((unsigned char)*v);
        if (c == 'k')
            ;                           /* already KB */
        else if (c == 'm')
            num *= 1024.0;
        else
            return -1;
        v++;
    }
    if (*v)
        return -1;
    *out = (unsigned long long)(num * 1024.0);
    return 0;
}

/* delete leftover <base>.001 ... .999 fragment files */
static int remove_volumes(const char *arch)
{
    char base[1024], vol[1100];
    unsigned k;
    size_t bl;
    snprintf(base, sizeof base, "%s", arch);
    bl = strlen(base);
    if (bl >= 4 && !ain_stricmp(base + bl - 4, ".AIN"))
        base[bl - 4] = 0;
    for (k = 1; k <= 999; k++) {
        snprintf(vol, sizeof vol, "%s.%03u", base, k);
        if (remove(vol) && errno != ENOENT) {
            fprintf(stderr, "error: cannot delete %s: %s\n", vol, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* append .AIN when the file part has no extension */
static void ensure_ain_ext(char *out, size_t n, const char *arch)
{
    const char *b = arch;
    const char *p;
    for (p = arch; *p; p++)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    if (strchr(b, '.'))
        snprintf(out, n, "%s", arch);
    else
        snprintf(out, n, "%s.AIN", arch);
}

/* open archive; retries with .AIN suffix, then as volume .001 of a set */
static int open_archive(AinArchive *a, const char *arch, char *used, size_t n)
{
    int rc;
    snprintf(used, n, "%s", arch);
    {
        size_t len = strlen(arch);
        if (len >= 4 && arch[len - 4] == '.' &&
            isdigit((unsigned char)arch[len - 1]) &&
            isdigit((unsigned char)arch[len - 2]) &&
            isdigit((unsigned char)arch[len - 3])) {
            char base[1024], first[1100];
            FILE *f;
            uint8_t h[AIN_HDR_SIZE];
            snprintf(base, sizeof base, "%s", arch);
            base[len - 4] = 0;
            snprintf(first, sizeof first, "%s.AIN", base);
            f = fopen(first, "rb");
            if (f) {
                int have_header = fread(h, 1, sizeof h, f) == sizeof h;
                fclose(f);
                if (have_header && h[0] == AIN_MAGIC &&
                    (h[2] == AIN_FLAG_VOLS ||
                     ((h[3] & AIN_FLAG_MORE_VOLUMES) &&
                      h[6] == 0 && h[7] == 0))) {
                    rc = ain_open(a, first);
                    if (rc)
                        return rc;
                    else {
                        snprintf(used, n, "%s", a->path);
                        a->path = used;
                        return 0;
                    }
                }
            }
        }
    }
    rc = ain_open(a, arch);
    if (!rc) {
        snprintf(used, n, "%s", a->path);
        a->path = used;
        return 0;
    }
    {
        char alt[1024];
        ensure_ain_ext(alt, sizeof alt, arch);
        if (strcmp(alt, arch)) {
            rc = ain_open(a, alt);
            if (!rc) {
                snprintf(used, n, "%s", a->path);
                a->path = used;
                return 0;
            }
        }
    }
    {
        /* retry: <base>.001 (fragmented set; base strips .NNN or .AIN) */
        char alt[1024], tmp[1024];
        size_t tl;
        snprintf(tmp, sizeof tmp, "%s", arch);
        tl = strlen(tmp);
        if (tl >= 4 && tmp[tl - 4] == '.' &&
            isdigit((unsigned char)tmp[tl - 1]) &&
            isdigit((unsigned char)tmp[tl - 2]) &&
            isdigit((unsigned char)tmp[tl - 3]))
            tmp[tl - 4] = 0;
        else if (tl >= 4 && !ain_stricmp(tmp + tl - 4, ".AIN"))
            tmp[tl - 4] = 0;
        snprintf(alt, sizeof alt, "%s.001", tmp);
        if (strcmp(alt, arch)) {
            rc = ain_open(a, alt);
            if (!rc) {
                snprintf(used, n, "%s", a->path);
                a->path = used;
            }
        }
    }
    return rc;
}

/* ---------------- command implementations ---------------- */

/* effective encoder effort: explicit -u wins, else the method's inherent
 * speed (m1 = slow/best, m2 = normal, m3 = fast) */
static int eff_speed(int speed_given, int speed, int method)
{
    if (speed_given)
        return speed;
    return method == 1 ? 1 : method == 3 ? 3 : 2;
}

/* ---------------- garble password handling ----------------
 * UX captured from the original: "Enter password: " prints to stdout and
 * reads stdin (input masked with one '"' per char); a failed candidate
 * prints "Incorrect password", after which "Continue? [Y/AY/Q]" appears
 * on the console and defaults to a retry.  At stdin EOF the original
 * re-prompts with an empty password forever; we abort instead so batch
 * automation can never hang (documented deviation). */

static char *opt_pw;             /* password from -g or entered at prompt  */
static char *opt_newpw;          /* -gOLD=NEW target; "" = cancel garble   */
static int opt_pw_ask, opt_newpw_ask, opt_newpw_given;

static int read_line_stdin(char *buf, size_t n)
{
    size_t l;
    if (!fgets(buf, (int)n, stdin))
        return -1;
    l = strlen(buf);
    while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
        buf[--l] = 0;
    return 0;
}

static void mask_echo(const char *s)
{
    for (; *s; s++)
        putchar('"');
    fflush(stdout);
}

/* returns 0 when the archive is usable (plain or unlocked), 1 to give up */
static int unlock_archive(AinArchive *a)
{
    char buf[256];
    int failed = 0;
    if (!a->garbled)
        return 0;
    if (opt_pw && !opt_pw_ask && !ain_set_password(a, opt_pw))
        return 0;
    if (opt_pw && !opt_pw_ask) {
        printf("Incorrect password\n");
        failed = 1;
    }
    for (;;) {
        if (failed)
            printf("Continue? [Y/AY/Q]");    /* console-only in the original */
        printf("Enter password: ");
        fflush(stdout);
        if (read_line_stdin(buf, sizeof buf)) {
            fprintf(stderr, "error: no password available for %s\n", a->path);
            return 1;
        }
        mask_echo(buf);
        if (!ain_set_password(a, buf)) {
            free(opt_pw);
            opt_pw = ain_strdup(buf);
            opt_pw_ask = 0;
            return 0;
        }
        printf("\nIncorrect password\n");
        failed = 1;
    }
}

/* password used when (re)writing an archive: -gOLD=NEW sets a new one,
 * -gOLD= cancels garbling; plain -gOLD (or a prompted one) retains it */
static const char *write_pw(void)
{
    char buf[256];
    if (!opt_newpw_given)
        return opt_pw;
    if (opt_newpw_ask) {
        printf("New password: ");
        fflush(stdout);
        if (read_line_stdin(buf, sizeof buf)) {
            fprintf(stderr, "error: no new password available\n");
            exit(1);
        }
        mask_echo(buf);
        opt_newpw = ain_strdup(buf);
        opt_newpw_ask = 0;
    }
    return opt_newpw;                  /* "" means write unencrypted       */
}

static int cmd_add(const char *arch, const char *base, char **pats, int np,
                   const Strs *xcl, const char *outdir, int method, int recurse,
                   int yes, unsigned long long frag, int frag_given, int setstamp,
                   int speed, const char *pw, unsigned threads)
{
    Strs files;
    char outpath[1024];
    FILE *t;
    int i, rc;
    memset(&files, 0, sizeof files);
    if (np == 0 && base && *base)
        rc = expand_spec("*.*", base, recurse, &files, xcl);
    else
        for (i = 0, rc = 0; i < np; i++)
            if ((rc = expand_spec(pats[i], base, recurse, &files, xcl)))
                break;
    if (rc) {
        for (i = 0; i < files.n; ++i)
            free(files.v[i]);
        free(files.v);
        return 1;
    }
    /* exclusion runs against the expanded list */
    if (outdir && *outdir)
        join_path(outpath, sizeof outpath, outdir,
                  ain_fs_basename(arch));
    else
        snprintf(outpath, sizeof outpath, "%s", arch);
    if (files.n == 0) {
        fprintf(stderr, "error: no files matched\n");
        return 1;
    }
    group_by_ext(&files);                 /* original groups by extension  */
    t = fopen(outpath, "rb");
    if (t) {
        fclose(t);
        if (!yes) {
            fprintf(stderr, "error: %s exists (use -y to overwrite)\n", outpath);
            return 1;
        }
    }
    rc = 0;
    if (frag_given && frag > 0)
        rc = remove_volumes(outpath);   /* stale volumes from earlier runs */
    if (!rc)
        rc = ain_create_mt(outpath, method, (const char **)files.v, files.n, frag,
                           speed, pw, threads);
    for (i = 0; i < files.n; i++)
        free(files.v[i]);
    free(files.v);
    if (rc)
        return 1;
    if (setstamp) {
        AinArchive a;
        char used[1024];
        if (open_archive(&a, outpath, used, sizeof used)) {
            fprintf(stderr, "error: cannot reopen %s to set its timestamp\n", outpath);
            return 1;
        }
        if (ain_load_index(&a)) {
            fprintf(stderr, "error: cannot decode %s to set its timestamp\n", used);
            ain_close(&a);
            return 1;
        }
        {
            uint32_t maxts = 0;
            for (i = 0; i < a.nrecs; i++)
                if (a.recs[i].ts > maxts)
                    maxts = a.recs[i].ts;
            if (a.nvols > 1) {
                /* stamp the .AIN first volume and all numeric continuations */
                char vbase[1024], vol[1100];
                size_t vl = strlen(used);
                unsigned k;
                snprintf(vbase, sizeof vbase, "%s", used);
                if (vl >= 4 && !ain_stricmp(vbase + vl - 4, ".AIN"))
                    vbase[vl - 4] = 0;
                if (ain_set_dostime(used, maxts))
                    rc = 1;
                for (k = 1; k < a.nvols; k++) {
                    snprintf(vol, sizeof vol, "%s.%03u", vbase, k);
                    if (ain_set_dostime(vol, maxts))
                        rc = 1;
                }
            } else {
                if (ain_set_dostime(used, maxts))
                    rc = 1;
            }
            ain_close(&a);
        }
    }
    return rc ? 1 : 0;
}

/* common: (kept here for clarity) rewrite is inlined into CMD_D/CMD_U below */

typedef struct {
    const char *path;
    int target;
    uint32_t size, ts;
} UpdateFile;

static int apply_updates(const Strs *files, AinEnt **entries, int *count,
                         int *changed, AinProgress *progress)
{
    UpdateFile *plan = (UpdateFile *)calloc((size_t)(files->n ? files->n : 1), sizeof *plan);
    int i, j, planned = 0, next = *count;
    uint64_t total = 0;
    if (!plan) {
        fprintf(stderr, "error: cannot allocate update plan\n");
        return -1;
    }
    for (i = 0; i < files->n; ++i) {
        int target = -1;
        uint32_t previous = 0, ts, size;
        if (ain_fs_file_size(files->v[i], &size)) {
            free(plan);
            return -1;
        }
        for (j = 0; j < *count; ++j)
            if (ain_fs_same_member(files->v[i], (*entries)[j].name)) {
                target = j;
                previous = (*entries)[j].ts;
            }
        for (j = 0; j < planned; ++j)
            if (ain_fs_same_member(files->v[i], plan[j].path)) {
                target = plan[j].target;
                previous = plan[j].ts;
            }
        ts = ain_file_dostime(files->v[i]);
        if (target >= 0 && ts <= previous)
            continue;
        plan[planned].path = files->v[i];
        plan[planned].target = target < 0 ? next++ : target;
        plan[planned].ts = ts;
        plan[planned].size = size;
        total += plan[planned].size;
        ++planned;
    }
    ain_progress_begin(progress, "Updating", total);
    for (i = 0; i < planned; ++i) {
        AinEnt entry;
        int target = plan[i].target;
        if (ain_read_file_progress(plan[i].path, &entry, plan[i].size, progress)) {
            fprintf(stderr, "error: cannot read %s or file size changed\n", plan[i].path);
            free(plan);
            return -1;
        }
        if (target == *count) {
            AinEnt *grown = (AinEnt *)realloc(*entries, (size_t)(*count + 1) * sizeof *grown);
            if (!grown) {
                fprintf(stderr, "error: cannot allocate updated entries\n");
                free(entry.name);
                free(entry.data);
                free(plan);
                return -1;
            }
            *entries = grown;
            ++*count;
            ain_status(1, "Adding %s (%u%%)\n", entry.name, progress->percent);
        } else {
            free((*entries)[target].name);
            free((*entries)[target].data);
            ain_status(1, "Updating %s (%u%%)\n", entry.name, progress->percent);
        }
        (*entries)[target] = entry;
    }
    *changed = planned != 0;
    free(plan);
    return 0;
}

int main(int argc, char **argv)
{
    enum { CMD_NONE, CMD_A, CMD_D, CMD_E, CMD_L, CMD_S, CMD_T, CMD_U, CMD_V, CMD_X, CMD_Y };
    int cmd = CMD_NONE;
    int method = 2, m_given = 0, recurse = 0, setstamp = 0, yes = 0;
    int speed = 1, speed_given = 0;
    unsigned threads = 0;
    unsigned long long frag = 0;
    int frag_given = 0;
    const char *outdir = NULL;
    int switches_on = 1;
    Strs specs, xcl;
    int i, rc = 1;
    AinArchive a;
    char archbuf[1024], archused[1024];
    const char *arch, *base = "";
    char **pats;
    int np;

    /* never pop dialogs (crash/abort): keep automation unattended */
    ain_fs_init();

    memset(&specs, 0, sizeof specs);
    memset(&xcl, 0, sizeof xcl);

    for (i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (switches_on && (s[0] == '-'
#ifdef _WIN32
                           || s[0] == '/'
#endif
                          ) && s[1]) {
            char c = (char)tolower((unsigned char)s[1]);
            const char *val = s + 2;
            switch (c) {
            case '?':
            case 'h':
                usage();
                return 0;
            case 'm':
            case 'u':
            case 'j':
            case 'q':
            {
                long requested = (c == 'u' && !val[0]) ? 1 : switch_number(val);
                long minimum = (c == 'm' || c == 'u') ? 1 : 0;
                long maximum = c == 'm' ? 4 : c == 'u' ? 3 : c == 'q' ? 2 : 256;

                if (requested < minimum ||
                    (c != 'j' && val[0] && val[1])) {
                    fprintf(stderr, "error: bad -%c switch %s\n", c, s);
                    return 2;
                }
                if (requested > maximum) {
                    fprintf(stderr, "error: bad -%c switch %s (expected %ld..%ld)\n",
                            c, s, minimum, maximum);
                    return 2;
                }
                if (c == 'm') {
                    method = (int)requested;
                    m_given = 1;
                } else if (c == 'u') {
                    speed = (int)requested;
                    speed_given = 1;
                } else if (c == 'q') {
                    ain_quiet_level = (unsigned)requested;
                } else {
                    threads = (unsigned)requested;
                }
                break;
            }
            case 'o':
                if (!*val) {
                    if (i + 1 < argc)
                        val = argv[++i];
                    else {
                        fprintf(stderr, "error: -o needs a directory\n");
                        return 2;
                    }
                }
                outdir = val;
                break;
            case 'r':
                recurse = 1;
                break;
            case 's':
                setstamp = 1;
                break;
            case 'y':
                yes = 1;                     /* yo/yf: same non-interactive */
                break;
            case 'f':
                frag_given = 1;
                if (frag_parse(val, &frag)) {
                    fprintf(stderr, "error: bad -f switch %s\n", s);
                    return 2;
                }
                break;
            case 'x':
                if (!*val) {
                    fprintf(stderr, "error: -x needs a pattern or @file\n");
                    return 2;
                }
                if (val[0] == '@') {
                    if (read_list_file(val + 1, &xcl))
                        return 2;
                } else {
                    str_push(&xcl, val);
                }
                break;
            case 'g': {
                char tmp[256];
                char *eq;
                snprintf(tmp, sizeof tmp, "%s", val);
                eq = strchr(tmp, '=');
                if (eq) {
                    *eq++ = 0;
                    opt_newpw_given = 1;
                }
                if (!tmp[0] || !strcmp(tmp, "?"))
                    opt_pw_ask = 1;              /* bare -g, -g?, -g?=       */
                else
                    opt_pw = ain_strdup(tmp);
                if (opt_newpw_given) {
                    if (eq && *eq && strcmp(eq, "?"))
                        opt_newpw = ain_strdup(eq); /* change to <new>          */
                    else if (eq && !*eq)
                        opt_newpw = ain_strdup(""); /* -gOLD= cancels garbling  */
                    else
                        opt_newpw_ask = 1;       /* <new> given as "?"       */
                }
                break;
            }
            case 'w':
            case 'p':
                break;                       /* accepted, no effect here */
            default:
                fprintf(stderr, "warning: ignoring unknown switch %s\n", s);
                break;
            }
            continue;
        }
        if (switches_on && !strcmp(s, "-")) {
            switches_on = 0;                 /* '-' alone disables switches */
            continue;
        }
        if (cmd == CMD_NONE && strlen(s) == 1) {
            switch (tolower((unsigned char)s[0])) {
            case 'a': cmd = CMD_A; continue;
            case 'd': cmd = CMD_D; continue;
            case 'e': cmd = CMD_E; continue;
            case 'l': cmd = CMD_L; continue;
            case 's': cmd = CMD_S; continue;
            case 't': cmd = CMD_T; continue;
            case 'u': cmd = CMD_U; continue;
            case 'v': cmd = CMD_V; continue;
            case 'x': cmd = CMD_X; continue;
            case 'y': cmd = CMD_Y; continue;
            default:  break;
            }
        }
        str_push(&specs, s);
    }

    if (cmd == CMD_NONE || specs.n < 1) {
        usage();
        return 2;
    }

    ensure_ain_ext(archbuf, sizeof archbuf, specs.v[0]);
    arch = archbuf;

    /* optional base directory (only when it really looks like one) */
    pats = specs.v + 1;
    np = specs.n - 1;
    if (np >= 1 && (has_wild(specs.v[1]) ? 0 : is_dirpath(specs.v[1]))) {
        base = specs.v[1];
        pats++;
        np--;
    }

    switch (cmd) {
    case CMD_A:
        return cmd_add(arch, base, pats, np, &xcl, outdir, method, recurse,
                       yes, frag, frag_given, setstamp,
                       eff_speed(speed_given, speed, method), write_pw(), threads);
    case CMD_L:
    case CMD_S:
    case CMD_V: {
        char **fp = NULL;
        int j;
        if (open_archive(&a, arch, archused, sizeof archused)) {
            fprintf(stderr, "error: cannot open %s\n", arch);
            return 1;
        }
        arch = archused;
        if (unlock_archive(&a)) {
            ain_close(&a);
            return 1;
        }
        if (ain_load_index(&a)) {
            fprintf(stderr, "error: %s: cannot decode index (corrupt?)\n", arch);
            ain_close(&a);
            return 1;
        }
        if (np && *base) {
            fp = (char **)malloc((size_t)np * sizeof(char *));
            for (j = 0; j < np; j++) {
                char buf[1024];
                join_path(buf, sizeof buf, base, pats[j]);
                fp[j] = ain_strdup(buf);
            }
            pats = fp;
        }
        if (cmd == CMD_S)
            rc = ain_analyze(&a, stdout, (const char **)pats, np);
        else
            rc = ain_list(&a, stdout, cmd == CMD_V, (const char **)pats, np);
        if (fp) {
            for (j = 0; j < np; j++)
                free(fp[j]);
            free(fp);
        }
        ain_close(&a);
        return rc ? 1 : 0;
    }
    case CMD_T:
    case CMD_E:
    case CMD_X: {
        const char *dest = cmd != CMD_T ? outdir : NULL;
        char **fp = NULL;
        int j;
        if (open_archive(&a, arch, archused, sizeof archused)) {
            fprintf(stderr, "error: cannot open %s\n", arch);
            return 1;
        }
        arch = archused;
        if (unlock_archive(&a)) {
            ain_close(&a);
            return 1;
        }
        if (ain_load_index(&a)) {
            fprintf(stderr, "error: %s: cannot decode index (corrupt?)\n", arch);
            ain_close(&a);
            return 1;
        }
        if (np && *base) {
            fp = (char **)malloc((size_t)np * sizeof(char *));
            for (j = 0; j < np; j++) {
                char buf[1024];
                join_path(buf, sizeof buf, base, pats[j]);
                fp[j] = ain_strdup(buf);
            }
            pats = fp;
        }
        if (cmd == CMD_T)
            rc = ain_test(&a, (const char **)pats, np);
        else
            rc = ain_extract(&a, dest, (const char **)pats, np,
                             cmd == CMD_E, yes);
        if (fp) {
            for (j = 0; j < np; j++)
                free(fp[j]);
            free(fp);
        }
        ain_close(&a);
        return rc ? 1 : 0;
    }
    case CMD_D:
    case CMD_U:
    case CMD_Y: {
        AinEnt *ents = NULL;
        int nents = 0, *keep = NULL;
        int changed = 0, i2, j2;
        int use_method;
        char outpath[1024];
        int opened;
        AinProgress progress;
        double started = ain_fs_time();
        if (started < 0)
            return 1;

        opened = !open_archive(&a, arch, archused, sizeof archused);
        arch = archused;

        if (cmd == CMD_D && np == 0) {
            fprintf(stderr, "error: d needs file patterns\n");
            return 1;
        }
        if ((cmd == CMD_D || cmd == CMD_Y) && !opened) {
            fprintf(stderr, "error: cannot open %s\n", arch);
            return 1;
        }
        if (cmd == CMD_U && !opened) {
            /* update onto non-existing archive == add */
            int um = m_given ? method : 2;
            return cmd_add(arch, base, pats, np, &xcl, outdir,
                           um, recurse, 1,
                           frag, frag_given, setstamp,
                           eff_speed(speed_given, speed, um), write_pw(), threads);
        }
        if (unlock_archive(&a)) {
            ain_close(&a);
            return 1;
        }
        if (ain_load_index(&a)) {
            fprintf(stderr, "error: %s: cannot decode index (corrupt?)\n", arch);
            ain_close(&a);
            return 1;
        }
        if (ain_load_entries(&a, &ents, &nents)) {
            fprintf(stderr, "error: %s: cannot decode members (corrupt?)\n", arch);
            ain_close(&a);
            return 1;
        }
        use_method = m_given ? method :
                     (a.native_volumes ? a.native_method : a.method);
        keep = (int *)calloc((size_t)nents, sizeof *keep);
        if (!keep)
            return 3;

        if (cmd == CMD_Y) {
            /* copy with new options: keep every member, always rewrite */
            for (i2 = 0; i2 < nents; i2++)
                keep[i2] = 1;
            changed = 1;
        } else if (cmd == CMD_D) {
            int ndeleted = 0;
            for (i2 = 0; i2 < nents; i2++) {
                int del = 0;
                for (j2 = 0; j2 < np; j2++)
                    if (ain_filter_match(pats[j2], base, ents[i2].name))
                        del = 1;
                keep[i2] = !del;
                if (del) {
                    ain_status(1, "Deleting %s\n", ents[i2].name);
                    ndeleted++;
                }
            }
            if (!ndeleted) {
                ain_status(2, "Nothing to delete\n");
                rc = 0;
                goto cleanup;
            }
            changed = 1;
        } else { /* CMD_U */
            Strs files;
            memset(&files, 0, sizeof files);
            rc = 0;
            for (i2 = 0; i2 < np; i2++)
                if ((rc = expand_spec(pats[i2], base, recurse, &files, &xcl)))
                    break;
            if (np == 0 && base && *base)
                rc = expand_spec("*.*", base, recurse, &files, &xcl);
            if (rc) {
                for (i2 = 0; i2 < files.n; ++i2)
                    free(files.v[i2]);
                free(files.v);
                rc = 1;
                goto cleanup;
            }
            if (np == 0 && !(base && *base)) {
                /* freshen: try every member against same-named disk file */
                for (i2 = 0; i2 < nents; i2++) {
                    uint32_t dts;
                    char diskpath[1024];
                    FILE *g;
                    snprintf(diskpath, sizeof diskpath, "%s", ents[i2].name);
                    ain_fs_native_path(diskpath);
                    g = fopen(diskpath, "rb");
                    if (!g)
                        continue;
                    fclose(g);
                    dts = ain_file_dostime(diskpath);
                    if (dts > ents[i2].ts)
                        str_push(&files, diskpath);
                }
            } else {
                group_by_ext(&files);     /* new files grouped before append */
            }
            rc = apply_updates(&files, &ents, &nents, &changed, &progress);
            for (i2 = 0; i2 < files.n; i2++)
                free(files.v[i2]);
            free(files.v);
            if (rc) {
                rc = 1;
                goto cleanup;
            }
            {
                int *grown = (int *)realloc(keep, (size_t)nents * sizeof *grown);
                if (!grown) {
                    fprintf(stderr, "error: cannot allocate update selection\n");
                    rc = 1;
                    goto cleanup;
                }
                keep = grown;
            }
            for (i2 = 0; i2 < nents; i2++)
                keep[i2] = 1;
        }

        if (changed) {
            AinEnt *sur = (AinEnt *)malloc((size_t)nents * sizeof *sur);
            int kept = 0;
            uint32_t maxts = 0;
            if (!sur)
                return 3;
            for (i2 = 0; i2 < nents; i2++)
                if (keep[i2])
                    sur[kept++] = ents[i2];
            if (kept == 0) {
                rc = 0;
                if (a.nvols > 1) {
                    /* delete the whole volume set */
                    char tmp[1024], vol[1100];
                    size_t tl;
                    unsigned k;
                    snprintf(tmp, sizeof tmp, "%s", arch);
                    tl = strlen(tmp);
                    if (tl >= 4)
                        tmp[tl - 4] = 0;      /* drop ".001"/".AIN" */
                    if (remove(arch) && errno != ENOENT) {
                        fprintf(stderr, "error: cannot delete %s: %s\n",
                                arch, strerror(errno));
                        rc = 1;
                    }
                    for (k = 1; k <= 999; k++) {
                        snprintf(vol, sizeof vol, "%s.%03u", tmp, k);
                        if (remove(vol) && errno != ENOENT) {
                            fprintf(stderr, "error: cannot delete %s: %s\n", vol, strerror(errno));
                            rc = 1;
                        }
                    }
                    if (!rc)
                        ain_status(2, "Archive %s deleted (no files left)\n", tmp);
                } else if (!remove(arch)) {
                    ain_status(2, "Archive %s deleted (no files left)\n", arch);
                } else {
                    fprintf(stderr, "error: cannot delete %s: %s\n", arch, strerror(errno));
                    rc = 1;
                }
            } else {
                unsigned long long wfrag =
                    frag_given ? frag : (a.nvols > 1 ? a.frag : 0ULL);
                if (wfrag) {
                    /* rewrite keeps fragmentation: work under the base name */
                    char tmp[1024];
                    size_t tl;
                    snprintf(tmp, sizeof tmp, "%s", arch);
                    tl = strlen(tmp);
                    if (tl >= 4 && tmp[tl - 4] == '.' &&
                        isdigit((unsigned char)tmp[tl - 1]) &&
                        isdigit((unsigned char)tmp[tl - 2]) &&
                        isdigit((unsigned char)tmp[tl - 3]))
                        tmp[tl - 4] = 0;
                    else if (tl >= 4 && !ain_stricmp(tmp + tl - 4, ".AIN"))
                        tmp[tl - 4] = 0;
                    snprintf(outpath, sizeof outpath, "%s.AIN", tmp);
                    if (remove_volumes(outpath)) {
                        free(sur);
                        rc = 1;
                        goto cleanup;
                    }
                } else if (outdir && *outdir && (cmd == CMD_U || cmd == CMD_Y))
                    join_path(outpath, sizeof outpath, outdir,
                              ain_fs_basename(arch));
                else
                    snprintf(outpath, sizeof outpath, "%s", arch);
                if (cmd == CMD_Y)
                    ain_status(2, "Updating archive\n");
                else
                    ain_status(2, "Rewriting archive %s\n", outpath);
                rc = ain_write(outpath, use_method, sur, kept, wfrag,
                               eff_speed(speed_given, speed, use_method),
                               write_pw()) ? 1 : 0;
                if (!rc && a.nvols > 1 && !wfrag &&
                    !ain_stricmp(outpath, arch) && remove_volumes(outpath))
                    rc = 1;
                if (!rc && setstamp) {
                    for (i2 = 0; i2 < kept; i2++)
                        if (sur[i2].ts > maxts)
                            maxts = sur[i2].ts;
                    if (wfrag) {
                        char vbase[1024], vol[1100];
                        size_t bl = strlen(outpath);
                        unsigned k;
                        snprintf(vbase, sizeof vbase, "%s", outpath);
                        if (bl >= 4)
                            vbase[bl - 4] = 0;      /* drop the ".AIN" */
                        if (ain_set_dostime(outpath, maxts))
                            rc = 1;
                        for (k = 1; k <= 999; k++) {
                            int isdir, exists;
                            snprintf(vol, sizeof vol, "%s.%03u", vbase, k);
                            exists = ain_fs_probe(vol, &isdir);
                            if (exists < 0) {
                                rc = 1;
                                break;
                            }
                            if (!exists)
                                break;
                            if (ain_set_dostime(vol, maxts))
                                rc = 1;
                        }
                    } else {
                        if (ain_set_dostime(outpath, maxts))
                            rc = 1;
                    }
                }
            }
            free(sur);
        } else {
            ain_status(2, "Nothing to update\n");
            rc = 0;
        }

    cleanup:
        if (cmd == CMD_U && !rc) {
            uint64_t total = 0;
            for (i2 = 0; i2 < nents; ++i2)
                total += ents[i2].n;
            if (ain_size_summary(changed ? outpath : arch, total, started, &progress))
                rc = 1;
        }
        /* names/data of replaced entries were freed in place; free the rest */
        for (i2 = 0; i2 < nents; i2++) {
            free(ents[i2].name);
            free(ents[i2].data);
        }
        free(ents);
        free(keep);
        ain_close(&a);
        return rc;
    }
    default:
        usage();
        return 2;
    }
}
