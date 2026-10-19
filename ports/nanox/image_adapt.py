#!/usr/bin/env python3
"""Attach only the versioned C16 colour-resource extension; leave codecs off."""
from adapt import replace


def adapt_images(dest):
    path = dest / 'nanox/client.c'
    path.write_text(path.read_text() + '\n#include "image-client.inc"\n')
    replace(dest, 'nanox/srvnet.c', ' current_fd=fd; curfunc=',
            ' current_fd=fd;\n if(r->reqType==253){nxr_image_dispatch(fd,packet);return 0;}\n curfunc=')
