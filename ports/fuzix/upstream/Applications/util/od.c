/* od - octal dump		   Author: Andy Tanenbaum             */
/* Adapted to UZI180 by H. Peraza                             */
/* A subset of modern flags/options added by Henrik Löfgren   */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#define RADIX_HEX   16
#define RADIX_DEC   10
#define RADIX_OCT   8
#define RADIX_ASC   1
#define RADIX_NONE  0

int  vflag;
int  addr_radix;
int  data_radix;
int  word_length;
int  print_ascii;
int  linenr, width, state, ever;
/* Ripple C characters occupy whole words; retain all 16 input cells. */
int  prevwds[16];
long off;
long total_bytes=0;
char buf[512], buffer[BUFSIZ];
int  next;
int  bytespresent;


int  main(int argc, char *argv[]);
long offset(int argc, char *argv[], int k);
void dumpfile(void);
void wdump(short *words, int k, int radix);
void bdump(char bytes[16], int k, int c);
void adump(unsigned char bytes[16], int k);
void pad(int k);
void byte(int val, int c);
int  getwords(short **words);
int  same(char *w1, char *w2);
void outword(int val, int radix);
void outnum(int num, int radix);
void addrout(long l);
char hexit(int k);
void usage(void);



long offset(int argc, char *argv[], int k)
{
    int dot = 0, bad = 0;
    char *p, *endp;
    long val;
    long mult;

    /* See if the offset is octal with a dot. */
    p = argv[k];
    while (*p)
        if (*p++ == '.') dot = 1;

    /* Convert offset to binary. */
    p = argv[k];
    if (dot) {
        val = strtol(p, &endp, 8);
        if (val < 0 || val == LONG_MAX || endp == p || *endp++ != '.')
            bad = 1;
    } else {
        val = strtol(p, &endp, 0);
        if (val < 0 || val == LONG_MAX || endp == p)
            bad = 1;
    }

    if (bad) {
        printf("Bad offset: %s\n", p);
        exit(1);
    }

    /* Look for multiplier */
    p = endp;
    if (!*p && k + 1 == argc - 1)
            p = argv[k + 1];

    if (!*p)
        return val;

    if (*p == 'b')
        mult = 512L;
    else if (*p == 'K') {
        if (p[1] == 'B') {
            mult = 1000L;
            p++;
        } else
            mult = 1024L;
    } else if (*p == 'M') {
        if (p[1] == 'B') {
            mult = 1000L * 1000L;
            p++;
        } else
            mult = 1024L * 1024L;
    } else
        mult = 0;

    if (mult == 0 || p[1] != '\0') {
        printf("Bad offset multiplier: %s\n", p);
        exit(1);
    }

    return mult * val;
}


void dumpfile(void)
{
    int k;
    short *words;
    long bytes_read = 0;
    int run;
    char *src;
    char *dst;
    run = 1;
    while ((k = getwords(&words)) && run) {	/* 'k' is # bytes read */
	    bytes_read += k;
        if (!vflag) {		/* ensure 'lazy' evaluation */
	        if (k == width && ever == 1 &&
                same((char *)words, (char *)prevwds)) {
		        if (state == 0) {
		            printf("*\n");
		            state = 1;
		            off += width;
		            continue;
		        } else if (state == 1) {
		            off += width;
		            continue;
		        }
	        }
	    }
	    addrout(off);
	    off += k;
	    state = 0;
	    ever = 1;
	    linenr = 1;

        /* Finish up if specified number of bytes have been read */
        if((total_bytes) && (bytes_read >= total_bytes)) {
            run = 0;
            k -= (bytes_read - total_bytes);
            off -= (bytes_read - total_bytes);
        }

	    if(word_length == 2) wdump(words, k, data_radix);
        else if(word_length == 1) bdump((char *)words, k, data_radix);

        if(print_ascii) {
            if(!run || k < width) pad(width - k);
            adump((unsigned char *)words, k);
        }
        printf("\n");

        /* Cast to char to handle different widths */
        dst = (char *)prevwds;
        src = (char *)words;
	    for (k = 0; k < width; k++) dst[k] = src[k];
	    for (k = 0; k < width; k++) src[k] = 0;
    }
}

/* Pad with spaces to ensure that the ascii output lines up on the last
 * line
 */
