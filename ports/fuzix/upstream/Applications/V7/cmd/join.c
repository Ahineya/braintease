/* UNIX V7 source code: see /COPYRIGHT or www.tuhs.org for details. */
/* ANSIfied for FUZIX */

/*	join F1 F2 on stuff */

#include	<stdio.h>
#include	<stdlib.h>
#include	<string.h>
#include	<unistd.h>
#include	<err.h>

#define F1 0
#define F2 1
#define	NFLD	20	/* max field per line */
#define comp() cmp(ppi[F1][jf1],ppi[F2][jf2])

FILE *f[2];
char buf[2][1024];	/*input lines */
char *ppi[2][NFLD];	/* pointers to fields in lines */
char *s1,*s2;
int	jf1	= 1;	/* join of this field of file 1 */
int	jf2	= 1;	/* join of this field of file 2 */
int	olist[2*NFLD];	/* output these fields */
int	olistf[2*NFLD];	/* from these files */
int	no;	/* number of entries in olist */
int	sep1	= ' ';	/* default field separator */
int	sep2	= '\t';
const char *null	= "";
int	unpub1;
int	unpub2;
int	aflg;

int cmp(const char *s1, const char *s2)
{
	return(strcmp(s1, s2));
}

/* Ripple adaptation: bounded complete records and explicit missing fields. */
int input(int n) {
    int i = 0, j;
    char *bp, *end;
    for (j = 0; j < NFLD; j++) ppi[n][j] = "";
    if (!fgets(buf[n], sizeof(buf[n]), f[n])) {
        if (ferror(f[n])) errx(1, "read error");
        return 0;
    }
    bp = buf[n];
    end = strchr(bp, '\n');
    if (!end && !feof(f[n])) errx(1, "input line exceeds 1022 characters");
    if (end) *end = 0;
    do {
        if (sep1 == ' ') while (*bp == ' ' || *bp == '\t') bp++;
        if (i == NFLD - 1) errx(1, "too many fields (maximum 19)");
        ppi[n][i++] = bp;
        while (*bp && *bp != sep1 && *bp != sep2) bp++;
        if (!*bp) break;
        *bp++ = 0;
    } while (1);
    return i;
}

int output(int on1, int on2)	/* print items from olist */
{
	int i;
	const char *temp;

	if (no <= 0) {	/* default case */
		printf("%s", on1? ppi[F1][jf1]: ppi[F2][jf2]);
		for (i = 0; i < on1; i++)
			if (i != jf1)
				printf("%c%s", sep1, ppi[F1][i]);
		for (i = 0; i < on2; i++)
			if (i != jf2)
				printf("%c%s", sep1, ppi[F2][i]);
		printf("\n");
	} else {
		for (i = 0; i < no; i++) {
			temp = null;
			if (olist[i] >= 0 && olist[i] < NFLD - 1 &&
			    !(olistf[i]==F1 && on1<=olist[i]) &&
			    !(olistf[i]==F2 && on2<=olist[i])) {
				temp = ppi[olistf[i]][olist[i]];
				if (*temp == 0)
					temp = null;
			}
			printf("%s", temp);
			if (i == no - 1)
				printf("\n");
			else
				printf("%c", sep1);
		}
	}
}

int main(int argc, const char *argv[])
{
	int i;
	int n1, n2;
	long top2, bot2;

	while (argc > 1 && argv[1][0] == '-') {
		if (argv[1][1] == '\0')
			break;
		switch (argv[1][1]) {
		case 'a':
			switch(argv[1][2]) {
			case '1':
				aflg |= 1;
				break;
			case '2':
				aflg |= 2;
				break;
			default:
				aflg |= 3;
			}
			break;
		case 'e':
			if (argc < 3)
				errx(1, "usage: join [-jf1 x -jf2 y] [-o list] file1 file2");
			null = argv[2];
			argv++;
			argc--;
			break;
		case 't':
			if (!argv[1][2] || argv[1][3]) errx(1, "use -tCHAR for one separator");
            sep1 = sep2 = argv[1][2];
			break;
		case 'o':
			if (argc < 3)
				errx(1, "usage: join [-jf1 x -jf2 y] [-o list] file1 file2");
			for (no = 0; no < 2*NFLD && argc > 2; no++) {
				if (argv[2][0] == '1' && argv[2][1] == '.') {
					olistf[no] = F1;
					olist[no] = atoi(&argv[2][2]);
				} else if (argv[2][0] == '2' && argv[2][1] == '.') {
					olist[no] = atoi(&argv[2][2]);
					olistf[no] = F2;
				} else
					break;
				argc--;
				argv++;
			}
			break;
        case '1':
        case '2':
            if (argc < 3) errx(1, "missing join field");
            if (argv[1][1] == '1') jf1 = atoi(argv[2]);
            else jf2 = atoi(argv[2]);
            argc--; argv++;
            break;
		case 'j':
			if (argc < 3)
				errx(1, "usage: join [-jf1 x -jf2 y] [-o list] file1 file2");
			if (argv[1][2] == '1')
				jf1 = atoi(argv[2]);
			else if (argv[1][2] == '2')
				jf2 = atoi(argv[2]);
			else
				jf1 = jf2 = atoi(argv[2]);
			argc--;
			argv++;
			break;
		default: errx(1, "unknown option %s", argv[1]);
		}
		argc--;
		argv++;
	}
	for (i = 0; i < no; i++)
		olist[i]--;	/* 0 origin */
	if (argc != 3)
		errx(1, "usage: join [-jf1 x -jf2 y] [-o list] file1 file2");
	jf1--;
	jf2--;	/* everyone else believes in 0 origin */
	if (jf1 < 0 || jf1 >= NFLD - 1 || jf2 < 0 || jf2 >= NFLD - 1)
		errx(1, "join field out of range");
	s1 = ppi[F1][jf1];
	s2 = ppi[F2][jf2];
	if (!strcmp(argv[1], "-"))
		f[F1] = stdin;
	else if ((f[F1] = fopen(argv[1], "r")) == NULL)
		errx(1, "can't open %s", argv[1]);
	if ((f[F2] = fopen(argv[2], "r")) == NULL)
		errx(1, "can't open %s", argv[2]);

#define get1() n1=input(F1)
#define get2() n2=input(F2)
	get1();
	bot2 = ftell(f[F2]);
	get2();
	while(n1>0 && n2>0 || aflg!=0 && n1+n2>0) {
		if(n1>0 && n2>0 && comp()>0 || n1==0) {
			if(aflg&2) output(0, n2);
			bot2 = ftell(f[F2]);
			get2();
		} else if(n1>0 && n2>0 && comp()<0 || n2==0) {
			if(aflg&1) output(n1, 0);
			get1();
		} else /*(n1>0 && n2>0 && comp()==0)*/ {
			while(n2>0 && comp()==0) {
				output(n1, n2);
				top2 = ftell(f[F2]);
				get2();
			}
			fseek(f[F2], bot2, 0);
			get2();
			get1();
			for(;;) {
				if(n1>0 && n2>0 && comp()==0) {
					output(n1, n2);
					get2();
				} else if(n1>0 && n2>0 && comp()<0 || n2==0) {
					fseek(f[F2], bot2, 0);
					get2();
					get1();
				} else /*(n1>0 && n2>0 && comp()>0 || n1==0)*/{
					fseek(f[F2], top2, 0);
					bot2 = top2;
					get2();
					break;
				}
			}
		}
	}
	return(fflush(stdout) < 0 ? 1 : 0);
}
