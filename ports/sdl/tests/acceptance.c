/* Compare this application, unchanged, against upstream SDL 1.2 and the guest.
 * Output is logical RGBA, not native storage: RGB565 loss and SDL's particular
 * alpha rounding remain observable without assuming a host representation.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s: %s\n", name, SDL_GetError());
        failures++;
    }
}

static void require(int condition, const char *name)
{
    if (!condition) {
        fprintf(stderr, "FATAL %s: %s\n", name, SDL_GetError());
        exit(2);
    }
}

static SDL_Surface *surface(int depth, int width, int height, int alpha)
{
    Uint32 r = 0, g = 0, b = 0, a = 0;
    SDL_Surface *s;
    if (depth == 16) { r = 0xf800UL; g = 0x07e0UL; b = 0x001fUL; }
    if (depth == 24 || depth == 32) {
        r = 0x00ff0000UL; g = 0x0000ff00UL; b = 0x000000ffUL;
        if (alpha) a = 0xff000000UL;
    }
    s = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, depth, r, g, b, a);
    require(s != NULL, "create surface");
    if (depth == 8) {
        SDL_Color colors[8];
        static const Uint8 values[8][3] = {
            {0,0,0}, {255,0,0}, {0,255,0}, {0,0,255},
            {255,255,255}, {85,170,34}, {12,34,56}, {128,64,192}
        };
        int i;
        for (i = 0; i < 8; i++) {
            colors[i].r = values[i][0]; colors[i].g = values[i][1];
            colors[i].b = values[i][2]; colors[i].unused = 0;
        }
        require(SDL_SetColors(s, colors, 0, 8) == 1, "set palette");
    }
    return s;
}

static Uint32 pixel(SDL_Surface *s, int x, int y)
{
    Uint8 *p = (Uint8 *)s->pixels + (Uint32)y * s->pitch +
               (Uint32)x * s->format->BytesPerPixel;
    Uint32 value = 0;
    int i;
    for (i = 0; i < s->format->BytesPerPixel; i++) {
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        value = (value << 8) | p[i];
#else
        value |= (Uint32)p[i] << (i * 8);
#endif
    }
    return value;
}

static void dump(const char *name, SDL_Surface *s)
{
    int x, y;
    require(SDL_LockSurface(s) == 0, "lock dump");
    printf("PIX %s %d %d\n", name, s->w, s->h);
    for (y = 0; y < s->h; y++) {
        printf("ROW");
        for (x = 0; x < s->w; x++) {
            Uint8 r, g, b, a;
            SDL_GetRGBA(pixel(s, x, y), s->format, &r, &g, &b, &a);
            printf(" %02x%02x%02x%02x", (unsigned)r, (unsigned)g,
                   (unsigned)b, (unsigned)a);
        }
        printf("\n");
    }
    SDL_UnlockSurface(s);
}

static void rectangle(const char *name, SDL_Rect *r)
{
    printf("RECT %s %d %d %u %u\n", name, (int)r->x, (int)r->y,
           (unsigned)r->w, (unsigned)r->h);
}

static void dot(SDL_Surface *s, int x, int y, int r, int g, int b, int a)
{
    SDL_Rect rect;
    rect.x = x; rect.y = y; rect.w = 1; rect.h = 1;
    require(SDL_FillRect(s, &rect, SDL_MapRGBA(s->format, r, g, b, a)) == 0,
            "fill pixel");
}

static void pattern(SDL_Surface *s)
{
    static const Uint8 colors[6][4] = {
        {255,0,0,0}, {0,255,0,64}, {0,0,255,128},
        {255,255,255,255}, {85,170,34,192}, {128,64,192,255}
    };
    int x, y;
    for (y = 0; y < s->h; y++) for (x = 0; x < s->w; x++) {
        int i = (x + y * s->w) % 6;
        dot(s, x, y, colors[i][0], colors[i][1], colors[i][2], colors[i][3]);
    }
}

static void fill_tests(void)
{
    SDL_Surface *s = surface(32, 6, 4, 0);
    SDL_Rect clip = {1,1,3,2}, r = {-2,0,5,4}, outside = {10,10,3,3};
    require(SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 200, 10, 20)) == 0,
            "fill background");
    check(SDL_SetClipRect(s, &clip) == SDL_TRUE, "nonempty clip");
    require(SDL_FillRect(s, &r, SDL_MapRGB(s->format, 10, 200, 20)) == 0,
            "clipped fill");
    rectangle("fill-negative", &r);
    require(SDL_FillRect(s, &outside, 0) == 0, "outside fill");
    rectangle("fill-outside", &outside);
    dump("fill-clipped", s);
    clip.x = -2; clip.y = -3; clip.w = 4; clip.h = 5;
    check(SDL_SetClipRect(s, &clip) == SDL_TRUE, "negative clip");
    SDL_GetClipRect(s, &r); rectangle("clip-negative", &r);
    clip.x = 20; clip.y = 20; clip.w = 2; clip.h = 2;
    check(SDL_SetClipRect(s, &clip) == SDL_FALSE, "empty clip");
    SDL_GetClipRect(s, &r); rectangle("clip-empty", &r);
    SDL_FreeSurface(s);
}

static void conversions(void)
{
    static const int depths[4] = {8,16,24,32};
    int i, j;
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) {
        SDL_Surface *src = surface(depths[i], 3, 2, 0);
        SDL_Surface *dst = surface(depths[j], 5, 4, 0);
        SDL_Rect sr = {0,0,3,2}, dr = {1,1,0,0};
        char name[64];
        pattern(src);
        require(SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format,12,34,56)) == 0,
                "conversion background");
        require(SDL_BlitSurface(src, &sr, dst, &dr) == 0, "conversion blit");
        sprintf(name, "convert-%d-%d", depths[i], depths[j]);
        dump(name, dst); rectangle(name, &dr);
        /* Both source and destination clipping alter the copied origin. */
        sr.x = -1; sr.y = -1; sr.w = 5; sr.h = 4;
        dr.x = -1; dr.y = 0; dr.w = 0; dr.h = 0;
        require(SDL_BlitSurface(src, &sr, dst, &dr) == 0, "negative blit");
        sprintf(name, "negative-%d-%d", depths[i], depths[j]);
        dump(name, dst); rectangle(name, &dr);
        SDL_FreeSurface(dst); SDL_FreeSurface(src);
    }
}

