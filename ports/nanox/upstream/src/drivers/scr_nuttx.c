#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <nuttx/video/fb.h>
#include "device.h"
#include "genfont.h"
#include "genmem.h"
#include "fb.h"

#ifndef NUTTX_FB_PATH
#  define NUTTX_FB_PATH "/dev/fb0"
#endif

static int fb = -1;

static PSD fb_open(PSD psd);
static void fb_close(PSD psd);
#ifdef FBIO_UPDATE
static void fb_update(PSD psd, MWCOORD x, MWCOORD y, MWCOORD width, MWCOORD height);
#endif

SCREENDEVICE scrdev = {
  0, 0, 0, 0, 0, 0, 0, NULL, 0, NULL, 0, 0, 0, 0, 0, 0,
  gen_fonts,
  fb_open,
  fb_close,
  NULL,
  gen_getscreeninfo,
  gen_allocatememgc,
  gen_mapmemgc,
  gen_freememgc,
  gen_setportrait,
#ifdef FBIO_UPDATE
  fb_update,
#else
  NULL,
#endif
  NULL
};

static int screen_pixtype(uint8_t nuttx_pixtype)
{
  switch(nuttx_pixtype)
    {
      case FB_FMT_Y1:
      case FB_FMT_Y2:
      case FB_FMT_Y4:
      case FB_FMT_Y8:
      case FB_FMT_Y16:
        return MWPF_PIXELVAL;
      case FB_FMT_RGB4:
        return MWPF_PALETTE;
      case FB_FMT_RGB8:
        return MWPF_PALETTE;
      case FB_FMT_RGB8_222:
        return -1;
      case FB_FMT_RGB8_332:
        return MWPF_TRUECOLOR332;
      case FB_FMT_RGB12_444:
        return -1;
      case FB_FMT_RGB16_555:
        return MWPF_TRUECOLOR555;
      case FB_FMT_RGB16_565:
        return MWPF_TRUECOLOR565;
      case FB_FMT_RGB24:
        return MWPF_TRUECOLORRGB;
      case FB_FMT_RGB32:
        return MWPF_TRUECOLORARGB;
      default:
        return -1;
    }
}

static PSD fb_open(PSD psd)
{
  struct fb_videoinfo_s vinfo;
  struct fb_planeinfo_s pinfo;
  PSUBDRIVER subdriver;

  fb = open(NUTTX_FB_PATH, O_RDWR);
  if (fb < 0)
    {
      EPRINTF("Error: cannot open %s. Check CONFIG_VIDEO_FB\n",
              NUTTX_FB_PATH);
      return NULL;
    }

  if (ioctl(fb, FBIOGET_VIDEOINFO, &vinfo) < 0 ||
      ioctl(fb, FBIOGET_PLANEINFO, &pinfo) < 0)
    {
      EPRINTF("Error: cannot read framebuffer info\n");
      goto fail;
    }

  if (vinfo.xres == 0 || vinfo.yres == 0)
    {
      EPRINTF("Error: zero resolution\n");
      goto fail;
    }

  psd->xres = psd->xvirtres = vinfo.xres;
  psd->yres = psd->yvirtres = vinfo.yres;
  psd->bpp = pinfo.bpp;
  psd->planes = 1;
  psd->pitch = pinfo.stride;
  psd->size = pinfo.fblen;
  psd->ncolors = (psd->bpp >= 24) ? (1 << 24) : (1 << psd->bpp);
  psd->flags = PSF_SCREEN | PSF_ADDRMMAP;
  psd->portrait = MWPORTRAIT_NONE;

  psd->pixtype = screen_pixtype(vinfo.fmt);
  if (psd->pixtype < 0)
    {
      EPRINTF("Error: unsupported pixel type\n");
      goto fail;
    }

  psd->data_format = set_data_format(psd);

  psd->addr = mmap(NULL, psd->size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
  if (psd->addr == MAP_FAILED)
    {
      EPRINTF("Error: mmap failed\n");
      goto fail;
    }

  subdriver = select_fb_subdriver(psd);
  if (!subdriver)
    {
      EPRINTF("Error: no subdriver for %dbpp\n", psd->bpp);
      goto fail;
    }
  set_subdriver(psd, subdriver);

  return psd;

fail:
  if (fb >= 0)
    {
      close(fb);
      fb = -1;
    }
  return NULL;
}

static void fb_close(PSD psd)
{
  if (fb >= 0)
    {
      munmap(psd->addr, psd->size);
      close(fb);
      fb = -1;
    }
}

#ifdef FBIO_UPDATE

static void fb_update(PSD psd, MWCOORD x, MWCOORD y, MWCOORD width, MWCOORD height)
{
  struct fb_area_s area = {x, y, width, height};
  ioctl(fb, FBIO_UPDATE, &area);
}

#endif
