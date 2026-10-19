#!/usr/bin/env python3
"""Port the pinned NanoTetris while retaining its pieces, board and drawing."""
import shutil
from pathlib import Path
from adapt import replace
from server_adapt import body

HERE = Path(__file__).resolve().parent


def adapt_tetris(dest):
    for name in ('nxtetris.c', 'nxtetris.h'):
        shutil.copyfile(HERE / 'upstream/src/demos/nanox' / name,
                        dest / 'demos/nanox' / name)
    c, h = 'demos/nanox/nxtetris.c', 'demos/nanox/nxtetris.h'
    replace(dest, h, '//#define USE_HISCORE_FILE', '#define USE_HISCORE_FILE')
    for member in ('score', 'hiscore', 'fhiscore'):
        replace(dest, h, f'\tint {member};', f'\tlong {member};')
    replace(dest, h, '\tstruct timeval timeout;', '\tunsigned long deadline;')
    for name in (c, h):
        replace(dest, name, 'unsigned long timeout_delay(', 'long timeout_delay(')
    replace(dest, c, '#include "nxtetris.h"',
            '#include "nxtetris.h"\n#include "tetris-platform.h"')
    replace(dest, c, 'GR_WM_PROPS_APPWINDOW, "Tetris"',
            'GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NORESIZE, "Tetris"')
    replace(dest, c, 'GR_ROOT_WINDOW_ID, -1, -1,', 'GR_ROOT_WINDOW_ID, 32, 32,')
    replace(dest, c, '\tGrSelectEvents(state->main_window,',
            '\tif(!state->main_window)tetris_no_resource();\n\tGrSelectEvents(state->main_window,')
    replace(dest, c, '\tGrGetScreenInfo(&si);',
            '\ttetris_connected=1;\n\tGrGetScreenInfo(&si);')
    path = dest / c
    text = path.read_text().replace('= GrNewWindow(', '= tetris_window(').replace('= GrNewGC();', '= tetris_gc();')
    text = text.replace('random() %', 'rand() %').replace('srandom(time(0));', 'srand(tetris_seed);')
    # A vertical I is four rows tall: the upstream extra -1 spawned it at
    # row -1, before the board. Keep every shape inside the hidden spawn rows.
    text = text.replace('shape_sizes[state->next_shape.type][state->next_shape.orientation][1] - 1;',
                        'shape_sizes[state->next_shape.type][state->next_shape.orientation][1];\n\tif(state->current_shape.y<0)state->current_shape.y=0;')
    # Width checks must precede every board access. Rotation can cross either
    # edge, and the hidden spawn rows are still part of the allocated board.
    text = text.replace('if((yy == WELL_HEIGHT) || (xx == WELL_WIDTH) ||',
                        'if(yy<0 || yy>=WELL_HEIGHT || xx<0 || xx>=WELL_WIDTH ||')
    # int is 16 bits; scores must survive more than 327 lines.
    text = text.replace('sprintf(buf, "%d", state->score);', 'sprintf(buf, "%ld", state->score);')
    text = text.replace('sprintf(buf, "%d", state->hiscore);', 'sprintf(buf, "%ld", state->hiscore);')
    # Remove blocking per-cell and line deletion sleeps; hard drop is immediate.
    text = text.replace('\t\tGrDelay(DELETE_LINE_DELAY);', '')
    text = text.replace('while(!drop_block_1(state)) GrDelay(DROP_BLOCK_DELAY);',
                        'while(!drop_block_1(state)) {}')
    # The upstream loop started at column WELL_WIDTH (out of range), skipped
    # column zero, and failed to clear the newly exposed top row.
    text = text.replace('for(x = WELL_WIDTH; x; x--)', 'for(x = 0; x < WELL_WIDTH; x++)')
    text = text.replace('\t\tstate->score += SCORE_INCREMENT;',
                        '\t\tif(state->score<=2147483647L-SCORE_INCREMENT)state->score += SCORE_INCREMENT;\n\t\twrite_hiscore(state);')
    path.write_text(text)
    body(dest, c, 'delete_line', '''    int x,y;
    if(line<WELL_NOTVISIBLE || line>=WELL_HEIGHT)return;
    for(y=line;y>0;y--)for(x=0;x<WELL_WIDTH;x++)state->blocks[0][y][x]=state->blocks[0][y-1][x];
    memset(state->blocks[0][0],0,sizeof(state->blocks[0][0]));
    draw_well(state,0);''')
    # Save before a new game resets the score, not just at application exit.
    replace(dest, c, '\tclear_well(state);', '\twrite_hiscore(state);\n\tclear_well(state);')
    body(dest, c, 'calculate_timeout', '    state->deadline=tetris_milliseconds()+delays[state->level];')
    body(dest, c, 'timeout_delay', '''    long remaining=(long)(state->deadline-tetris_milliseconds());
    return remaining>0?remaining:1;''')
    body(dest, c, 'do_update', '''    if((long)(tetris_milliseconds()-state->deadline)>=0){
        drop_block_1(state);calculate_timeout(state);
    }''')
    # Both high-score versions exist under preprocessor branches in upstream.
    # Replace only the enabled branch and leave the fallback definition alone.
    body(dest, c, 'read_hiscore', '    state->hiscore=state->fhiscore=tetris_read_score();')
    body(dest, c, 'write_hiscore', '    tetris_save_score(state);')
    body(dest, c, 'handle_keyboard_event', '    tetris_keyboard(state);')
    body(dest, c, 'handle_mouse_event', '    tetris_mouse(state);')
    body(dest, c, 'main', '    return tetris_main(argc,argv);')
    with path.open('a') as output:
        output.write('\n#include "tetris-platform.inc"\n')
