/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Kolf's terrain/friction/cup constants are adapted from KDE Kolf, copyright
 * Jason Katz-Brown (2002-2005), Stefan Majewsky (2010). Collision integration
 * here is a new bounded circle/segment solver, not the original Box2D solver. */
#include "golf.h"
#include <math.h>
#include <string.h>
#define RADIUS 4.0
static double square(double x){return x*x;}
static double magnitude(double x,double y){return sqrt(x*x+y*y);}
int golf_rectangle(int t){return t==G_BRIDGE||t==G_FLOATER||t==G_WINDMILL||t==G_SIGN;}
static int strut(int t){return t==G_BRIDGE||t==G_FLOATER||t==G_WINDMILL;}
static int raised(const GolfObject *o){return o->type==G_CUP||o->type==G_BUMPER||o->type==G_BLACKHOLE||(o->type==G_SLOPE&&!o->ground);}
static int limited(const GolfGame *g){return g->hole->maxstrokes!=4&&g->strokes>=g->hole->maxstrokes;}
int golf_finished(const GolfGame *g){return g->state==G_HOLED||g->state==G_LIMIT;}
int golf_active(const GolfGame *g){return g->state!=G_HAZARD&&!golf_finished(g)&&(g->animated||g->state==G_ROLLING||g->state==G_WARPING);}
static void bounds(GolfObject *o){
    if(o->type==G_WALL){
        o->min_x=(int)(o->ax<o->bx?o->ax:o->bx)-1;o->max_x=(int)(o->ax>o->bx?o->ax:o->bx)+1;
        o->min_y=(int)(o->ay<o->by?o->ay:o->by)-1;o->max_y=(int)(o->ay>o->by?o->ay:o->by)+1;
    }else{
        int rect=o->type==G_SLOPE||golf_rectangle(o->type);
        o->min_x=(int)(rect?o->x:o->x-o->w/2)-1;o->max_x=(int)(rect?o->x+o->w:o->x+o->w/2)+1;
        o->min_y=(int)(rect?o->y:o->y-o->h/2)-1;o->max_y=(int)(rect?o->y+o->h:o->y+o->h/2)+1;
    }
    o->bounded=1;
}
int golf_prepare(GolfHole *h) {
    int i,cups=0;GolfObject *o;
    if(!h->has_ball||!h->has_info)return 0;
    for(i=0;i<h->count;i++){
        o=h->objects+i;if(o->type==G_CUP)cups++;
        if(o->type==G_WALL){
            o->ax+=o->x;o->ay+=o->y;o->bx+=o->x;o->by+=o->y;
            o->x=o->y=0;o->length=magnitude(o->bx-o->ax,o->by-o->ay);
            /* Degenerate saved walls have no collision surface. */
            if(o->length>0.001){o->nx=-(o->by-o->ay)/o->length;o->ny=(o->bx-o->ax)/o->length;}
        }else if(o->type==G_FLOATER){
            o->x=o->ax;o->y=o->ay;o->length=magnitude(o->bx-o->ax,o->by-o->ay);
            if(o->length>0.001){o->nx=(o->bx-o->ax)/o->length*o->speed/3.5;o->ny=(o->by-o->ay)/o->length*o->speed/3.5;}
        }else if(o->type==G_WINDMILL){
            /* Kolf exchanges the open edge when the guard moves to the top. */
            if(!o->bottom){o->walls=(o->walls&~(G_TOP|G_BOTTOM))|((o->walls&G_TOP)?G_BOTTOM:0);}
            else o->walls&=~G_BOTTOM;
            o->length=o->w/4/1.07-2;if(o->length<0)o->length=0;
        }else if(o->type==G_SLOPE){
            double amount=0.013*o->grade;if(!o->reversed)amount=-amount;
            o->ax=0;o->ay=0;
            if(o->gradient==G_HORIZONTAL)o->ax=amount;
            else if(o->gradient==G_VERTICAL)o->ay=amount;
            else if(o->gradient==G_DIAGONAL||o->gradient==G_CROSS){
                double len=magnitude(o->w,o->h);o->ax=o->h/len*amount;o->ay=o->w/len*amount;
                if(o->gradient==G_CROSS)o->ax=-o->ax;
            }
        }else if(o->type==G_BLACKHOLE){
            double angle=-o->exit_degrees*0.017453292519943295;
            o->nx=cos(angle);o->ny=sin(angle);
        }
        bounds(o);
    }
    return cups>0;
}
int golf_contains(const GolfObject *o,double x,double y) {
    double dx,dy;
    if(o->bounded){int ix=(int)x,iy=(int)y;if(ix<o->min_x||ix>o->max_x||iy<o->min_y||iy>o->max_y)return 0;}
    if(golf_rectangle(o->type))return x>=o->x&&x<=o->x+o->w&&y>=o->y&&y<=o->y+o->h;
    if(o->type==G_SLOPE){
        if(o->gradient!=G_ELLIPTIC){
            dx=(x-o->x)/o->w;dy=(y-o->y)/o->h;
            if(dx<0||dx>1||dy<0||dy>1)return 0;
            if(o->gradient==G_DIAGONAL)return o->reversed?dx+dy<=1:dx+dy>=1;
            if(o->gradient==G_CROSS)return o->reversed?dx>=dy:dx<=dy;
            return 1;
        }
        dx=(x-o->x-o->w/2)/(o->w/2);dy=(y-o->y-o->h/2)/(o->h/2);
    }else{dx=(x-o->x)/(o->w/2);dy=(y-o->y)/(o->h/2);}
    return dx*dx+dy*dy<=1;
}
/* An elevated slope must fit completely; small objects and the ball use
 * overlap, as Kolf's strut selection does. Struts occupy one shared level. */
