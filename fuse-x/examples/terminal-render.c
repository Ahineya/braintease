/* Graphical-terminal acceptance fixture: repaint a large TUI on SIGWINCH. */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <termios.h>
static volatile int resized=1;
static void changed(int n){resized=1;signal(n,changed);}
int main(void){
    struct winsize size;unsigned x,y;char line[241];
    signal(SIGWINCH,changed);
    for(;;){
        if(resized){
            resized=0;if(ioctl(0,TIOCGWINSZ,&size)<0)return 1;
            printf("\033[?2026h\033[2J\033[H\033[37;40mFuse-X native terminal / Привіт, Павло!\n");
            for(y=1;y<size.ws_row;y++){
                for(x=0;x<size.ws_col;x++)line[x]=x%32<12?'A'+(x+y)%26:' ';
                line[size.ws_col]=0;
                printf("\033[%u;1H\033[%u;%um%s",y+1,30+y%8,40+(y/8)%8,line);
            }
            printf("\033[0m\033[?25l\033[?2026l");fflush(stdout);
        }
        {fd_set input;struct timeval timeout={1,0};FD_ZERO(&input);FD_SET(0,&input);
         if(select(1,&input,0,0,&timeout)>0){char c;if(read(0,&c,1)<=0||c=='q')return 0;}}
    }
}
