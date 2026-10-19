/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-independent lifecycle/timing for the upstream NanoTetris game. */
static unsigned long tetris_milliseconds(void)
{
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return now.tv_sec*1000UL+now.tv_nsec/1000000UL;
}

static long tetris_read_score(void)
{
    int fd,n;char text[32],*end;long score;
    if(!tetris_score_path[0])return 0;
    fd=open(tetris_score_path,O_RDONLY);if(fd<0)return 0;
    n=read(fd,text,sizeof(text)-1);close(fd);if(n<=0)return 0;
    text[n]=0;errno=0;score=strtol(text,&end,10);
    if(errno||end==text||score<0)return 0;
    while(*end&&isspace((unsigned char)*end))end++;
    return *end?0:score;
}

static void tetris_save_score(nstate *state)
{
    long disk;int fd,n,failed=0;char temporary[192],text[32];
    if(state->score>state->hiscore)state->hiscore=state->score;
    if(!tetris_score_path[0]||state->hiscore<=state->fhiscore)return;
    disk=tetris_read_score();
    if(disk>=state->hiscore){state->hiscore=state->fhiscore=disk;return;}
    strcpy(temporary,tetris_temp_path);fd=mkstemp(temporary);
    if(fd<0){perror("nxtetris: high score");return;}
    n=snprintf(text,sizeof(text),"%ld\n",state->hiscore);
    if(write(fd,text,n)!=n)failed=1;
    if(close(fd)<0)failed=1;
    if(!failed&&rename(temporary,tetris_score_path)<0)failed=1;
    if(failed){perror("nxtetris: high score");unlink(temporary);return;}
    sync();state->fhiscore=state->hiscore;
    if(tetris_trace)fprintf(stderr,"NXTETRIS_SAVED %ld\n",state->hiscore);
}

static void tetris_signal(int sig)
{
    int saved=errno;char byte=1;signal(sig,tetris_signal);tetris_stopping=1;
    if(tetris_wake[1]>=0)write(tetris_wake[1],&byte,1);errno=saved;
}

static void tetris_cleanup(void)
{
    int writer=tetris_wake[1];tetris_wake[1]=-1;
    if(tetris_wake[0]>=0){GrUnregisterInput(tetris_wake[0]);close(tetris_wake[0]);tetris_wake[0]=-1;}
    if(writer>=0)close(writer);
    if(tetris_state)write_hiscore(tetris_state);
    if(tetris_connected){tetris_connected=0;GrClose();}
    free(tetris_state);tetris_state=NULL;
    if(tetris_trace)fprintf(stderr,"NXTETRIS_EXIT %d\n",getpid());
}

static void tetris_keyboard(nstate *state)
{
    unsigned key=state->event.keystroke.ch;
    if(key=='q'||key=='Q'||key==MWKEY_ESCAPE||key==MWKEY_CANCEL){state->state=STATE_EXIT;return;}
    if(key=='n'||key=='N'||key==MWKEY_APP1){state->state=STATE_NEWGAME;return;}
    if(state->state==STATE_STOPPED)return;
    if(key=='p'||key=='P'){
        state->state=state->state==STATE_PAUSED?STATE_RUNNING:STATE_PAUSED;
        calculate_timeout(state);return;
    }
    if(key=='c'||key=='C'){
        if(state->state==STATE_PAUSED){state->state=STATE_RUNNING;calculate_timeout(state);}return;
    }
    if(state->state!=STATE_RUNNING)return;
    switch(key){
    case MWKEY_LEFT:case 'j':case 'J':case 'h':move_block(state,0);break;
    case MWKEY_RIGHT:case 'k':case 'K':case 'l':move_block(state,1);break;
    case 'd':case 'D':rotate_block(state,0);break;
    case MWKEY_UP:case 'f':case 'F':rotate_block(state,1);break;
    case MWKEY_DOWN:drop_block_1(state);calculate_timeout(state);break;
    case ' ':case MWKEY_MENU:drop_block(state);calculate_timeout(state);break;
    }
}