static void padding(void)
{
    Uint8 storage[32];
    SDL_Surface *s;
    SDL_Rect r = {1,0,2,2};
    int i, y;
    for (i = 0; i < 32; i++) storage[i] = 0xd7;
    s = SDL_CreateRGBSurfaceFrom(storage, 3, 2, 24, 16,
            0xff0000UL, 0xff00UL, 0xffUL, 0);
    require(s != NULL, "external padded surface");
    require(SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 12,34,56)) == 0,
            "fill padded surface");
    require(SDL_FillRect(s, &r, SDL_MapRGB(s->format, 85,170,34)) == 0,
            "partial padded fill");
    for (y = 0; y < 2; y++) for (i = 9; i < 16; i++)
        check(storage[y * 16 + i] == 0xd7, "pitch padding preserved");
    dump("pitch-padding", s);
    SDL_FreeSurface(s);
    /* PREALLOC ownership: SDL_FreeSurface must leave this storage usable. */
    storage[0] = 0x5a; check(storage[0] == 0x5a, "external surface ownership");
}

static void transparency(void)
{
    static const int depths[4] = {8,16,24,32};
    static const int alphas[6] = {0,1,127,128,254,255};
    int i, j;
    for (i = 0; i < 4; i++) {
        SDL_Surface *src = surface(depths[i], 3, 2, 0);
        SDL_Surface *dst = surface(32, 3, 2, 0);
        char name[64];
        pattern(src);
        require(SDL_SetColorKey(src, SDL_SRCCOLORKEY,
                SDL_MapRGB(src->format,0,255,0)) == 0, "set color key");
        require(SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format,12,34,56)) == 0,
                "color key background");
        require(SDL_BlitSurface(src, NULL, dst, NULL) == 0, "color key blit");
        sprintf(name, "key-%d", depths[i]); dump(name, dst);
        require(SDL_SetColorKey(src, 0, 0) == 0, "disable color key");
        require(SDL_BlitSurface(src, NULL, dst, NULL) == 0, "key disabled blit");
        sprintf(name, "key-off-%d", depths[i]); dump(name, dst);
        SDL_FreeSurface(src); SDL_FreeSurface(dst);
    }
    for (i = 1; i < 4; i++) for (j = 0; j < 6; j++) {
        SDL_Surface *src = surface(depths[i], 3, 2, 0);
        SDL_Surface *dst = surface(depths[i], 3, 2, 0);
        char name[64];
        pattern(src);
        require(SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format,12,34,56)) == 0,
                "alpha background");
        require(SDL_SetAlpha(src, SDL_SRCALPHA, alphas[j]) == 0, "surface alpha");
        require(SDL_BlitSurface(src, NULL, dst, NULL) == 0, "surface alpha blit");
        sprintf(name, "alpha-%d-%d", depths[i], alphas[j]); dump(name, dst);
        SDL_FreeSurface(src); SDL_FreeSurface(dst);
    }
    for (i = 1; i < 4; i++) {
        SDL_Surface *src = surface(32, 3, 2, 1);
        SDL_Surface *dst = surface(depths[i], 3, 2, 0);
        char name[64];
        pattern(src);
        require(SDL_FillRect(dst, NULL, SDL_MapRGB(dst->format,12,34,56)) == 0,
                "pixel alpha background");
        require(SDL_BlitSurface(src, NULL, dst, NULL) == 0, "pixel alpha blit");
        sprintf(name, "pixel-alpha-%d", depths[i]); dump(name, dst);
        SDL_FreeSurface(src); SDL_FreeSurface(dst);
    }
}

