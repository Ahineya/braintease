#!/usr/bin/env python3
"""Bounded text clipboard extension, shared by native and VM clients."""
from adapt import replace


def adapt_clipboard(dest):
    path = dest/'nanox/client.c'
    path.write_text(path.read_text()+'\n#include "clipboard-client.inc"\n')
    replace(dest, 'nanox/srvnet.c', ' current_fd=fd;',
            ' current_fd=fd;\n if(r->reqType==251){nxr_clipboard_dispatch(fd,packet);return 0;}')