static int carrier(const GolfGame *g,const GolfObject *o,double x,double y) {
    int i,ix=(int)x,iy=(int)y;const GolfObject *p;
    for(i=g->hole->count-1;i>=0;i--){
        p=&g->live[i].object;if(!strut(p->type))continue;
        if(ix+4<p->min_x||ix-4>p->max_x||iy+4<p->min_y||iy-4>p->max_y)continue;
        if(o&&o->type==G_SLOPE){if(o->x<p->x||o->y<p->y||o->x+o->w>p->x+p->w||o->y+o->h>p->y+p->h)continue;}
        else if(x+4<p->x||x-4>p->x+p->w||y+4<p->y||y-4>p->y+p->h)continue;
        return i;
    }return -1;
}
void golf_begin(GolfGame *g,const GolfHole *h) {
    int i;GolfLive *r;
    memset(g,0,sizeof(*g));g->hole=h;g->x=h->start_x;g->y=h->start_y;
    g->shot_x=g->x;g->shot_y=g->y;g->bumper_max=8;g->warp=g->hazard=-1;
    for(i=0;i<h->count;i++){
        r=g->live+i;r->object=h->objects[i];r->direction=1;r->visible=1;r->carrier=-1;
        if(r->object.type==G_WINDMILL)r->progress=(r->object.w-r->object.length)/2;
        if(((r->object.type==G_FLOATER||r->object.type==G_WINDMILL)&&r->object.speed)||r->object.changing)g->animated=1;
    }
    for(i=0;i<h->count;i++){
        r=g->live+i;
        if(raised(&r->object))r->carrier=carrier(g,&r->object,r->object.x,r->object.y);
        r->level=strut(r->object.type)||r->carrier>=0;
    }
}
static void checkpoint(GolfGame *g){
    GolfSnapshot *s=&g->before;
    s->x=g->x;s->y=g->y;s->vx=g->vx;s->vy=g->vy;
    s->bumper_max=g->bumper_max;s->warp_speed=g->warp_speed;s->ticks=g->ticks;
    s->state=g->state;s->strokes=g->strokes;s->penalties=g->penalties;
    s->warp=g->warp;s->warp_ticks=g->warp_ticks;s->warps=g->warps;
    memcpy(s->live,g->live,(unsigned long)g->hole->count*sizeof(*g->live));g->can_undo=1;
}
static void restore(GolfGame *g){
    const GolfSnapshot *s=&g->before;
    g->x=g->shot_x=s->x;g->y=g->shot_y=s->y;g->vx=s->vx;g->vy=s->vy;
    g->bumper_max=s->bumper_max;g->warp_speed=s->warp_speed;g->ticks=s->ticks;
    g->state=s->state;g->strokes=s->strokes;g->penalties=s->penalties;
    g->warp=s->warp;g->warp_ticks=s->warp_ticks;g->warps=s->warps;
    memcpy(g->live,s->live,(unsigned long)g->hole->count*sizeof(*g->live));
    g->hazard=-1;g->hazard_dx=g->hazard_dy=0;
    /* Revision is a rendering invalidation token, never a rewindable clock. */
    g->revision++;
}
int golf_undo(GolfGame *g){
    if(!g->can_undo)return 0;
    restore(g);g->event=G_UNDONE;return 1;
}
int golf_shoot(GolfGame *g,double dx,double dy,double strength) {
    int i;double len=magnitude(dx,dy);
    if(g->state!=G_READY||len<0.001||strength<=0||strength>8.375)return 0;
    checkpoint(g);
    g->shot_x=g->x;g->shot_y=g->y;g->vx=dx/len*strength;g->vy=dy/len*strength;
    for(i=0;i<g->hole->count;i++)g->live[i].runs=0;
    g->bumper_max=8;g->warps=0;g->state=G_ROLLING;g->strokes++;g->event=G_HIT;return 1;
}
static void penalty(GolfGame *g,int event) {
    int strokes=g->strokes,penalties=g->penalties;
    if(event==G_SPLASH){
        g->hazard_dx=g->vx;g->hazard_dy=g->vy;g->vx=g->vy=0;
        g->strokes++;g->penalties++;g->state=G_HAZARD;g->event=event;return;
    }
    if(g->can_undo)restore(g);
    g->x=g->shot_x;g->y=g->shot_y;g->vx=g->vy=0;
    g->strokes=strokes;g->penalties=penalties;g->state=limited(g)?G_LIMIT:G_READY;g->event=event;
}
/* Exact closest-point distance for a circle outside an axis-aligned ellipse.
 * Using the lower bisection bound is conservative: a drop must clear the
 * entire ball, not just its centre. Most candidates fail the cheap bounds. */
