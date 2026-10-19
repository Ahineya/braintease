/* UNIX V7 source code: see Applications/V7/COPYRIGHT or www.tuhs.org. */
/* ANSIfied for FUZIX. Ripple/Fuse-X adaptation: checked three-file front end,
 * dynamic change tables, wide offsets, streaming lines and safe ed scripts.
 * The range-coalescing algorithm below is retained from the V7 implementation.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAX_LINES 30000
struct range { long from, to; };
struct diff { struct range old, new; };
static struct diff *d13, *d23, *de;
static FILE *fp[3];
static long cline[3], last[4];
static int eflag;
static char temporary[3][32];
static int temporary_count;

static void cleanup(void)
{
    int i;
    for (i = 0; i < temporary_count; i++)
        unlink(temporary[i]);
}

static void fail(const char *message)
{
    fprintf(stderr, "diff3: %s\n", message);
    exit(2);
}

static void interrupted(int sig)
{
    cleanup();
    _exit(2);
}

static int make_temp(char **path)
{
    int fd;
    if (temporary_count == 3) fail("too many temporary files");
    *path = temporary[temporary_count];
    strcpy(*path, "/tmp/d3XXXXXX");
    fd = mkstemp(*path);
    if (fd < 0) fail("cannot create temporary file");
    temporary_count++;
    return fd;
}

static void seek_file(FILE *file, long offset)
{
    if (offset < 0 || fseek(file, offset, SEEK_SET))
        fail("cannot seek input");
}

/* Only diff's command lines are parsed; consume whole physical lines so a
 * long payload cannot be mistaken for another command at a buffer boundary. */
static int report_line(FILE *file, char *line)
{
    unsigned n = 0;
    int c, have = 0;
    while ((c = getc(file)) != EOF) {
        have = 1;
        if (c == '\n') break;
        if (n < 95) line[n++] = c;
    }
    if (ferror(file)) fail("cannot read diff report");
    line[n] = 0;
    return have;
}

static long number(char **text)
{
    char *end;
    long n;
    errno = 0;
    n = strtol(*text, &end, 10);
    if (end == *text || errno || n < 0 || n > MAX_LINES)
        fail("invalid diff range");
    *text = end;
    return n;
}

static struct diff *readin(const char *path, int *count)
{
    FILE *file = fopen(path, "r");
    struct diff *table = NULL, *grown;
    int capacity = 0, used = 0;
    char line[96], *p, kind;
    long a, b, c, d;
    if (!file) fail("cannot open diff report");
    while (report_line(file, line)) {
        if (line[0] < '0' || line[0] > '9') continue;
        p = line;
        a = b = number(&p);
        if (*p == ',') { p++; b = number(&p); }
        kind = *p++;
        if (kind != 'a' && kind != 'c' && kind != 'd')
            fail("invalid diff command");
        c = d = number(&p);
        if (*p == ',') { p++; d = number(&p); }
        if (*p || a > b || c > d) fail("invalid diff command");
        if (kind == 'a') a++;
        if (kind == 'd') c++;
        b++; d++;
        if (used >= MAX_LINES) fail("too many changes");
        if (used + 1 >= capacity) {
            capacity = capacity > 15000 ? MAX_LINES + 1 : capacity ? capacity * 2 : 32;
            grown = realloc(table, (size_t)capacity * sizeof(*table));
            if (!grown) fail("out of memory");
            table = grown;
        }
        if (used && (a < table[used - 1].old.to || c < table[used - 1].new.to))
            fail("unordered diff ranges");
        table[used].old.from = a; table[used].old.to = b;
        table[used].new.from = c; table[used].new.to = d;
        used++;
    }
    if (fclose(file)) fail("cannot close diff report");
    if (!table) {
        table = calloc(1, sizeof(*table));
        if (!table) fail("out of memory");
    } else memset(table + used, 0, sizeof(*table));
    *count = used;
    return table;
}

static void separate(const char *tag) { printf("====%s\n", tag); }

static void prange(struct range *range)
{
    if (range->to <= range->from) printf("%lda\n", range->from - 1);
    else {
        printf("%ld", range->from);
        if (range->to > range->from + 1) printf(",%ld", range->to - 1);
        printf("c\n");
    }
}

/* Count native characters rather than host octets. No line buffer or 16-bit
 * byte offset: long lines and files larger than 64K remain streamable. */
static long skip(int i, long from, const char *prefix)
{
    long total = 0, length;
    int c = EOF;
    while (cline[i] < from - 1) {
        length = 0;
        if (prefix) fputs(prefix, stdout);
        while ((c = getc(fp[i])) != EOF) {
            length++; total++;
            if (prefix) putchar(c);
            if (c == '\n') break;
        }
        if (ferror(fp[i])) fail("cannot read input");
        if (!length) fail("input changed or inconsistent diff report");
        if (prefix && c != '\n') fputs("\n\\ No newline at end of file\n", stdout);
        cline[i]++;
    }
    return total;
}