static void tetris_mouse(nstate *state)
{
    GR_WINDOW_ID id=state->event.button.wid;
    if(!(state->event.button.changebuttons&GR_BUTTON_L))return;
    GrSetFocus(state->main_window);
    if(id==state->new_game_button){state->state=STATE_NEWGAME;return;}
    if(id==state->pause_continue_button){
        state->state=state->state==STATE_PAUSED?STATE_RUNNING:STATE_PAUSED;
        calculate_timeout(state);return;
    }
    if(state->state!=STATE_RUNNING)return;
    if(id==state->left_button)move_block(state,0);
    else if(id==state->right_button)move_block(state,1);
    else if(id==state->clockwise_button)rotate_block(state,1);
    else if(id==state->anticlockwise_button)rotate_block(state,0);
    else if(id==state->drop_button){drop_block(state);calculate_timeout(state);}
}

static void tetris_usage(void)
{
    puts("usage: nxtetris [--seed 0..32767]\n"
         "Arrows: move, rotate up, soft-drop down. Space: hard drop.\n"
         "D/F: rotate. P: pause/resume. C: continue. N: new game. Q/Esc: quit.\n"
         "High score: $HOME/.nxtetris-score");
}

static int tetris_main(int argc,char **argv)
{
    int i,last=-1;char *home=getenv("HOME"),*end,bytes[32];unsigned long seed;
    nstate *state;
    tetris_seed=(unsigned)time(NULL)&32767U;
    if(argc==2&&(!strcmp(argv[1],"--help")||!strcmp(argv[1],"-h"))){tetris_usage();return 0;}
    if(argc!=1){
        if(argc!=3||strcmp(argv[1],"--seed")){tetris_usage();return 2;}
        errno=0;seed=strtoul(argv[2],&end,10);
        if(errno||end==argv[2]||*end||argv[2][0]=='-'||seed>32767UL){tetris_usage();return 2;}
        tetris_seed=seed;
    }
    if(!home||!*home)home="/";
    if(snprintf(tetris_score_path,sizeof(tetris_score_path),"%s%s.nxtetris-score",home,
        home[strlen(home)-1]=='/'?"":"/")>=sizeof(tetris_score_path))tetris_score_path[0]=0;
    if(snprintf(tetris_temp_path,sizeof(tetris_temp_path),"%s%s.nxtetris.XXXXXX",home,
        home[strlen(home)-1]=='/'?"":"/")>=sizeof(tetris_temp_path))tetris_score_path[0]=0;
    tetris_trace=getenv("NANOX_TRACE")!=NULL;
    state=calloc(1,sizeof(*state));if(!state)return 1;
    tetris_state=state;atexit(tetris_cleanup);
    init_game(state);
    if(pipe(tetris_wake)<0)return 1;
    for(i=0;i<2;i++)if(fcntl(tetris_wake[i],F_SETFL,O_NONBLOCK)<0)return 1;
    GrRegisterInput(tetris_wake[0]);signal(SIGTERM,tetris_signal);signal(SIGINT,tetris_signal);
#ifndef NXR_NATIVE_GUI
    signal(SIGHUP,tetris_signal);
#endif
    state->state=STATE_RUNNING;calculate_timeout(state);draw_pause_continue_button(state);
    draw_well(state,1);GrFlush();
    if(tetris_trace)fprintf(stderr,"NXTETRIS_READY %d %u %ld\n",getpid(),state->main_window,state->hiscore);
    while(!tetris_stopping&&state->state!=STATE_EXIT){
        if(state->state!=last){draw_pause_continue_button(state);last=state->state;}
        if(state->state==STATE_RUNNING)GrGetNextEventTimeout(&state->event,timeout_delay(state));
        else GrGetNextEvent(&state->event);
        if(state->event.type==GR_EVENT_TYPE_FDINPUT){while(read(tetris_wake[0],bytes,sizeof(bytes))>0){}}
        else handle_event(state);
        if(state->state==STATE_NEWGAME){new_game(state);calculate_timeout(state);}
        if(state->state==STATE_RUNNING)do_update(state);
        if(tetris_trace&&(state->event.type==GR_EVENT_TYPE_KEY_DOWN||
            state->event.type==GR_EVENT_TYPE_BUTTON_DOWN||state->event.type==GR_EVENT_TYPE_TIMEOUT))
            fprintf(stderr,"NXTETRIS_STATE %d %ld %d %d %d %d\n",state->state,state->score,
                state->current_shape.x,state->current_shape.y,state->current_shape.type,state->current_shape.orientation);
        if(tetris_trace&&state->event.type==GR_EVENT_TYPE_KEY_DOWN)
            fprintf(stderr,"NXTETRIS_INPUT %u\n",state->event.keystroke.ch);
        GrFlush();
    }
    return 0;
}