void pad(int k) {
    int i,j;
    int pad_len;

    if(data_radix == RADIX_HEX) pad_len = 2*word_length + 1;
    else if(data_radix == RADIX_OCT) pad_len = 3*word_length + 1;
    else if(data_radix == RADIX_DEC) pad_len = 3*word_length +
                                               (2 - word_length);

    for(i=0; i<k; i++) {
        for(j=0; j<pad_len; j++) printf(" ");
    }
}

void adump(unsigned char bytes[16], int k) {
    int i;
    printf(" >");
    for(i=0; i<k; i++) {
        if(bytes[i] > 0x1f && bytes[i] < 0x7F) printf("%c", bytes[i]);
        else printf(".");
    }
    printf("<");

}


void wdump(short *words, int k, int radix)
{
    int i;

    if (linenr++ != 1) printf("       ");
    for (i = 0; i < (k + 1) / 2; i++) {
        const unsigned char *cells = (const unsigned char *)words;
        unsigned long value = cells[2*i];
        if (2*i+1 < k) value |= (unsigned long)cells[2*i+1] << 16;
        if (radix == RADIX_OCT) printf(" %011lo", value);
        else if (radix == RADIX_DEC) printf(" %010lu", value);
        else printf(" %08lx", value);
    }
}


void bdump(char bytes[16], int k, int c)
{
    int i;

    if (linenr++ != 1) printf("       ");
    for (i = 0; i < k; i++)
	byte(bytes[i] & 0377, c);
}

void byte(int val, int c)
{
    if (c == RADIX_OCT) {
	printf(" ");
	outnum(val, 7);
	return;
    } else if (c == RADIX_HEX) {
	printf(" %02x", val);
	return;
    } else if (c == RADIX_DEC) {
    printf(" %03d", val);
    return;
    }
    if (val == 0)
	printf("  \\0");
    else if (val == '\b')
	printf("  \\b");
    else if (val == '\f')
	printf("  \\f");
    else if (val == '\n')
	printf("  \\n");
    else if (val == '\r')
	printf("  \\r");
    else if (val == '\t')
	printf("  \\t");
    else if (val >= ' ' && val < 0177)
	printf("   %c", val);
    else {
	printf(" ");
	outnum(val, 7);
    }
}


int getwords(short **words)
{
    int count;

    if (next >= bytespresent) {
	bytespresent = read(0, buf, 512);
        if (bytespresent < 0) { perror("od"); exit(1); }
	next = 0;
    }
    if (next >= bytespresent) return(0);
    *words = (short *) &buf[next];
    if (next + width <= bytespresent)
	count = width;
    else
	count = bytespresent - next;

    next += count;

    return(count);
}

int same(char *w1, char *w2)
{
    int i;

    i = width;
    while (i--)
	if (*w1++ != *w2++) return(0);

    return(1);
}

void outword(int val, int radix)
{
    printf(" ");
    outnum(val, radix);
}


void outnum(int num, int radix)
{
    /*  Output a number with all leading 0s present.  Octal is 6 places,
     *  decimal is 5 places, hex is 4 places.
     */
    unsigned val;

    val = (unsigned) num;
    if (radix == RADIX_OCT)
	printf ("%06o", val);
    else if (radix == RADIX_DEC)
	printf ("%05u", val);
    else if (radix == RADIX_HEX)
	printf ("%04x", val);
    else if (radix == 7) {
  	/* special case */
	printf ("%03o", val);
    }
}


void addrout(long l)
{
    switch(addr_radix) {
        case RADIX_OCT:
            printf("%07lo", l);
            break;
        case RADIX_DEC:
            printf("%07ld", l);
            break;
        case RADIX_HEX:
            printf("%07lx", l);
            break;
        default:
            break;
    }
}


