#!/usr/bin/env python3
"""NXDSKTOP drawing and companion clients, with explicit C16/platform policy."""
import re
import shutil
from pathlib import Path
from adapt import replace

HERE = Path(__file__).resolve().parent


def drawing_function(source, name):
    """Retain complete upstream drawing functions, including their signatures."""
    match = re.search(r'static (?:void|int)\s+' + name + r'\([^;{}]*\)\s*\{', source)
    if not match:
        raise RuntimeError(f'NXDSKTOP drawing function missing: {name}')
    start = source.index('{', match.start())
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda m: ' ' * len(m.group()), source, flags=re.S)
    depth, end = 1, start + 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


def adapt_desktop(dest):
    directory = dest / 'demos/nanox'
    source = (HERE / 'upstream/src/demos/nanox/nxdsktop.c').read_text()
    # Keep the attribution/history and the actual upstream menu/bar artwork.
    # A separate panel and popup leave the root drawable by nxroach. Platform
    # process/descriptor/signal policy belongs in the adapter, not the import.
    drawing = '\n'.join(drawing_function(source, name) for name in
                        ('draw3drect', 'draw_taskbar', 'draw_menu', 'in_rect'))
    drawing = drawing.replace('int my = height - TASKBAR_HEIGHT - menu_h;', 'int my = 0;')
    drawing = drawing.replace('(SYS_COUNT * MENU_ITEM_EXIT_HEIGHT) + 4;',
                              '(SYS_COUNT * MENU_ITEM_EXIT_HEIGHT) + 8;')
    (directory / 'nxdsktop.c').write_text(source[:source.index('#include')] +
        '#include "nxdsktop-platform.h"\n' + drawing +
        '#include "nxdsktop-platform.inc"\n')
    for name in ('nxmsg', 'nxselect', 'nxcalc'):
        shutil.copyfile(HERE / f'upstream/src/demos/nanox/{name}.c', directory / f'{name}.c')
    # Fixed geometry clients must not advertise resizing they cannot handle.
    replace(dest, 'demos/nanox/nxselect.c', 'GR_WM_PROPS_APPWINDOW;',
            'GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NORESIZE;')
    replace(dest, 'demos/nanox/nxselect.c', 'GrOpen();',
            'if(GrOpen()<0){fputs("nxselect: cannot connect to Nano-X\\n",stderr);return 1;}')
    replace(dest, 'demos/nanox/nxselect.c', 'getcwd(cwd, sizeof(cwd));',
            'if(!getcwd(cwd,sizeof(cwd))){perror("nxselect: cwd");return 1;}')
    # Keep navigation predictable, reject path truncation, and page an entire
    # directory rather than silently hiding everything after entry 70.
    replace(dest, 'demos/nanox/nxselect.c', '#include <stdio.h>',
            '#include <stdio.h>\n#include <stdlib.h>')
    replace(dest, 'demos/nanox/nxselect.c', '#define MAX_FILES        70', '#define MAX_FILES        512')
    replace(dest, 'demos/nanox/nxselect.c', '#define MAX_PATH_LEN     128', '#define MAX_PATH_LEN     256')
    replace(dest, 'demos/nanox/nxselect.c', 'static GR_COORD win_width  = 200;', 'static GR_COORD win_width  = 320;')
    replace(dest, 'demos/nanox/nxselect.c', 'snprintf(out, n, "%s/%s", a, b);',
            'snprintf(out,n,"%s%s%s",a,!strcmp(a,"/")?"":"/",b);')
    replace(dest, 'demos/nanox/nxselect.c', 'safe_strcpy(file_list[file_count], e->d_name, MAX_NAME_LEN);',
            'if(strlen(path)+strlen(e->d_name)+2>sizeof(full))continue;\n        safe_strcpy(file_list[file_count], e->d_name, MAX_NAME_LEN);')
    replace(dest, 'demos/nanox/nxselect.c', 'closedir(d);', '''closedir(d);
    { int i,j;char name[MAX_NAME_LEN];unsigned char kind;
      for(i=2;i<file_count;i++)for(j=i;j>1&&strcmp(file_list[j-1],file_list[j])>0;j--){
        strcpy(name,file_list[j]);strcpy(file_list[j],file_list[j-1]);strcpy(file_list[j-1],name);
        kind=file_is_dir[j];file_is_dir[j]=file_is_dir[j-1];file_is_dir[j-1]=kind;
      }
    }''')
    replace(dest, 'demos/nanox/nxselect.c', 'if (y < g_list_top)',
            'if (y < g_list_top || y >= g_list_top+g_list_height)')
    replace(dest, 'demos/nanox/nxselect.c', '''int max_offset = (file_count-1) - g_page_size;
    if (max_offset < 0) max_offset = 0;''',
            'int max_offset = file_count>1 ? ((file_count-2)/g_page_size)*g_page_size : 0;')
    replace(dest, 'demos/nanox/nxselect.c', 'running = 0;\n            }', 'if(accepted)running = 0;\n            }')
    replace(dest, 'demos/nanox/nxselect.c', 'GrMapWindow(win);',
            'if(!win||!gc){GrClose();return 1;}\n    GrSetGCUseBackground(gc,GR_FALSE);\n    GrMapWindow(win);GrSetFocus(win);\n    if(getenv("NANOX_TRACE"))fprintf(stderr,"NXSELECT_READY %d\\n",getpid());')
    replace(dest, 'demos/nanox/nxselect.c', 'set_title(win, "Select File");',
            'if(!win){GrClose();return 1;}\n    set_title(win, "Select File");')
    replace(dest, 'demos/nanox/nxselect.c', 'GR_EVENT_MASK_CLOSE_REQ);',
            'GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);')
    replace(dest, 'demos/nanox/nxselect.c', 'else if (ev.type == GR_EVENT_TYPE_CLOSE_REQ)',
            'else if(ev.type==GR_EVENT_TYPE_KEY_DOWN&&ev.keystroke.ch==MWKEY_ESCAPE)running=0;\n        else if (ev.type == GR_EVENT_TYPE_CLOSE_REQ)')
    # Source split_lines wrote a 33rd row after exactly 32 newlines.
    replace(dest, 'demos/nanox/nxmsg.c', 'lines[line_count][j] = 0;\n    line_count++;',
            'if(line_count<MAX_LINES){lines[line_count][j]=0;line_count++;}')
    replace(dest, 'demos/nanox/nxmsg.c', 'GrOpen();',
            'if(GrOpen()<0){fputs("nxmsg: cannot connect to Nano-X\\n",stderr);return 1;}')
    replace(dest, 'demos/nanox/nxmsg.c', 'props.flags = GR_WM_FLAGS_TITLE;',
            'props.flags = GR_WM_FLAGS_TITLE | GR_WM_FLAGS_PROPS;\n        props.props=GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NORESIZE;')
    replace(dest, 'demos/nanox/nxmsg.c', "(k->ch == MWKEY_ENTER || k->ch == '\\r'))\n        return 1;",
            "(k->ch == MWKEY_ENTER || k->ch == '\\r')) { accepted=1;return 1; }")
    replace(dest, 'demos/nanox/nxmsg.c', 'GrMapWindow(win);',
            'if(!win||!gc){GrClose();return 1;}\n    GrMapWindow(win);GrSetFocus(win);\n    if(getenv("NANOX_TRACE"))fprintf(stderr,"NXMSG_READY %d\\n",getpid());')
    replace(dest, 'demos/nanox/nxmsg.c', 'gc = GrNewGC();',
            'gc = GrNewGC();if(!gc){GrClose();return 1;}')
    replace(dest, 'demos/nanox/nxmsg.c', '        GR_WM_PROPERTIES props;',
            '        GR_WM_PROPERTIES props;\n        if(!win){GrClose();return 1;}')
    # Calculator uses the existing binary64 math/formatting runtime, not host FP.
    replace(dest, 'demos/nanox/nxcalc.c', 'GR_WM_PROPS_APPWINDOW, "nxcalc"',
            'GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NORESIZE, "Calculator"')
    replace(dest, 'demos/nanox/nxcalc.c', 'int r, c, idx, tw, th, tb;',
            'int r,c,idx;GR_SIZE tw,th,tb;')
    replace(dest, 'demos/nanox/nxcalc.c', 'GrMapWindow(win);',
            'if(!win||!gc_text||!gc_button||!gc_button_press||!gc_border){GrClose();return 1;}\n    GrMapWindow(win);\n    if(getenv("NANOX_TRACE"))fprintf(stderr,"NXCALC_READY %d\\n",getpid());')
    replace(dest, 'demos/nanox/nxcalc.c', '    GrSelectEvents(win,',
            '    if(!win){GrClose();return 1;}\n    GrSelectEvents(win,')
    replace(dest, 'demos/nanox/nxcalc.c', '    gc_border = GrNewGC();',
            '    gc_border = GrNewGC();\n    if(!gc_text||!gc_button||!gc_button_press||!gc_border){GrClose();return 1;}')
    replace(dest, 'demos/nanox/nxcalc.c', "static char pending_op = 0;", "static char pending_op = 0;\nstatic int calc_error;")
    replace(dest, 'demos/nanox/nxcalc.c', "    char tmp[128];\n    double val;", "    char tmp[128];\n    double val;\n    if(calc_error){calc_error=0;acc=0;pending_op=0;entering=dot_entered=0;strcpy(display,\"0\");}")
    replace(dest, 'demos/nanox/nxcalc.c', '            strncpy(display, "Error", sizeof(display)-1);',
            '            calc_error=1;\n            strncpy(display, "Error", sizeof(display)-1);')
    replace(dest, 'demos/nanox/nxcalc.c', '        pending_op = label[0];', '        if(calc_error)return;\n        pending_op = label[0];')
    replace(dest, 'demos/nanox/nxcalc.c', '        apply_pending(val);\n        pending_op = 0;',
            '        apply_pending(val);\n        if(calc_error)return;\n        pending_op = 0;')

    # Keep the upstream calculator arithmetic, but avoid per-click full repaints
    # and synchronous color/font queries in the drawing loop.
    from server_adapt import body
    replace(dest,'demos/nanox/nxcalc.c','/* ========================= MAIN ========================= */',
            '#include "calc-platform.inc"\n/* ========================= MAIN ========================= */')
    replace(dest,'demos/nanox/nxcalc.c','    GrMapWindow(win);',
            '    GrGetFontInfo(0,&calc_font);\n    for(idx=0;idx<ROWS*COLS;idx++)if(btn_labels[idx])label_width[idx]=calc_width(btn_labels[idx]);\n'
            '    GrSetGCForeground(gc_button,GrGetSysColor(GR_COLOR_WINDOW));\n    GrMapWindow(win);')
    p=dest/'demos/nanox/nxcalc.c';text=p.read_text()
    start=text.index('            case GR_EVENT_TYPE_BUTTON_DOWN: {')
    end=text.index('            case GR_EVENT_TYPE_CLOSE_REQ:',start)
    text=text[:start]+"""            case GR_EVENT_TYPE_BUTTON_DOWN:
                if(pressed>=0)draw_button(pressed,0);
                pressed=button_at(event.button.x,event.button.y);
                if(pressed>=0)draw_button(pressed,1);
                GrFlush();break;
            case GR_EVENT_TYPE_BUTTON_UP:
                idx=button_at(event.button.x,event.button.y);
                if(pressed>=0){
                    if(idx==pressed){press_button(btn_labels[idx]);draw_display();}
                    draw_button(pressed,0);pressed=-1;GrFlush();
                }
                break;

"""+text[end:]
    text=text.replace('            redraw();\n            return;', '            draw_display();\n            return;')
    p.write_text(text)
    body(dest,'demos/nanox/nxcalc.c','redraw',
         '    int i;draw_display();for(i=0;i<ROWS*COLS;i++)draw_button(i,i==pressed);GrFlush();')
