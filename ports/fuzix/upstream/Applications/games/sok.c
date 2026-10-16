/* SPDX-License-Identifier: GPL-2.0-only */
/* Ripple adaptation of FUZIX's tiny Sokoban clone. Movement/undo are based on
 * upstream sok.c; terminal, validation and progress I/O are adapted for C16. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <pwd.h>
#include <errno.h>
#include "sok.h"

static struct level map, base_map;
static unsigned level, level_count, moves, left;
static int level_fd, tty_active;
static struct termios tcsave;
static char progress[160];
#define MAX_UNDO 64
#define BLOCK_MOVED 0x80
#define MAP_WALL '#'
#define MAP_FLOOR ' '
#define MAP_BLOCK '$'
#define MAP_BLOCK_ON '*'
#define MAP_TARGET '.'
static uint8_t undolist[MAX_UNDO], undop;
static int8_t delta[5][2] = {{0,-1},{0,0},{1,0},{-1,0},{0,1}};

static void moveto(unsigned y, unsigned x) { printf("\033[%u;%uH", y+1, x+1); }
static void draw(unsigned y, unsigned x, unsigned c) {
    moveto(2+y, x+left); putchar(c); fflush(stdout);
}
static void drawplayer(void) {
    draw(map.py, map.px, map.map[map.py][map.px] == MAP_TARGET ? '+' : '@');
}
static void cleanup(void) {
    if (tty_active) {
        tcsetattr(0, TCSANOW, &tcsave);
        fputs("\033[?25h\033[?1049l", stdout); fflush(stdout);
        tty_active = 0;
    }
}
static void interrupted(int sig) { cleanup(); _exit(128+sig); }
static void tty_init(void) {
    struct termios raw;
    struct winsize win;
    if (tcgetattr(0, &tcsave) == 0) {
        raw = tcsave;
        raw.c_lflag &= ~(ICANON|ECHO|ECHOE|ECHOK);
        raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 1;
        if (tcsetattr(0, TCSANOW, &raw)) { perror("sokoban: terminal"); exit(1); }
        tty_active = 1;
        atexit(cleanup);
        signal(SIGINT, interrupted); signal(SIGTERM, interrupted); signal(SIGHUP, interrupted);
        signal(SIGTSTP, SIG_IGN);
        fputs("\033[?1049h\033[?25l", stdout);
    }
    if (ioctl(0, TIOCGWINSZ, &win)) {win.ws_col=80;win.ws_row=24;}
    if (!win.ws_col) win.ws_col=80;
    if (!win.ws_row) win.ws_row=24;
    if (win.ws_col < MAP_W || win.ws_row < MAP_H+4) {
        fputs("sokoban: terminal must be at least 32 columns by 18 rows\n", stderr); exit(1);
    }
    left = (win.ws_col > 240 ? 240 : win.ws_col) / 2 - MAP_W / 2;
}
static int one_key(int optional) {
    unsigned char c;
    int n;
    do {
        n=read(0,&c,1);
        if(!n && optional)return -1;
    } while ((n<0 && errno==EINTR) || (!n && tty_active));
    if (n<0) {perror("sokoban: input");exit(1);}
    if (!n || c==4) exit(0);
    if (c>127) return 0;
    return c;
}
static unsigned getkey(void) {
    int c=one_key(0);
    if (c==27) {
        c=one_key(1);
        if (c=='[' || c=='O') {
            c=one_key(1);
            if (c=='A') return 'K'; if(c=='B') return 'J';
            if(c=='C') return 'L'; if(c=='D') return 'H';
        }
        return 0;
    }
    c=toupper(c);
    if(c=='W')return 'K'; if(c=='A')return 'H'; if(c=='S')return 'J'; if(c=='D')return 'L';
    return c;
}
static uint8_t fixup(uint8_t code)
{
	if (code == MAP_BLOCK)
		code = MAP_FLOOR;
	if (code == MAP_BLOCK_ON)
		code = MAP_TARGET;
	return code;
}

static void move(uint8_t dir)
{
	int8_t *dp;
	uint8_t ny, nx, n;

	dp = delta[dir];
	ny = map.py + *dp;
	nx = map.px + dp[1];

	if (ny >= MAP_H || nx >= MAP_W) return;
	n = map.map[ny][nx];

	if (n == MAP_WALL)
		return;

	if (n == MAP_BLOCK || n == MAP_BLOCK_ON) {
		uint8_t by = ny + *dp;
		uint8_t bx = nx + dp[1];
		uint8_t m;
		if (by >= MAP_H || bx >= MAP_W) return;
		m = map.map[by][bx];
		if (m == MAP_WALL || m == MAP_BLOCK || m == MAP_BLOCK_ON)
			return;
		/* Adjust count of blocks positioned */
		if (map.map[by][bx] == MAP_TARGET) {
			draw(by, bx, MAP_BLOCK_ON);
			map.done++;
		} else
			draw(by, bx, MAP_BLOCK);

		/* Move on the map */
		map.map[by][bx] = fixup(base_map.map[by][bx]) == MAP_TARGET ? MAP_BLOCK_ON : MAP_BLOCK;
		map.map[ny][nx] = fixup(base_map.map[ny][nx]);
		if (map.map[ny][nx] == MAP_TARGET)
			map.done--;
		/* We don't need to redraw dy dx as we will put the player on it */
		dir |= BLOCK_MOVED;	/* For undo */
	}
	draw(map.py, map.px, map.map[map.py][map.px]);
	map.py += *dp;
	map.px += *++dp;
	drawplayer();
	moves++;
	if (undop == MAX_UNDO) {
		memmove(undolist, undolist + 1, MAX_UNDO - 1);
		undop--;
	}
	undolist[undop++] = dir;
}