void usage(void)
{
    fprintf(stderr, "Usage: od [OPTION]... [FILE]...\n");
    fprintf(stderr, "  or: od [-bcdhovx] [file] [ [+] offset [.] [b] ]\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr,
            "-A RADIX  Output format for file offset. RADIX is one of\n");
    fprintf(stderr, "          [doxn] for Decimal, Octal, Hex or None\n");
    fprintf(stderr, "-j BYTES  Skip BYTES input bytes first\n");
    fprintf(stderr, "-N BYTES  Limit dump to BYTES input bytes\n");
    fprintf(stderr, "-t TYPE   Select output format\n");
    fprintf(stderr, "-w BYTES  output BYTES bytes per output line, can be 1,2,4,8 or 16.\n");
    fprintf(stderr, "-v        do not use * to mark line supression\n\n");

    fprintf(stderr, "TYPE is made up of one of these specifications:\n");
    fprintf(stderr, "c         Printable character or backslash escape\n");
    fprintf(stderr, "o[SIZE]   Octal, SIZE bytes per integer\n");
    fprintf(stderr, "u[SIZE]   Unsigned decimal, SIZE bytes per integer\n");
    fprintf(stderr, "x[SIZE]   Hexadecimal, SIZE bytes per integer\n");
    fprintf(stderr, "SIZE can be 1 or 2\n");
    fprintf(stderr, "Add a z suffix to any type displays printable ");
    fprintf(stderr, "characters at the end of each output line\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    int k;
    int opt;
    char *p;

    /* Default values */
    data_radix = RADIX_OCT;
    addr_radix = RADIX_OCT;
    word_length = 2;
    print_ascii = 0;
    width = 16;

    /* single-byte hex dump */
    if (!strcmp(argv[0], "hd")) {
        data_radix = RADIX_HEX;
        addr_radix = RADIX_HEX;
        word_length = 1;
        print_ascii = 1;
    }

    /* Process flags */
    setbuf(stdout, buffer);

    while((opt = getopt(argc, argv, "A:t:j:N:w:bcdhovxq")) != -1) {
        switch(opt) {
            case 'A':
                switch(optarg[0]) {
                    case 'o':
                        addr_radix = RADIX_OCT;
                        break;
                    case 'd':
                        addr_radix = RADIX_DEC;
                        break;
                    case 'x':
                        addr_radix = RADIX_HEX;
                        break;
                    case 'n':
                        addr_radix = RADIX_NONE;
                        break;
                    default:
                        usage();
                        break;
                }
                break;
            case 't':
                switch(optarg[0]) {
                    case 'c':
                        data_radix = RADIX_ASC;
                        word_length = 1;
                        break;
                    case 'o':
                        data_radix = RADIX_OCT;
                        break;
                    case 'u':
                        data_radix = RADIX_DEC;
                        break;
                    case 'x':
                        data_radix = RADIX_HEX;
                        break;
                    default:
                        usage();
                        break;
                }
                k=1;
                if(optarg[k]) {
                    if(optarg[k] == '1' || optarg[k] == '2') {
                        word_length = optarg[1]-0x30;
                        k++;
                    } else {
                        fprintf(stderr,
                            "Error - only 1 or 2 byte words supported\n");
                    }
                }

                if(optarg[k]) {
                    if(optarg[k] == 'z') print_ascii = 1;
                    else usage();
                }

                break;
            case 'j':
    	        off = offset(1, &optarg, 0);
                break;
            case 'N':
                total_bytes = offset(1, &optarg, 0);
                break;
            case 'w':
                width = atoi(optarg);
                /* Only support powers of two to avoid issues
                 * with always reading 512 bytes from disk
                 */
                if(width != 1 && width !=2 && width !=4 &&
                   width != 8 && width !=16) {
                    fprintf(stderr,
                       "Error - only width 1,2,4,8 and 16 are supported\n");
                    exit(1);
                }
                break;
            case 'b':
                word_length = 1;
                break;
            case 'c':
                data_radix = RADIX_ASC;
                word_length = 1;
                break;
            case 'd':
                data_radix = RADIX_DEC;
                break;
            case 'h':
                data_radix = RADIX_HEX;
                word_length = 1;
                break;
            case 'o':
                /* Default values */
                break;
            case 'v':
                vflag++;
                break;
            case 'x':
                data_radix = RADIX_HEX;
                break;
            default:
                usage();
                break;
        }
    }

    /* Process file name, if any. */
    if(optind < argc)
        p = argv[optind];

    if (optind < argc && *p != '+') {
	/* Explicit file name given. */
    close(0);
	if (open(argv[optind], O_RDONLY) != 0) {
	    fprintf(stderr, "od: cannot open %s\n", argv[optind]);
	    exit(1);
	}
	optind++;
    }

    /* Process offset, if any. */
    if (optind < argc) {
	/* Offset present. */
	off = offset(argc, argv, optind);
    }
    lseek(0, off, SEEK_SET);

    dumpfile();
    addrout(off);
    printf("\n");

    return 0;
}