static int water_overlap(const GolfObject *o,double x,double y){
    double a=o->w/2,b=o->h/2,dx=fabs(x-o->x),dy=fabs(y-o->y),lo=0,hi,mid,qx,qy;int i;
    if(dx>a+RADIUS+.01||dy>b+RADIUS+.01)return 0;
    if(square(dx/a)+square(dy/b)<=1)return 1;
    hi=a*dx+b*dy;
    for(i=0;i<24;i++){
        mid=(lo+hi)/2;qx=a*dx/(mid+a*a);qy=b*dy/(mid+b*b);
        if(qx*qx+qy*qy>1)lo=mid;else hi=mid;
    }
    qx=a*a*dx/(lo+a*a);qy=b*b*dy/(lo+b*b);
    return square(dx-qx)+square(dy-qy)<=square(RADIUS+.01);
}
static int safe_drop(const GolfGame *g,double x,double y){
    int i;double margin=g->hole->borders?RADIUS:0;
    if(x<margin||x>400-margin||y<margin||y>400-margin)return 0;
    for(i=0;i<g->hole->count;i++)if(g->live[i].visible&&g->live[i].object.type==G_WATER&&water_overlap(&g->live[i].object,x,y))return 0;
    return 1;
}
int golf_recover(GolfGame *g,int choice){
    double x,y,dx,dy,len;int i,j,found=0;
    static const double directions[8][2]={{1,0},{0,1},{-1,0},{0,-1},{.7071067811865475,.7071067811865475},{-.7071067811865475,.7071067811865475},{-.7071067811865475,-.7071067811865475},{.7071067811865475,-.7071067811865475}};
    if(g->state!=G_HAZARD||(choice!=G_DROP&&choice!=G_REHIT))return 0;
    x=g->shot_x;y=g->shot_y;
    if(choice==G_DROP){
        dx=g->hazard_dx;dy=g->hazard_dy;len=magnitude(dx,dy);
        if(len<1e-9){dx=g->x-g->shot_x;dy=g->y-g->shot_y;len=magnitude(dx,dy);}
        if(len>=1e-9){
            dx/=len;dy/=len;
            for(i=0;i<=190;i++){
                x=g->x-dx*(3*i);y=g->y-dy*(3*i);
                if(safe_drop(g,x,y)){found=1;break;}
            }
        }else{
            /* A blinking puddle can appear under a stationary ball. Upstream
             * normalizes zero here; use a finite, deterministic radial search. */
            for(i=1;i<=190&&!found;i++)for(j=0;j<8;j++){
                x=g->x+directions[j][0]*(3*i);y=g->y+directions[j][1]*(3*i);
                if(safe_drop(g,x,y)){found=1;break;}
            }
        }
        if(!found)return 0;
    }
    g->x=x;g->y=y;g->vx=g->vy=0;g->hazard=-1;g->hazard_dx=g->hazard_dy=0;
    g->state=limited(g)?G_LIMIT:G_READY;g->event=G_RECOVERED;g->revision++;return 1;
}
static void edge(GolfObject *o,double ax,double ay,double bx,double by){
    memset(o,0,sizeof(*o));o->type=G_WALL;o->ax=ax;o->ay=ay;o->bx=bx;o->by=by;
    if(ay==by){o->length=fabs(bx-ax);o->ny=bx>=ax?1:-1;}
    else{o->length=fabs(by-ay);o->nx=by>=ay?-1:1;}
    bounds(o);
}
int golf_edges(const GolfLive *r,GolfObject *e){
    const GolfObject *o=&r->object;double x=o->x,y=o->y,w=o->w,h=o->h;int n=0;
    if(o->walls&G_TOP)edge(e+n++,x,y,x+w,y);
    if(o->walls&G_LEFT)edge(e+n++,x,y,x,y+h);
    if(o->walls&G_RIGHT)edge(e+n++,x+w,y,x+w,y+h);
    if(o->walls&G_BOTTOM)edge(e+n++,x,y+h,x+w,y+h);
    if(o->type==G_WINDMILL){
        y+=o->bottom?h:0;
        edge(e+n++,x,y,x+w/4,y);edge(e+n++,x+w-w/4,y,x+w,y);
        y+=o->bottom?4:-4;edge(e+n++,x+r->progress,y,x+r->progress+o->length,y);
    }return n;
}
/* Called every other 11 ms step, matching QGraphicsScene::advance pacing. */
static void animate(GolfGame *g){
    int i,j,bc=carrier(g,NULL,g->x,g->y);GolfLive *r,*child;GolfObject *o;
    for(i=0;i<g->hole->count;i++){
        r=g->live+i;o=&r->object;r->dx=r->dy=0;
        if(o->changing){
            int period=(18*(10+o->interval))/10,visible;
            if(period<1)period=1;r->frame=(r->frame+1)%(2*period);visible=r->frame<period;
            if(r->visible!=visible){r->visible=visible;g->revision++;}
        }
        if(o->type==G_WINDMILL&&o->speed){
            double old=r->progress;r->progress+=r->direction*o->speed/3.0;
            if(r->progress<0){r->progress=0;r->direction=1;}
            if(r->progress+o->length>o->w){r->progress=o->w-o->length;r->direction=-1;}
            r->dx=r->progress-old;g->revision++;
        }
        if(o->type!=G_FLOATER||!o->speed||o->length<0.001)continue;
        /* Recompute supports before translating so newly overlapping objects
         * can join a platform; this also avoids stacking struts on struts. */
        for(j=0;j<g->hole->count;j++){
            child=g->live+j;if(raised(&child->object))child->carrier=carrier(g,&child->object,child->object.x,child->object.y);
        }
        r->progress+=r->direction*o->speed/3.5;
        /* Modulo/reflection handles short paths and high speeds without a loop. */
        if(r->progress<0||r->progress>o->length){
            r->progress=fmod(r->progress,2*o->length);if(r->progress<0)r->progress+=2*o->length;
            if(r->progress>o->length){r->progress=2*o->length-r->progress;r->direction=-r->direction;}

            /* Direction follows the endpoint hit, including multi-bounce steps. */
            r->direction=r->progress<=0?1:r->progress>=o->length?-1:r->direction;
        }
        r->dx=o->ax+(o->bx-o->ax)*r->progress/o->length-o->x;
        r->dy=o->ay+(o->by-o->ay)*r->progress/o->length-o->y;
        o->x+=r->dx;o->y+=r->dy;bounds(o);g->revision++;
        for(j=0;j<g->hole->count;j++){
            child=g->live+j;if(child->carrier!=i)continue;
            child->object.x+=r->dx;child->object.y+=r->dy;bounds(&child->object);
        }
        if(bc==i&&g->state!=G_WARPING){g->x+=r->dx;g->y+=r->dy;}
    }
    for(i=0;i<g->hole->count;i++){
        r=g->live+i;if(raised(&r->object))r->carrier=carrier(g,&r->object,r->object.x,r->object.y);
        r->level=strut(r->object.type)||r->carrier>=0;
    }
}
/* Resolve the earliest swept circle/segment contact, including rounded ends.
 * This prevents a fast ball crossing thin or diagonal walls between ticks. */