static void overlaps(void)
{
    static const int depths[4] = {8,16,24,32};
    int i, direction;
    for (i = 0; i < 4; i++) for (direction = 0; direction < 4; direction++) {
        SDL_Surface *s = surface(depths[i], 5, 4, 0);
        SDL_Rect sr = {0,0,4,3}, dr = {0,0,0,0};
        char name[64];
        pattern(s);
        if (direction == 0) dr.x = 1;
        if (direction == 1) sr.x = 1;
        if (direction == 2) dr.y = 1;
        if (direction == 3) sr.y = 1;
        require(SDL_BlitSurface(s, &sr, s, &dr) == 0, "opaque overlap blit");
        sprintf(name, "overlap-%d-%d", depths[i], direction); dump(name, s);
        SDL_FreeSurface(s);
    }
}

static void le16(Uint8 *p, Uint16 value)
{
    p[0] = value & 255; p[1] = (value >> 8) & 255;
}

static void le32(Uint8 *p, Uint32 value)
{
    int i;
    for (i = 0; i < 4; i++) p[i] = (value >> (8 * i)) & 255;
}

static void bitmap(void)
{
    Uint8 bmp[70];
    SDL_RWops *rw;
    SDL_Surface *s;
    int i;
    for (i = 0; i < 70; i++) bmp[i] = 0;
    bmp[0] = 'B'; bmp[1] = 'M'; le32(bmp + 2, 70); le32(bmp + 10, 54);
    le32(bmp + 14, 40); le32(bmp + 18, 2); le32(bmp + 22, 2);
    le16(bmp + 26, 1); le16(bmp + 28, 24); le32(bmp + 34, 16);
    /* Bottom row blue/white; top row red/green; two padding octets per row. */
    bmp[54] = 255; bmp[57] = 255; bmp[58] = 255; bmp[59] = 255;
    bmp[64] = 255; bmp[66] = 255;
    rw = SDL_RWFromMem(bmp, 70); require(rw != NULL, "BMP memory RWops");
    s = SDL_LoadBMP_RW(rw, 1); require(s != NULL, "BMP memory decode");
    check(s->w == 2 && s->h == 2, "BMP dimensions"); dump("bmp-memory-24", s);
    SDL_FreeSurface(s);
    {
        /* More than a signed C16 int: exercise RWops lengths and row offsets. */
        Uint32 length = 54UL + 128UL * 128UL * 3UL, offset;
        Uint8 *large = (Uint8 *)SDL_calloc(length, sizeof(Uint8));
        SDL_Surface *sample;
        SDL_Rect sr = {125,126,3,2};
        int x, y;
        require(large != NULL, "large BMP storage");
        large[0]='B'; large[1]='M'; le32(large+2,length); le32(large+10,54);
        le32(large+14,40); le32(large+18,128); le32(large+22,128);
        le16(large+26,1); le16(large+28,24); le32(large+34,length-54);
        for(y=0;y<128;y++)for(x=0;x<128;x++){
            offset=54UL+((Uint32)y*128UL+x)*3UL;
            large[offset]=(x*3+y)&255; large[offset+1]=(x+y*3)&255;
            large[offset+2]=(x*7+y*5)&255;
        }
        rw=SDL_RWFromMem(large,length); require(rw!=NULL,"large BMP RWops");
        s=SDL_LoadBMP_RW(rw,1); require(s!=NULL,"large BMP decode");
        check(s->w==128&&s->h==128,"large BMP dimensions");
        sample=surface(32,3,2,0);
        require(SDL_BlitSurface(s,&sr,sample,NULL)==0,"large BMP sample");
        dump("bmp-memory-large",sample);
        SDL_FreeSurface(sample);
        {
            SDL_Surface *reopened;
            Sint32 file_length;
            const char *path="sdl-roundtrip.bmp";
            require(SDL_SaveBMP(s,path)==0,"large BMP stdio save");
            rw=SDL_RWFromFile(path,"rb"); require(rw!=NULL,"large BMP stdio reopen");
            file_length=SDL_RWseek(rw,0,RW_SEEK_END);
            check(file_length==(Sint32)length,"large BMP saved file length");
            printf("RWOPS bmp-file-length %ld\n",(long)file_length);
            require(SDL_RWclose(rw)==0,"large BMP stdio close");
            reopened=SDL_LoadBMP(path); require(reopened!=NULL,"large BMP stdio decode");
            check(reopened->w==128&&reopened->h==128,"reopened BMP dimensions");
            sample=surface(32,3,2,0);
            require(SDL_BlitSurface(reopened,&sr,sample,NULL)==0,"reopened BMP sample");
            dump("bmp-file-roundtrip-large",sample);
            SDL_FreeSurface(sample); SDL_FreeSurface(reopened);
            check(remove(path)==0,"remove BMP test file");
        }
        SDL_FreeSurface(s); SDL_free(large);
    }
}

