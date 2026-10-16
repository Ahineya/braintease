/* cut - extract columns from a file or stdin.  Author: Michael J. Holme
 *
 *      Copyright 1989, Michael John Holme, All rights reserved.
 *      This code may be freely distributed, provided that this notice
 *      remains intact.
 *
 *      V1.1: 6th September 1989
 *
 *      Bugs, criticisms, etc,
 *      c/o Mark Powell
 *          JANET sq79@uk.ac.liv
 *          ARPA  sq79%liv.ac.uk@nsfnet-relay.ac.uk
 *          UUCP  ...!mcvax!ukc!liv.ac.uk!sq79
 *-------------------------------------------------------------------------
 *      Changed for POSIX1003.2/Draft10 conformance
 *      Thomas Brupbacher (tobr@mw.lpc.ethz.ch), September 1990.
 *      Changes:
 *          - separation of error messages ( stderr) and output (stdout).
 *          - support for -b and -n (no effect, -b acts as -c)
 *          - support for -s
 *-------------------------------------------------------------------------
 */

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#define MAX_FIELD	80	/* Pointers to the beginning of each field
				 * are stored in columns[], if a line holds
				 * more than MAX_FIELD columns the array
				 * boundary is exceed. But unlikely at 80 */

#define MAX_ARGS	32	/* Maximum number of fields following -f or
				 * -c switches                                                                                                    */
int args[MAX_ARGS * 2];
int num_args;

/* Lots of new defines, should easen maintainance... */
#define DUMP_STDIN	0	/* define for mode: no options */
#define OPTIONF		1	/* define for mode: option -f  */
#define OPTIONC		2	/* define for mode: option -c  */
#define OPTIONB		3	/* define for mode: option -b  */
#define NOTSET		0	/* option not selected         */
#define SET		1	/* option selected             */

/* Defines for the warnings */
#define DELIMITER_NOT_APPLICABLE	0
#define OVERRIDING_PREVIOUS_MODE	1
#define OPTION_NOT_APPLICABLE		2
#define UNKNOWN_OPTION			3
#define FILE_NOT_READABLE		4

/* Defines for the fatal errors */
#define SYNTAX_ERROR			101
#define POSITION_ERROR			102
#define USAGE				103
#define LINE_TO_LONG_ERROR		104
#define RANGE_ERROR			105
#define MAX_FIELDS_EXCEEDED_ERROR	106
#define MAX_ARGS_EXCEEDED_ERROR		107


int  mode;			/* 0 = dump stdin to stdout, 1=-f, 2=-c */
int  flag_i;			/* SET = -i set on command line         */
int  flag_s;			/* SET = -s set on command line         */
char delim = '\t';		/* default delimiting character         */
FILE *fd;
char *name;
char line[BUFSIZ];
int  exit_status;


void warn(int warn_number, char *option)
{
    static const char *warn_msg[] =
    {
	"%s: Option -d allowed only with -f\n",
	"%s: -%s overrides earlier option\n",
	"%s: -%s not allowed in current mode\n",
	"%s: unknown option %s\n",
        "%s: Cannot open %s\n"
    };

    fprintf(stderr, warn_msg[warn_number], name, option);
    exit_status = warn_number + 1;

}

void cuterror(int err)
{
    static const char *err_mes[] =
    {
	"%s: syntax error\n",
	"%s: position must be >0\n",
        "%s: usage: cut [-f args [-i] [-d x]]|[-c args] [filename [...]]\n",
	"%s: line longer than BUFSIZ\n",
	"%s: range must not decrease from left to right\n",
	"%s: MAX_FIELD exceeded\n",
	"%s: MAX_ARGS exceeded\n"
    };

    fprintf(stderr, err_mes[err - 101], name);
    exit(err);
}


void get_args(void)
{
    int i = 0;
    int arg_ptr = 0;
    int flag;

    num_args = 0;
    do {
	if (num_args == MAX_ARGS)
	    cuterror(MAX_ARGS_EXCEEDED_ERROR);
	if (!isdigit(line[i]) && line[i] != '-')
	    cuterror(SYNTAX_ERROR);

	args[arg_ptr] = 1;
	args[arg_ptr + 1] = BUFSIZ;
	flag = 1;

	while (line[i] != ',' && line[i] != 0) {
	    if (isdigit(line[i])) {
		args[arg_ptr] = 0;
		while (isdigit(line[i]))
		    args[arg_ptr] = 10 * args[arg_ptr] + line[i++] - '0';
		if (!args[arg_ptr])
		    cuterror(POSITION_ERROR);
		arg_ptr++;
	    }
	    if (line[i] == '-') {
		arg_ptr |= 1;
		i++;
		flag = 0;
	    }
	}
	if (flag && arg_ptr & 1)
	    args[arg_ptr] = args[arg_ptr - 1];
	if (args[num_args * 2] > args[num_args * 2 + 1])
	    cuterror(RANGE_ERROR);
	num_args++;
	arg_ptr = num_args * 2;
    }
    while (line[i++]);
}