static void endpoint(double x,double y,double dx,double dy,double px,double py,
                     double *best,double *nx,double *ny) {
    double a=dx*dx+dy*dy,b=(x-px)*dx+(y-py)*dy,c=square(x-px)+square(y-py)-RADIUS*RADIUS;
    double disc,t,xx,yy,len;
    if(a<1e-12)return;
    disc=b*b-a*c;if(disc<0)return;
    t=(-b-sqrt(disc))/a;if(t<0||t>*best)return;
    xx=x+t*dx-px;yy=y+t*dy-py;len=magnitude(xx,yy);
    if(len<1e-9)return;
    *best=t;*nx=xx/len;*ny=yy/len;
}
static void segment(const GolfObject *o,double x,double y,double dx,double dy,
                    double *best,double *nx,double *ny) {
    double d=(x-o->ax)*o->nx+(y-o->ay)*o->ny,dv=dx*o->nx+dy*o->ny;
    double t,px,py,along,side;
    int sign;
    if(o->length<0.001)return;
    for(sign=-1;sign<=1;sign+=2){
        side=sign;if(dv*side>=-1e-10)continue;
        t=(side*RADIUS-d)/dv;
        if(t<0||t>*best)continue;
        px=x+t*dx;py=y+t*dy;
        along=((px-o->ax)*(o->bx-o->ax)+(py-o->ay)*(o->by-o->ay))/o->length;
        if(along>=0&&along<=o->length){*best=t;*nx=side*o->nx;*ny=side*o->ny;}
    }
    endpoint(x,y,dx,dy,o->ax,o->ay,best,nx,ny);
    endpoint(x,y,dx,dy,o->bx,o->by,best,nx,ny);
}
/* Moving edges are swept in relative coordinates and transfer their velocity.
 * The broad phase remains integer-only for stationary, distant geometry. */