static void rwops(void)
{
    Uint8 bytes[96], out[8];
    Uint64 first=((Uint64)0x01234567UL<<32)|0x89abcdefUL;
    Uint64 second=((Uint64)0xfedcba98UL<<32)|0x76543210UL;
    SDL_RWops *rw;
    Sint32 count, position;
    int i;
    for(i=0;i<96;i++)bytes[i]=0xa5;
    rw=SDL_RWFromMem(bytes,96); require(rw!=NULL,"endian memory RWops");
    check(SDL_WriteLE16(rw,0x1234)==1,"write LE16");
    check(SDL_WriteBE16(rw,0xabcd)==1,"write BE16");
    check(SDL_WriteLE32(rw,0x01234567UL)==1,"write LE32");
    check(SDL_WriteBE32(rw,0x89abcdefUL)==1,"write BE32");
    check(SDL_WriteLE64(rw,first)==1,"write LE64");
    check(SDL_WriteBE64(rw,second)==1,"write BE64");
    check(SDL_RWtell(rw)==28,"endian write position");
    printf("RWOPS endian-octets");
    for(i=0;i<28;i++)printf(" %02x",(unsigned)bytes[i]);
    printf("\n");
    for(i=28;i<96;i++)check(bytes[i]==0xa5,"endian write bounds");
    require(SDL_RWseek(rw,0,RW_SEEK_SET)==0,"endian rewind");
    check(SDL_ReadLE16(rw)==0x1234,"read LE16");
    check(SDL_ReadBE16(rw)==0xabcd,"read BE16");
    check(SDL_ReadLE32(rw)==0x01234567UL,"read LE32");
    check(SDL_ReadBE32(rw)==0x89abcdefUL,"read BE32");
    check(SDL_ReadLE64(rw)==first,"read LE64");
    check(SDL_ReadBE64(rw)==second,"read BE64");
    check(SDL_RWtell(rw)==28,"endian read position");
    check(SDL_RWread(rw,out,0,1)==0,"zero size memory read");
    check(SDL_RWread(rw,out,1,0)==0,"zero count memory read");
    check(SDL_RWwrite(rw,out,1,0)==0,"zero count memory write");
    check(SDL_RWtell(rw)==28,"zero operations preserve position");
    check(SDL_RWseek(rw,4,RW_SEEK_SET)==4,"memory seek set");
    check(SDL_RWseek(rw,8,RW_SEEK_CUR)==12,"memory seek relative");
    check(SDL_RWseek(rw,-8,RW_SEEK_END)==88,"memory seek end");
    check(SDL_RWseek(rw,9999,RW_SEEK_SET)==96,"memory seek upper clamp");
    check(SDL_RWread(rw,out,1,1)==0,"memory EOF read");
    check(SDL_RWwrite(rw,out,1,1)==0,"memory EOF write");
    check(SDL_RWseek(rw,-9999,RW_SEEK_CUR)==0,"memory relative lower clamp");
    check(SDL_RWseek(rw,-9999,RW_SEEK_END)==0,"memory end lower clamp");
    check(SDL_RWseek(rw,93,RW_SEEK_SET)==93,"partial object seek");
    for(i=0;i<8;i++)out[i]=0x5a;
    count=SDL_RWread(rw,out,2,2); position=SDL_RWtell(rw);
    check(count==1&&position==96,"partial read consumes final incomplete object");
    check(out[0]==0xa5&&out[1]==0xa5&&out[2]==0xa5&&out[3]==0x5a,
          "partial read exact bounds");
    printf("RWOPS partial-read %ld %ld\n",(long)count,(long)position);
    check(SDL_RWseek(rw,93,RW_SEEK_SET)==93,"partial write seek");
    count=SDL_RWwrite(rw,out,2,2); position=SDL_RWtell(rw);
    check(count==1&&position==95,"partial write copies complete objects only");
    printf("RWOPS partial-write %ld %ld\n",(long)count,(long)position);
    SDL_ClearError();
    check(SDL_RWseek(rw,0,99)==-1&&SDL_GetError()[0]!=0,"invalid memory seek origin");
    check(SDL_RWtell(rw)==95,"invalid origin preserves position");
    require(SDL_RWclose(rw)==0,"close memory RWops");
    rw=SDL_RWFromConstMem(bytes,96); require(rw!=NULL,"const memory RWops");
    SDL_ClearError();
    check(SDL_RWwrite(rw,out,1,1)==-1&&SDL_GetError()[0]!=0,"const memory rejects writes");
    check(SDL_RWtell(rw)==0,"const write preserves position");
    require(SDL_RWclose(rw)==0,"close const RWops");
    printf("RWOPS endian seek EOF zero-count bounds passed\n");
}