static void change(int i, struct range *range, int duplicate)
{
    printf("%d:", i);
    last[i] = range->to;
    prange(range);
    if (!duplicate) {
        skip(i - 1, range->from, NULL);
        skip(i - 1, range->to, "  ");
    }
}

static void keep(int i, struct range *old, struct range *new)
{
    long delta = last[3] - last[i];
    struct range range;
    range.from = new->from - delta;
    range.to = new->to - delta;
    change(i, &range, 1);
}

static int duplicate(struct range *left, struct range *right)
{
    long position[2], n;
    int a, b, equal = 1;
    if (left->to - left->from != right->to - right->from) return 0;
    skip(0, left->from, NULL); skip(1, right->from, NULL);
    position[0] = ftell(fp[0]); position[1] = ftell(fp[1]);
    if (position[0] < 0 || position[1] < 0) fail("cannot tell input position");
    for (n = left->from; n < left->to && equal; n++) {
        do {
            a = getc(fp[0]); b = getc(fp[1]);
            if (ferror(fp[0]) || ferror(fp[1])) fail("cannot read input");
            if (a != b) { equal = 0; break; }
        } while (a != '\n' && a != EOF);
    }
    seek_file(fp[0], position[0]); seek_file(fp[1], position[1]);
    return equal;
}

static int edit(struct diff *diff, int same, int j)
{
    if (((same + 1) & eflag) == 0) return j;
    j++;
    de[j].old = diff->old;
    de[j].new.from = de[j - 1].new.to + skip(2, diff->new.from, NULL);
    de[j].new.to = de[j].new.from + skip(2, diff->new.to, NULL);
    return j;
}

/* Escape a literal single-dot line, then emit address-specific substitutions
 * after the input block. Applying the script must preserve the actual text. */
static void script_text(struct diff *edit, int substitutions)
{
    long pos = edit->new.from, row = edit->old.from, length;
    int c, first;
    seek_file(fp[2], pos);
    while (pos < edit->new.to) {
        length = 0; first = 0;
        do {
            c = getc(fp[2]);
            if (c == EOF) fail("cannot read script text");
            if (!length) first = c;
            if (!substitutions) {
                if (c == '\n' && length == 1 && first == '.') putchar('.');
                putchar(c);
            }
            length++; pos++;
        } while (c != '\n' && pos < edit->new.to);
        if (c != '\n') fail("ed scripts require newline-terminated text");
        if (substitutions && first == '.' && length == 2)
            printf("%lds/.././\n", row);
        row++;
    }
}

static void edscript(int n)
{
    int i;
    /* Check representability before emitting any commands. */
    for (i = 1; i <= n; i++) {
        if (de[i].new.to > de[i].new.from) {
            seek_file(fp[2], de[i].new.to - 1);
            if (getc(fp[2]) != '\n') fail("ed scripts require newline-terminated text");
        }
    }
    for (; n > 0; n--) {
        if (de[n].new.from == de[n].new.to && de[n].old.to > de[n].old.from) {
            printf("%ld", de[n].old.from);
            if (de[n].old.to > de[n].old.from + 1) printf(",%ld", de[n].old.to - 1);
            puts("d");
            continue;
        }
        prange(&de[n].old);
        script_text(de + n, 0);
        puts(".");
        script_text(de + n, 1);
    }
}

void merge(int m1, int m2)
{
	register struct diff *d1, *d2, *d3;
	int dup;
	int j;
	int t1, t2;
	d1 = d13;
	d2 = d23;
	j = 0;
	for (; (t1 = d1 < d13 + m1) | (t2 = d2 < d23 + m2);) {
/*			first file is different from others*/
		if (!t2 || (t1 && d1->new.to < d2->new.from)) {
/*			stuff peculiar to 1st file */
			if (eflag == 0) {
				separate("1");
				change(1, &d1->old, 0);
				keep(2, &d1->old, &d1->new);
				change(3, &d1->new, 0);
			}
			d1++;
			continue;
		}
/*			second file is different from others*/
		if (!t1 || (t2 && d2->new.to < d1->new.from)) {
			if (eflag == 0) {
				separate("2");
				keep(1, &d2->old, &d2->new);
				change(2, &d2->old, 0);
				change(3, &d2->new, 0);
			}
			d2++;
			continue;
		}
/*			merge overlapping changes in first file
 *			this happens after extension see below*/
		if (d1 + 1 < d13 + m1 && d1->new.to >= d1[1].new.from) {
			d1[1].old.from = d1->old.from;
			d1[1].new.from = d1->new.from;
			d1++;
			continue;
		}
/*			merge overlapping changes in second*/
		if (d2 + 1 < d23 + m2 && d2->new.to >= d2[1].new.from) {
			d2[1].old.from = d2->old.from;
			d2[1].new.from = d2->new.from;
			d2++;
			continue;
		}
/*			stuff peculiar to third file or different in all*/
		if (d1->new.from == d2->new.from &&
		    d1->new.to == d2->new.to) {
			dup = duplicate(&d1->old, &d2->old);
/*				dup=0 means all files differ
 *				dup =1 meands files 1&2 identical*/
			if (eflag == 0) {
				separate(dup ? "3" : "");
				change(1, &d1->old, dup);
				change(2, &d2->old, 0);
				d3 = d1->old.to > d1->old.from ? d1 : d2;
				change(3, &d3->new, 0);
			} else
				j = edit(d1, dup, j);
			d1++;
			d2++;
			continue;
		}
/*			overlapping changes from file1 & 2
 *			extend changes appropriately to
 *			make them coincide*/
		if (d1->new.from < d2->new.from) {
			d2->old.from -= d2->new.from - d1->new.from;
			d2->new.from = d1->new.from;
		} else if (d2->new.from < d1->new.from) {
			d1->old.from -= d1->new.from - d2->new.from;
			d1->new.from = d2->new.from;
		}
		if (d1->new.to > d2->new.to) {
			d2->old.to += d1->new.to - d2->new.to;
			d2->new.to = d1->new.to;
		} else if (d2->new.to > d1->new.to) {
			d1->old.to += d2->new.to - d1->new.to;
			d1->new.to = d2->new.to;
		}
	}
	if (eflag)
		edscript(j);
}