static void contact(const GolfObject *o,GolfGame *g,double remain,double wx,double wy,
                    double *t,double *nx,double *ny,double *hitx,double *hity){
    double old=*t;int x=(int)g->x,y=(int)g->y,ex=(int)(g->x+(g->vx-wx)*remain),ey=(int)(g->y+(g->vy-wy)*remain);
    if((x<ex?x:ex)-5>o->max_x||(x>ex?x:ex)+5<o->min_x||(y<ey?y:ey)-5>o->max_y||(y>ey?y:ey)+5<o->min_y)return;
    segment(o,g->x,g->y,(g->vx-wx)*remain,(g->vy-wy)*remain,t,nx,ny);
    if(*t<old){*hitx=wx;*hity=wy;}
}
static void move(GolfGame *g) {
    double remain=1,t,nx,ny,dot,hitx,hity;int pass,i,j,n,x,y,ex,ey,minx,maxx,miny,maxy,platform=carrier(g,NULL,g->x,g->y),level=platform>=0;
    GolfObject edges[7];const GolfObject *o;GolfLive *r;
    for(pass=0;pass<4&&remain>0.00001;pass++){
        t=1.000001;nx=ny=hitx=hity=0;
        /* Calculate the sweep once. In RVM a binary64 operation is a service
         * call, so doing this again for every distant wall is expensive. */
        x=(int)g->x;y=(int)g->y;ex=(int)(g->x+g->vx*remain);ey=(int)(g->y+g->vy*remain);
        minx=(x<ex?x:ex)-5;maxx=(x>ex?x:ex)+5;miny=(y<ey?y:ey)-5;maxy=(y>ey?y:ey)+5;
        for(i=0;i<g->hole->count;i++){
            r=g->live+i;o=&r->object;
            if(o->type==G_WALL){if(!level&&!(maxx<o->min_x||minx>o->max_x||maxy<o->min_y||miny>o->max_y))contact(o,g,remain,0,0,&t,&nx,&ny,&hitx,&hity);}
            else if(golf_rectangle(o->type)){
                /* Avoid constructing distant rectangles and their gate segments. */
                int ix=(int)g->x,iy=(int)g->y;
                if(ix<o->min_x-25||ix>o->max_x+25||iy<o->min_y-25||iy>o->max_y+25)continue;
                n=golf_edges(r,edges);
                for(j=0;j<n;j++){
                    double wx=0,wy=0;
                    /* dx/dy describe this tick's already-applied translation. */
                    if(o->type==G_FLOATER&&i!=platform){wx=r->dx;wy=r->dy;}
                    if(o->type==G_WINDMILL&&j==n-1)wx=r->dx;
                    if(wx||wy){edges[j].ax-=wx*remain;edges[j].bx-=wx*remain;edges[j].ay-=wy*remain;edges[j].by-=wy*remain;bounds(edges+j);}
                    contact(edges+j,g,remain,wx,wy,&t,&nx,&ny,&hitx,&hity);
                }
            }
        }
        if(g->hole->borders){
            double a;
            if(g->vx<0){a=(4-g->x)/(g->vx*remain);if(a>=0&&a<=t){t=a;nx=1;ny=0;hitx=hity=0;}}
            if(g->vx>0){a=(396-g->x)/(g->vx*remain);if(a>=0&&a<=t){t=a;nx=-1;ny=0;hitx=hity=0;}}
            if(g->vy<0){a=(4-g->y)/(g->vy*remain);if(a>=0&&a<=t){t=a;nx=0;ny=1;hitx=hity=0;}}
            if(g->vy>0){a=(396-g->y)/(g->vy*remain);if(a>=0&&a<=t){t=a;nx=0;ny=-1;hitx=hity=0;}}
        }
        if(t>1){g->x+=g->vx*remain;g->y+=g->vy*remain;return;}
        g->x+=g->vx*remain*t+nx*0.001;g->y+=g->vy*remain*t+ny*0.001;
        dot=(g->vx-hitx)*nx+(g->vy-hity)*ny;g->vx-=2*dot*nx;g->vy-=2*dot*ny;
        g->state=G_ROLLING;g->event=G_BOUNCE;remain*=1-t;
    }
}
void golf_step(GolfGame *g) {
    const GolfObject *o,*slope=NULL;GolfLive *r;
    double friction=1,speed,amount,dx,dy,len,old,factor;int i,ix,iy,platform,level;
    if(golf_finished(g)||g->state==G_HAZARD)return;
    g->event=G_NONE;
    ++g->ticks;
    if(g->animated&&(g->ticks&1))animate(g);
    else for(i=0;i<g->hole->count;i++){g->live[i].dx=g->live[i].dy=0;}
    if(g->state==G_WARPING){
        if(--g->warp_ticks>0)return;
        o=&g->live[g->warp].object;
        g->x=o->exit_x+10*o->nx;g->y=o->exit_y+10*o->ny;
        g->vx=o->nx*g->warp_speed;g->vy=o->ny*g->warp_speed;
        g->state=G_ROLLING;g->warps++;g->live[g->warp].runs++;g->revision++;return;
    }
    /* Moving gates can start a stationary ball. Static courses stay asleep. */
    if(g->state==G_READY&&!g->animated)return;
    move(g);
    if(g->x < 0||g->x > 400||g->y < 0||g->y > 400){penalty(g,G_OUT);return;}
    ix=(int)g->x;iy=(int)g->y;platform=carrier(g,NULL,g->x,g->y);level=platform>=0;
    if(platform>=0&&g->live[platform].object.type==G_BRIDGE)friction=.63;
    for(i=g->hole->count-1;i>=0;i--){
        r=g->live+i;o=&r->object;
        if(!r->visible||r->level<level)continue;
        if(o->bounded){int pad=o->type==G_BUMPER?4:0;if(ix<o->min_x-pad||ix>o->max_x+pad||iy<o->min_y-pad||iy>o->max_y+pad)continue;}
        if(o->type==G_BUMPER){
            dx=g->x-o->x;dy=g->y-o->y;len=magnitude(dx,dy);
            if(len>0.001&&len<o->w/2+RADIUS&&(g->vx*dx+g->vy*dy)<0){
                speed=magnitude(g->vx,g->vy)*.9+1.8;if(speed>g->bumper_max)speed=g->bumper_max;
                if(g->bumper_max>.4)g->bumper_max-=.35;
                g->vx=dx/len*speed;g->vy=dy/len*speed;g->state=G_ROLLING;g->event=G_BUMP;
            }continue;
        }
        if(o->type==G_WALL||golf_rectangle(o->type)||!golf_contains(o,g->x,g->y))continue;
        if(o->type==G_WATER){g->hazard=i;penalty(g,G_SPLASH);return;}
        if(o->type==G_CUP&&magnitude(g->vx,g->vy)<=3.75){
            g->x=o->x;g->y=o->y;g->vx=g->vy=0;g->state=G_HOLED;g->event=G_SUNK;return;
        }
        if(o->type==G_BLACKHOLE&&r->runs<=10&&(speed=magnitude(g->vx,g->vy))<=3.75){
            g->warp_speed=o->min_speed+speed/3.75*(o->max_speed-o->min_speed);
            amount=magnitude(o->exit_x-o->x,o->exit_y-o->y)*2.5-g->warp_speed*35+500;
            g->warp_ticks=(int)(amount/11);if(g->warp_ticks<1)g->warp_ticks=1;
            g->vx=g->vy=0;g->state=G_WARPING;g->warp=i;g->event=G_WARP;g->revision++;return;
        }
        if(o->type==G_SAND)friction=7;
        if(o->type==G_SLOPE&&!slope)slope=o;
    }
    speed=magnitude(g->vx,g->vy);old=speed;
    if((g->ticks&1)&&speed>0){
        amount=0.027*friction;if(speed<=amount){g->vx=g->vy=0;speed=0;}
        else{factor=(speed-amount)/speed;g->vx*=factor;g->vy*=factor;speed-=amount;}
    }
    if(slope){
        o=slope;amount=0.013*o->grade;if(!o->reversed)amount=-amount;
        if(o->bounded&&o->gradient!=G_ELLIPTIC){if(o->ax!=0)g->vx+=o->ax;if(o->ay!=0)g->vy+=o->ay;}
        else if(o->gradient==G_HORIZONTAL)g->vx+=amount;
        else if(o->gradient==G_VERTICAL)g->vy+=amount;
        else if(o->gradient==G_ELLIPTIC){
            dx=o->x+o->w/2-g->x;dy=o->y+o->h/2-g->y;len=magnitude(dx,dy);
            if(len>0.001){amount=sin(amount*len/(o->w/2)*1.5707963267948966);g->vx+=dx/len*amount;g->vy+=dy/len*amount;}
        }else{
            len=magnitude(o->w,o->h);dx=o->h/len;dy=o->w/len;
            g->vx+=(o->gradient==G_CROSS?-dx:dx)*amount;g->vy+=dy*amount;
        }
    }
    if(slope){speed=magnitude(g->vx,g->vy);if(speed>=.06)g->state=G_ROLLING;}
    if(speed<0.06&&fabs(speed-old)<0.06){g->vx=g->vy=0;g->state=limited(g)?G_LIMIT:G_READY;}
}