static void rwops_profile(void)
{
    Uint8 bytes[32], out[32];
    SDL_RWops *rw;
    int i;
    require(SDL_Init(0)==0,"RWops profile init");
    for(i=0;i<32;i++)bytes[i]=0xa5;
    for(i=0;i<32;i++)out[i]=0x5a;
    rw=SDL_RWFromMem(bytes,32); require(rw!=NULL,"RWops profile memory");
    require(SDL_RWseek(rw,4,RW_SEEK_SET)==4,"RWops profile start");
    check(SDL_RWread(rw,out,-1,1)==0,"negative read size");
    check(SDL_RWread(rw,out,1,-1)==0,"negative read count");
    check(SDL_RWwrite(rw,out,-1,1)==0,"negative write size");
    check(SDL_RWwrite(rw,out,1,-1)==0,"negative write count");
    check(SDL_RWwrite(rw,out,0,1)==0,"zero size write");
    check(SDL_RWread(rw,out,0x40000000L,4)==0,"overflowing read product");
    check(SDL_RWwrite(rw,out,0x40000000L,4)==0,"overflowing write product");
    check(SDL_RWtell(rw)==4,"invalid counts preserve position");
    for(i=0;i<32;i++)check(bytes[i]==0xa5,"invalid writes preserve storage");
    for(i=0;i<32;i++)check(out[i]==0x5a,"invalid reads preserve destination");
    check(SDL_RWseek(rw,0x7fffffffL,RW_SEEK_SET)==32,"extreme seek set clamp");
    check(SDL_RWseek(rw,0x7fffffffL,RW_SEEK_CUR)==32,"relative seek overflow clamp");
    check(SDL_RWseek(rw,-0x7fffffffL-1,RW_SEEK_CUR)==0,"relative seek underflow clamp");
    check(SDL_RWseek(rw,0x7fffffffL,RW_SEEK_END)==32,"end seek overflow clamp");
    check(SDL_RWseek(rw,-0x7fffffffL-1,RW_SEEK_END)==0,"end seek underflow clamp");
    require(SDL_RWclose(rw)==0,"close profile memory");
    SDL_ClearError(); rw=SDL_RWFromMem(NULL,32);
    check(rw==NULL&&SDL_GetError()[0]!=0,"reject null memory RWops");
    if(rw)SDL_RWclose(rw);
    SDL_Quit();
    printf("PROFILE RWops negative/overflow counts and seek clamps passed\n");
}