static uint8_t reverse[] = "L KJH";

static void undo(void)
{
	uint8_t op;
	uint8_t dir;
	uint8_t c;
	int8_t *dp;
	uint8_t pry, prx;

	if (undop == 0)
		return;
	op = undolist[--undop];
	dir = op & 0x7F;
	/* As we are reversing a move we know we won't be pushing any other
	   block */
	if (op & BLOCK_MOVED) {
		/* We need to reverse a block */
		if (map.map[map.py][map.px] == MAP_TARGET)
			map.done++;
		map.map[map.py][map.px] = fixup(base_map.map[map.py][map.px]) == MAP_TARGET ? MAP_BLOCK_ON : MAP_BLOCK;
		/* The block after the one in direction goes back to previous
		   (we don't allow a multi-block push) */
		dp = delta[dir];
		pry = map.py + *dp;
		prx = map.px + *++dp;
		c = fixup(base_map.map[pry][prx]);
		map.map[pry][prx] = c;
		draw(pry, prx, c);
		if (c == MAP_TARGET)
			map.done--;
	}
	{
		unsigned saved = undop;
		move(reverse[dir] - 'H');
		undop = saved;
		if (moves >= 2) moves -= 2;
	}
}


static int notifier(const char *text) {
    unsigned c;
    moveto(0,0); printf("\033[K%s (y/n)",text); fflush(stdout);
    do {c=getkey();} while(c!='Y' && c!='N' && c!='Q');
    return c=='Y';
}
static void redraw_all(void) {
    unsigned y;
    fputs("\033[H\033[2J",stdout);
    printf("Sokoban - level %u/%u - %u moves",level,level_count-1,moves);
    for(y=0;y<MAP_H;y++) {
        moveto(2+y,left); fwrite(map.map[y],1,MAP_W,stdout);
    }
    moveto(MAP_H+3,0);
    fputs("Arrows/hjkl/WASD: move  u: undo  r: restart  q: quit",stdout);
    drawplayer(); fflush(stdout);
}
static void start_level(void) {
    moves=undop=0;
    memcpy(&map,&base_map,sizeof(map));
    redraw_all();
}
static void key(void) {
    unsigned c=getkey();
    if(c=='Q') {if(notifier("Quit?"))exit(0);}
    else if(c=='R') {if(notifier("Restart level?"))start_level();}
    else if(c==18) redraw_all();
    else if(c=='U') undo();
    else if(c=='H'||c=='J'||c=='K'||c=='L') move(c-'H');
    moveto(0,0); printf("\033[KSokoban - level %u/%u - %u moves",level,level_count-1,moves); fflush(stdout);
}
static int load_level(unsigned n) {
    unsigned done=0, y,x,boxes=0,goals=0,placed=0;
    int count;
    char *dest=(char *)&base_map;
    if(n>=level_count)return 0;
    if(lseek(level_fd,(unsigned long)n*MAP_SEEK,SEEK_SET)<0)goto bad;
    while(done<sizeof(base_map)) {
        count=read(level_fd,dest+done,sizeof(base_map)-done);
        if(count<=0)goto bad;
        done+=count;
    }
    if(base_map.py>=MAP_H||base_map.px>=MAP_W)goto bad;
    if(!n)return 1;
    for(y=0;y<MAP_H;y++)for(x=0;x<MAP_W;x++) {
        unsigned c=base_map.map[y][x];
        if(c!='#'&&c!=' '&&c!='$'&&c!='.'&&c!='*')goto bad;
        if(c=='$'||c=='*')boxes++;
        if(c=='.'||c=='*')goals++;
        if(c=='*')placed++;
    }
    if(!boxes||boxes!=goals||boxes!=base_map.blocks||placed!=base_map.done)goto bad;
    if(base_map.map[base_map.py][base_map.px]!=' '&&base_map.map[base_map.py][base_map.px]!='.')goto bad;
    return 1;
bad:
    fprintf(stderr,"sokoban: invalid or unreadable level %u\n",n);exit(1);return 0;
}
static void save_progress(void) {
    FILE *f;
    if(!progress[0])return;
    f=fopen(progress,"w");
    if(!f){perror("sokoban: progress");return;}
    if(fprintf(f,"%u\n",level)<0)fputs("sokoban: could not save progress\n",stderr);
    if(fclose(f))perror("sokoban: progress");
}
static unsigned number(const char *s) {
    char *end;
    unsigned long n;
    errno=0;n=strtoul(s,&end,10);
    if(errno||!*s||*end||n<1||n>32767UL){fputs("sokoban: invalid level number\n",stderr);exit(1);}
    return (unsigned)n;
}
int main(int argc,char **argv) {
    const char *path="/usr/lib/sok/sok.levels", *home;
    struct stat st;
    struct passwd *pw;
    int i=1,explicit_level=0;
    FILE *f;
    char line[32],*end;
    unsigned long n;
    if(argc==2&&!strcmp(argv[1],"--help")) {
        puts("Usage: sokoban [-l level] [level-file]\nPush every $ onto a .; * marks a box on a goal.\nArrows/hjkl/WASD move; u undoes; r restarts; q quits.\nDefault progress is saved in ~/.sokoban.\nSokEvo puzzles by Lee J Haywood; see /usr/lib/sok/README.");return 0;
    }
    if(i<argc&&!strcmp(argv[i],"-l")) {
        if(i+1>=argc){fputs("sokoban: -l requires a level\n",stderr);return 1;}
        level=number(argv[i+1]);explicit_level=1;i+=2;
    }
    if(i<argc) {path=argv[i++];explicit_level=1;}
    if(i!=argc || path[0]=='-') {fputs("Usage: sokoban [-l level] [level-file]\n",stderr);return 1;}
    level_fd=open(path,O_RDONLY);
    if(level_fd<0){perror(path);return 1;}
    if(fstat(level_fd,&st)||st.st_size<2UL*MAP_SEEK||st.st_size%MAP_SEEK||st.st_size/MAP_SEEK>32768UL) {
        fputs("sokoban: invalid level file\n",stderr);return 1;
    }
    level_count=st.st_size/MAP_SEEK;
    if(!level)level=1;
    if(!explicit_level) {
        home=getenv("HOME");
        if(!home || !*home){pw=getpwuid(getuid());home=pw?pw->pw_dir:"/";}
        if(strlen(home)+10<sizeof(progress)) {
            strcpy(progress,home);strcat(progress,"/.sokoban");
            f=fopen(progress,"r");
            if(f) {
                if(fgets(line,sizeof(line),f)) {
                    n=strtoul(line,&end,10);
                    if(n>=1&&n<level_count&&(*end=='\n'||!*end))level=n;
                }
                fclose(f);
            }
        }
    }
    if(level>=level_count){fputs("sokoban: level out of range\n",stderr);return 1;}
    load_level(level); /* Reject bad data before changing terminal settings. */
    tty_init();
    do {
        start_level();
        while(map.done!=map.blocks)key();
        moveto(0,0);printf("\033[KLevel %u done in %u moves. Press a key.",level,moves);fflush(stdout);
        ++level;save_progress();getkey();
    } while(load_level(level));
    puts("\nAll levels complete!");
    close(level_fd);return 0;
}