void cut(void)
{
    int i, j, length, maxcol;
    char *columns[MAX_FIELD];

    while (fgets(line, BUFSIZ, fd)) {

	length = strlen(line) - 1;
	*(line + length) = 0;

	switch (mode) {
	case DUMP_STDIN:
	    fputs(line, stdout);
	    break;

	case OPTIONF:
	    maxcol = 0;
	    columns[maxcol++] = line;
	    for (i = 0; i < length; i++) {
		if (*(line + i) == delim) {
		    *(line + i) = 0;
		    if (maxcol == MAX_FIELD)
			cuterror(MAX_FIELDS_EXCEEDED_ERROR);
		    columns[maxcol] = line + i + 1;
		    while (*(line + i + 1) == delim && flag_i) {
			columns[maxcol]++;
			i++;
		    }
		    maxcol++;
		}
	    }
	    if (maxcol == 1) {
		if (flag_s != SET)
		    fputs(line, stdout);
	    } else {
		for (i = 0; i < num_args; i++) {
		    for (j = args[i * 2]; j <= args[i * 2 + 1]; j++)
			if (j <= maxcol) {
			    printf("%s", columns[j - 1]);
			    if (i != num_args - 1 || j != args[i * 2 + 1])
				putchar(delim);
			}
		}
	    }
	    break;

	case OPTIONC:
	    for (i = 0; i < num_args; i++) {
		for (j = args[i * 2]; j <= (args[i * 2 + 1] > length ? length :
					    args[i * 2 + 1]); j++)
		    putchar(*(line + j - 1));
	    }
	}

	if (maxcol == 1 && flag_s == SET) {
	    ;
	} else {
	    putchar('\n');
	}
    }
}


int main(int argc, char *argv[])
{
    int i = 1;
    int numberFilenames = 0;
    name = argv[0];

    if (argc == 1)
	cuterror(USAGE);

    /* Ripple: standard option parsing supports attached values and bounds checks. */
    {
        int option;
        while ((option = getopt(argc, argv, "b:c:d:f:isn")) != -1) {
            switch (option) {
            case 'd':
                if (strlen(optarg) != 1) cuterror(SYNTAX_ERROR);
                delim = optarg[0]; break;
            case 'f': case 'b': case 'c':
                if (strlcpy(line, optarg, sizeof(line)) >= sizeof(line)) cuterror(LINE_TO_LONG_ERROR);
                mode = option == 'f' ? OPTIONF : option == 'b' ? OPTIONB : OPTIONC;
                break;
            case 'i': flag_i = SET; break;
            case 's': flag_s = SET; break;
            case 'n': break;
            default: cuterror(USAGE);
            }
        }
    }
    if (!mode) cuterror(USAGE);

    /* Here follow the checks, if the selected options are reasonable. */

    if (mode == OPTIONB)	/* since in Minix char == byte */
	mode = OPTIONC;

    /* Flag -s is only allowed with -f, otherwise warn and reset flag_s */
    if (flag_s == SET && (mode == OPTIONB || mode == OPTIONC)) {
	warn(OPTION_NOT_APPLICABLE, "s");
	flag_s = NOTSET;
    }

    /* Flag -i is only allowed with -f, otherwise warn and reset flag_i */
    if (flag_i == SET && mode == OPTIONF) {
	warn(OPTION_NOT_APPLICABLE, "s");
	flag_i = NOTSET;
    }

    get_args();

    if (optind == argc) { fd = stdin; cut(); }
    else for (i = optind; i < argc; i++) {
        if (!strcmp(argv[i], "-")) fd = stdin;
        else fd = fopen(argv[i], "r");
        if (!fd) warn(FILE_NOT_READABLE, argv[i]);
        else { cut(); if (fd != stdin) fclose(fd); }
    }

    return (exit_status);
}