static int SDLCALL reject(const SDL_Event *event)
{
    (void)event; return 0;
}

static void events(void)
{
    SDL_Event e, got[3];
    int n;
    require(SDL_Init(SDL_INIT_VIDEO) == 0, "video/event init");
    while (SDL_PollEvent(&e)) {}
    memset(&e, 0, sizeof(e)); e.type = SDL_USEREVENT; e.user.code = 17;
    require(SDL_PushEvent(&e) == 0, "push first user event");
    e.user.code = 23; require(SDL_PushEvent(&e) == 0, "push second user event");
    n = SDL_PeepEvents(got, 3, SDL_PEEKEVENT, SDL_EVENTMASK(SDL_USEREVENT));
    check(n == 2 && got[0].user.code == 17 && got[1].user.code == 23,
          "peek preserves ordered events");
    check(SDL_PollEvent(&e) == 1 && e.type == SDL_USEREVENT && e.user.code == 17,
          "poll first user event");
    check(SDL_PollEvent(&e) == 1 && e.type == SDL_USEREVENT && e.user.code == 23,
          "poll second user event");
    check(SDL_PollEvent(&e) == 0, "queue drained");
    e.type = SDL_USEREVENT; e.user.code = 31;
    require(SDL_PushEvent(&e) == 0, "push before filter");
    SDL_SetEventFilter(reject);
    check(SDL_GetEventFilter() == reject, "filter identity");
    check(SDL_PollEvent(&e) == 0, "filter change flushes pending queue");
    e.type = SDL_USEREVENT; e.user.code = 41;
    require(SDL_PushEvent(&e) == 0, "push bypasses filter");
    check(SDL_PollEvent(&e) == 1 && e.user.code == 41, "pushed events bypass filter");
    SDL_SetEventFilter(NULL);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);
    check(SDL_EventState(SDL_USEREVENT, SDL_QUERY) == SDL_IGNORE, "event disabled");
    e.type = SDL_USEREVENT; e.user.code = 43;
    require(SDL_PushEvent(&e) == 0, "push ignored user event");
    check(SDL_PollEvent(&e) == 1 && e.user.code == 43, "push bypasses event state");
    SDL_EventState(SDL_USEREVENT, SDL_ENABLE);
    check(SDL_EventState(SDL_USEREVENT, SDL_QUERY) == SDL_ENABLE, "event enabled");
    e.type = SDL_USEREVENT; e.user.code = 47;
    require(SDL_PushEvent(&e) == 0, "push before state change");
    SDL_EventState(SDL_USEREVENT, SDL_ENABLE);
    check(SDL_PollEvent(&e) == 0, "state change flushes pending queue");
    SDL_Quit();
    printf("EVENT queue-order peek poll filter state passed\n");
}