static void compare(const char *left, const char *right, int output)
{
    pid_t pid, waited;
    int status;
    char *args[5];
    args[0] = "diff"; args[1] = "--";
    args[2] = (char *)left; args[3] = (char *)right; args[4] = NULL;
    pid = fork();
    if (pid < 0) fail("cannot fork diff");
    if (pid == 0) {
        if (dup2(output, 1) < 0) _exit(2);
        if (output != 1) close(output);
        execv("/bin/diff", args);
        perror("diff3: /bin/diff");
        _exit(2);
    }
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) > 1)
        fail("diff failed");
    if (close(output)) fail("cannot close diff report");
}

static void usage(void)
{
    fputs("usage: diff3 [-e|-x|-3] [--] file1 file2 file3\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    int i = 1, j, mode, stdin_used = 0, fd, n, off, m1, m2;
    char *names[3], *reports[2], buffer[512];
    struct stat st;
    atexit(cleanup);
    signal(SIGHUP, interrupted); signal(SIGINT, interrupted);
    signal(SIGTERM, interrupted); signal(SIGPIPE, interrupted);
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (j = 1; argv[i][j]; j++) {
            switch (argv[i][j]) {
            case 'e': mode = 3; break;
            case 'x': mode = 1; break;
            case '3': mode = 2; break;
            default: usage(); return 2;
            }
            if (eflag && eflag != mode) usage();
            eflag = mode;
        }
        i++;
    }
    if (argc - i != 3) usage();
    /* Reject repeated stdin before reading it: a terminal would otherwise
     * block forever before we even discover the second '-' operand. */
    for (j = 0; j < 3; j++)
        if (!strcmp(argv[i + j], "-") && stdin_used++)
            fail("stdin may appear only once");
    for (j = 0; j < 3; j++) {
        names[j] = argv[i + j];
        if (!strcmp(names[j], "-")) {
            fd = make_temp(&names[j]);
            while ((n = read(0, buffer, sizeof(buffer))) > 0) {
                off = 0;
                while (off < n) {
                    int written = write(fd, buffer + off, n - off);
                    if (written <= 0) fail("cannot spool stdin");
                    off += written;
                }
            }
            if (n < 0 || close(fd)) fail("cannot spool stdin");
        }
        if (stat(names[j], &st) || !S_ISREG(st.st_mode))
            fail("inputs must be readable regular files (or one stdin)");
        fp[j] = fopen(names[j], "r");
        if (!fp[j]) { perror(names[j]); return 2; }
    }
    /* The helper inherits no buffered input streams. Reopen for the merge. */
    for (j = 0; j < 3; j++) {
        if (fclose(fp[j])) fail("cannot close input");
    }
    fd = make_temp(&reports[0]); compare(names[0], names[2], fd);
    fd = make_temp(&reports[1]); compare(names[1], names[2], fd);
    d13 = readin(reports[0], &m1); d23 = readin(reports[1], &m2);
    de = calloc((size_t)(m1 < m2 ? m1 : m2) + 1, sizeof(*de));
    if (!de) fail("out of memory");
    for (j = 0; j < 3; j++) {
        fp[j] = fopen(names[j], "r");
        if (!fp[j]) fail("cannot reopen input");
    }
    merge(m1, m2);
    for (j = 0; j < 3; j++) if (fclose(fp[j])) fail("cannot close input");
    if (fflush(stdout) || ferror(stdout)) fail("cannot write output");
    return 0;
}
