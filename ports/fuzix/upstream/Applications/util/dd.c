/*
 * Copyright (c) 1993 by David I. Bell
 * Permission is granted to use, distribute, or modify this source,
 * provided that this copyright notice remains intact.
 *
 * The "dd" built-in command.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>


typedef unsigned char BOOL;

#ifndef FALSE
#define FALSE  0
#define TRUE   1
#endif

#define	PAR_NONE	0
#define	PAR_IF		1
#define	PAR_OF		2
#define	PAR_BS		3
#define	PAR_COUNT	4
#define	PAR_SEEK	5
#define	PAR_SKIP	6


typedef struct {
    char *name;
    int value;
} PARAM;


static PARAM params[] =
{
    {"if",    PAR_IF},
    {"of",    PAR_OF},
    {"bs",    PAR_BS},
    {"count", PAR_COUNT},
    {"seek",  PAR_SEEK},
    {"skip",  PAR_SKIP},
    {NULL,    PAR_NONE}
};


static long getnum(char *);

BOOL intflag;

static char localbuf[8192];

int main(int argc, char *argv[])
{
    char *str;
    char *cp;
    PARAM *par;
    char *infile;
    char *outfile;
    int infd;
    int outfd;
    int incc;
    int outcc;
    int blocksize;
    long count;
    long seekval;
    long skipval;
    long intotal;
    long outtotal;
    long records;
    struct stat inputstat, outputstat;
    unsigned long infull=0, inpart=0, outfull=0, outpart=0;
    char *buf;
    int ret = 1;

    infile = NULL;
    outfile = NULL;
    seekval = 0;
    skipval = 0;
    blocksize = 512;
    count = 0x7fffffff;
    records = 0;

    while (--argc > 0) {
	str = *++argv;
	cp = strchr(str, '=');
	if (cp == NULL) {
	    fprintf(stderr, "Bad dd argument\n");
	    return 1;
	}
	*cp++ = '\0';

	for (par = params; par->name; par++) {
	    if (strcmp(str, par->name) == 0)
		break;
	}

	switch (par->value) {
	case PAR_IF:
	    if (infile) {
		fprintf(stderr, "Multiple input files illegal\n");
		return 1;
	    }
	    infile = cp;
	    break;

	case PAR_OF:
	    if (outfile) {
		fprintf(stderr, "Multiple output files illegal\n");
		return 1;
	    }
	    outfile = cp;
	    break;

	case PAR_BS:
	    { long size = getnum(cp);
              blocksize = size > 0 && size <= 32767 ? (int)size : 0; }
	    if (blocksize <= 0) {
		fprintf(stderr, "Bad block size value\n");
		return 1;
	    }
	    break;

	case PAR_COUNT:
	    count = getnum(cp);
	    if (count < 0) {
		fprintf(stderr, "Bad count value\n");
		return 1;
	    }
	    break;

	case PAR_SEEK:
	    seekval = getnum(cp);
	    if (seekval < 0) {
		fprintf(stderr, "Bad seek value\n");
		return 1;
	    }
	    break;

	case PAR_SKIP:
	    skipval = getnum(cp);
	    if (skipval < 0) {
		fprintf(stderr, "Bad skip value\n");
		return 1;
	    }
	    break;

	default:
	    fprintf(stderr, "Unknown dd parameter\n");
	    return 1;
	}
    }

    if (skipval > 2147483647L / blocksize ||
        seekval > 2147483647L / blocksize) {
        fprintf(stderr, "dd: offset too large\n");
        return 1;
    }
    buf = localbuf;
    if (blocksize > sizeof(localbuf)) {
	buf = malloc(blocksize);
	if (buf == NULL) {
	    fprintf(stderr, "Cannot allocate buffer\n");
	    return 1;
	}
    }
    intotal = 0;
    outtotal = 0;

    if (infile) {
	infd = open(infile, 0);
	if (infd < 0) {
	    perror(infile ? infile : "stdin");
	    if (buf != localbuf)
	        free(buf);
	    return 1;
	}
    } else
    	infd = 0;

    if (outfile) {
        if (fstat(infd, &inputstat) == 0 && stat(outfile, &outputstat) == 0 &&
            inputstat.st_dev == outputstat.st_dev &&
            inputstat.st_ino == outputstat.st_ino) {
            fprintf(stderr, "dd: input and output are the same file\n");
            close(infd);
            return 1;
        }
        outfd = open(outfile, O_WRONLY | O_CREAT | (seekval ? 0 : O_TRUNC), 0666);
        if (outfd < 0) {
            perror(outfile);
	    close(infd);
	    if (buf != localbuf)
	        free(buf);
	    return 1;
	}
    } else {
	outfile = "-";
	outfd = 1;
    }

    if (skipval) {
	if (lseek(infd, skipval * blocksize, 0) < 0) {
	    while (skipval-- > 0) {
		incc = read(infd, buf, blocksize);
		if (incc < 0) {
		    perror(infile ? infile : "stdin");
		    goto cleanup;
		}
		if (incc == 0) {
		    fprintf(stderr, "End of file while skipping\n");
		    goto cleanup;
		}
	    }
	}
    }
    if (seekval) {
	if (lseek(outfd, seekval * blocksize, 0) < 0) {
	    perror(outfile);
	    goto cleanup;
	}
    }
    incc = 0;
    while (records < count && (incc = read(infd, buf, blocksize)) > 0) {
        records++;
        if (incc == blocksize) infull++; else inpart++;
        if (incc == blocksize) outfull++; else outpart++;
	intotal += incc;
	cp = buf;

	if (intflag) {
	    fprintf(stderr, "Interrupted\n");
	    goto cleanup;
	}
	while (incc > 0) {
	    outcc = write(outfd, cp, incc);
	    if (outcc <= 0) {
		perror(outfile);
		goto cleanup;
	    }
	    outtotal += outcc;
	    cp += outcc;
	    incc -= outcc;
	}

    }

    if (incc < 0) {
	perror(infile ? infile : "stdin");
	goto cleanup;
    }

    if (outfile && strcmp(outfile, "-") && fstat(outfd, &outputstat) == 0 &&
        S_ISREG(outputstat.st_mode) &&
        ftruncate(outfd, seekval * blocksize + outtotal) < 0) {
        perror(outfile);
        goto cleanup;
    }
    ret = 0;

  cleanup:
    close(infd);

    if (close(outfd) < 0) {
	perror(outfile);
	ret = 1;
    }

    if (buf != localbuf)
	free(buf);

    fprintf(stderr, "%lu+%lu records in\n", infull, inpart);
    fprintf(stderr, "%lu+%lu records out\n", outfull, outpart);

    return ret;
}


/*
 * Read a number with a possible multiplier.
 * Returns -1 if the number format is illegal.
 */
/* Native file lengths/counts use 32-bit long; reject overflow explicitly. */
static long getnum(char *cp)
{
    unsigned long value = 0, scale = 1;
    if (!isdigit(*cp))
        return -1;
    while (isdigit(*cp)) {
        unsigned digit = *cp++ - '0';
        if (value > (2147483647UL - digit) / 10)
            return -1;
        value = value * 10 + digit;
    }
    if (*cp) {
        switch (*cp++) {
        case 'k': scale=1024; break;
        case 'b': scale=512; break;
        case 'w': scale=sizeof(int); break;
        default: return -1;
        }
        if (*cp || value > 2147483647UL / scale)
            return -1;
    }
    return value * scale;
}
