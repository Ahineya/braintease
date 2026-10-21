/* SPDX-License-Identifier: MIT */
/* Compare Net's real drawing commands and retained state with upstream.
 * Including the engine also lets us check its private incremental cache. */
#include "net.c"

static SHA_State trace;
static int failures, frames, clips;

void activate_timer(frontend *f) { (void)f; }
void deactivate_timer(frontend *f) { (void)f; }
static void check(int ok, const char *what)
{
    if (!ok) {
        if (failures < 5) printf("FAIL frame %d: %s\n", frames, what);
        failures++;
    }
}
static void record(const char *s) { SHA_Bytes(&trace, s, strlen(s)); }
static void numbers(const char *op, int a, int b, int c, int d, int e)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s %d %d %d %d %d\n", op, a, b, c, d, e);
    record(buf);
}
static void rect(drawing *dr, int x, int y, int w, int h, int c)
{ (void)dr; numbers("rect", x, y, w, h, c); }
static void line(drawing *dr, int x, int y, int xx, int yy, int c)
{ (void)dr; numbers("line", x, y, xx, yy, c); }
static void polygon(drawing *dr, const int *p, int n, int fill, int outline)
{
    int i; (void)dr;
    numbers("polygon", n, fill, outline, 0, 0);
    for (i = 0; i < n; i++) numbers("point", p[i*2], p[i*2+1], 0, 0, 0);
}
static void region(drawing *dr, int x, int y, int w, int h)
{ (void)dr; clips++; numbers("clip", x, y, w, h, 0); }
static void unregion(drawing *dr) { (void)dr; record("unclip\n"); }
static void update(drawing *dr, int x, int y, int w, int h)
{ (void)dr; numbers("update", x, y, w, h, 0); }
static const drawing_api api = {
    .version=1, .draw_rect=rect, .draw_line=line, .draw_polygon=polygon,
    .clip=region, .unclip=unregion, .draw_update=update
};

static void render(drawing *dr, game_drawstate *ds, const game_state *old,
                   const game_state *state, int dir, game_ui *ui, float t,
                   float flash, int idle)
{
    int x, y, correct=1; char buf[40];
    clips=0; frames++;
    numbers("frame", frames, 0, 0, 0, 0);
    game_redraw(dr, ds, old, state, dir, ui, t, flash);
    for (y=-1; y <= ds->height; y++) for (x=-1; x <= ds->width; x++) {
        unsigned long v=visible(ds,x,y);
        if (v != todraw(ds,x,y)) correct=0;
        snprintf(buf, sizeof(buf), "%08lx\n", v);
        record(buf);
    }
    check(correct, "drawing cache lost packed tile flags");
    if (idle) check(clips==0, "unchanged board redraws tiles");
}

int main(int argc, char **argv)
{
    game_params *params=default_params();
    game_state *state, *next;
    game_ui *ui; game_drawstate *ds; drawing *dr;
    random_state *rs=random_new("Net drawing regression",22);
    char *desc, *aux=NULL, move[40]; unsigned char sum[20];
    int i, j, x, y, size=argc>2?atoi(argv[2]):47;
    decode_params(params, argc>1?argv[1]:"5x5");
    if (validate_params(params,true)) fatal("invalid test parameters");
    desc=new_game_desc(params,rs,&aux,false);
    state=new_game(NULL,params,desc); ui=new_ui(state); ui->cur_visible=false;
    dr=drawing_new(&api,NULL,NULL); ds=game_new_drawstate(dr,state);
    game_set_size(dr,ds,params,size); SHA_Init(&trace);
    render(dr,ds,NULL,state,1,ui,0,0,0);
    render(dr,ds,NULL,state,1,ui,0,0,1);
    /* Corners exercise outside-grid edges; centre exercises the source box.
     * Four turns must return the exact original rendering, in both directions. */
    for (i=0; i<5; i++) {
        x=i==4?params->width/2:(i%2)*(params->width-1);
        y=i==4?params->height/2:(i/2)*(params->height-1);
        for (j=0; j<8; j++) {
            sprintf(move,"%c%d,%d",j<4?'A':'C',x,y);
            next=execute_move(state,move); check(next!=NULL,"rotation accepted");
            if (!next) return 1;
            render(dr,ds,state,next,1,ui,0,0,0);
            render(dr,ds,state,next,1,ui,ROTATE_TIME/2,0,0);
            render(dr,ds,NULL,next,1,ui,0,0,0);
            render(dr,ds,NULL,next,1,ui,0,0,1);
            /* Undo animation must retain the same high flags too. */
            render(dr,ds,next,state,-1,ui,ROTATE_TIME/2,0,0);
            render(dr,ds,NULL,state,1,ui,0,0,0);
            render(dr,ds,NULL,next,1,ui,0,0,0);
            free_game(state); state=next;
        }
        sprintf(move,"L%d,%d",x,y); next=execute_move(state,move);
        free_game(state); state=next;
        render(dr,ds,NULL,state,1,ui,0,0,0);
        render(dr,ds,NULL,state,1,ui,0,0,1);
    }
    ui->cur_visible=true;
    render(dr,ds,NULL,state,1,ui,0,0,0);
    render(dr,ds,NULL,state,1,ui,0,0,1);
    for (i=1; i<8; i++) render(dr,ds,NULL,state,1,ui,0,FLASH_FRAME*i,0);
    render(dr,ds,NULL,state,1,ui,0,0,0);
    render(dr,ds,NULL,state,1,ui,0,0,1);
    SHA_Final(&trace,sum); printf("NET_DRAW ");
    for (i=0; i<20; i++) printf("%02x",(unsigned)sum[i]);
    printf(" frames=%d failures=%d\n",frames,failures);
    game_free_drawstate(dr,ds); drawing_free(dr); free_ui(ui); free_game(state);
    free_params(params); random_free(rs); sfree(desc); sfree(aux);
    return failures!=0;
}