static int SDLCALL thread_function(void *data)
{
    (void)data; return 0;
}

static Uint32 SDLCALL timer_function(Uint32 interval, void *data)
{
    (void)interval; (void)data; return 0;
}

static void unsupported(void)
{
    SDL_Thread *thread;
    SDL_TimerID timer;
    SDL_ClearError();
    thread = SDL_CreateThread(thread_function, NULL);
    check(thread == NULL && SDL_GetError()[0] != 0, "threads fail explicitly");
    if (thread) SDL_WaitThread(thread, NULL);
    SDL_ClearError();
    if (SDL_Init(SDL_INIT_TIMER) == 0) {
        timer = SDL_AddTimer(10, timer_function, NULL);
        check(timer == NULL && SDL_GetError()[0] != 0, "timer callbacks fail explicitly");
        if (timer) SDL_RemoveTimer(timer);
    } else check(SDL_GetError()[0] != 0, "timer init fails explicitly");
    SDL_Quit();
    SDL_ClearError();
    check(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTTHREAD)<0&&SDL_GetError()[0]!=0,
          "event thread fails explicitly");
    SDL_Quit();
    printf("PROFILE unsupported threads event-thread timer-callbacks checked\n");
}

static void timing(void)
{
    Uint32 before, after, elapsed;
    require(SDL_Init(0)==0,"clock init");
    before=SDL_GetTicks(); SDL_Delay(30); after=SDL_GetTicks(); elapsed=after-before;
    check(elapsed>=20UL&&elapsed<10000UL,"monotonic ticks and real delay");
    before=after; after=SDL_GetTicks();
    check(after-before<10000UL,"clock does not move backwards");
    SDL_Quit(); printf("TIMING ticks delay passed\n");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "--surface-only";
    if (!strcmp(mode, "--surface-only")) {
        require(SDL_Init(0) == 0, "base init");
        fill_tests(); conversions(); padding(); transparency(); overlaps(); bitmap(); rwops();
        SDL_Quit();
    } else if (!strcmp(mode, "--events")) events();
    else if (!strcmp(mode, "--unsupported")) unsupported();
    else if (!strcmp(mode, "--timing")) timing();
    else if (!strcmp(mode, "--rwops-profile")) rwops_profile();
    else { fprintf(stderr, "usage: sdl-acceptance [--surface-only|--events|--unsupported|--timing|--rwops-profile]\n"); return 2; }
    if (failures) { fprintf(stderr, "SDL_ACCEPTANCE failures=%d\n", failures); return 1; }
    printf("SDL_ACCEPTANCE %s passed\n", mode);
    return 0;
}
