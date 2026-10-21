/* SPDX-License-Identifier: MIT */
#include "chat.h"
#include "composer.h"
#include <curses.h>
#include <errno.h>
#include <ripple_unicode.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define UI_LINES 8192
/* Display-only wrapped offsets. Model text can never emit terminal escapes. */
typedef struct {
    const char *p;
    unsigned n;
} Row;
static volatile sig_atomic_t stopped;
static void stop(int sig) {
    (void)sig;
    stopped = 1;
}
static void text(int y, int x, const char *s, unsigned long n, int width, chtype style) {
    unsigned used = 0, w;
    size_t at = 0, next;
    uint32_t cp;
    struct ru_decoder d = {0, 0, 0};
    int decoded;
    if (y < 0 || y >= LINES || x < 0 || x >= COLS || width <= 0)
        return;
    wmove(stdscr, y, x);
    wattrset(stdscr, style);
    while (at < n && used < (unsigned)width) {
        next = ru_next_grapheme((const unsigned char *)s, n, at, &w);
        if (next <= at || used + w > (unsigned)width)
            break;
        while (at < next) {
            decoded = ru_decode(&d, (unsigned char)s[at], &cp);
            if (decoded >= 0)
                ++at;
            if (decoded) {
                if (cp < 32 || (cp >= 127 && cp < 160))
                    cp = ' ';
                ripple_curses_scalar(stdscr, cp);
            }
        }
        used += w;
    }
}
static unsigned wrap(Row *rows, unsigned count, const char *s, unsigned width) {
    size_t at = 0, start = 0, n = strlen(s), next;
    unsigned cells = 0, w;
    while (at < n && count < UI_LINES) {
        if (s[at] == '\n') {
            rows[count].p = s + start;
            rows[count++].n = at - start;
            start = ++at;
            cells = 0;
            continue;
        }
        next = ru_next_grapheme((const unsigned char *)s, n, at, &w);
        if (next <= at)
            break;
        if (cells + w > width && at > start) {
            rows[count].p = s + start;
            rows[count++].n = at - start;
            start = at;
            cells = 0;
            continue;
        }
        cells += w;
        at = next;
    }
    if (count < UI_LINES) {
        rows[count].p = s + start;
        rows[count++].n = at - start;
    }
    return count;
}
static const char *shortcuts(int cols) {
    return cols >= 100 ? "Enter send  ^O newline  ^W word  ^N new  F3 panel  F4 tools  Tab chats  Esc stop  F10 quit"
        : cols >= 60 ? "Enter send  ^W word  F3 panel  F4 tools  Tab chats  F10 quit"
        : "Enter send  ^W word  F3/F4";
}
/* Painting and hit testing use the same shortcut strings at every pane size. */
static int shortcut_key(int cols, int column, uint32_t *key) {
    static const char *labels[] = {"Enter send", "^O newline", "^W word", "^N new", "F3 panel", "F4 tools", "Tab chats", "Esc stop", "F10 quit", "F3/F4"};
    static const unsigned keys[] = {13, 15, 23, 14, KEY_F(3), KEY_F(4), 9, 27, KEY_F(10), KEY_F(3)};
    const char *line = shortcuts(cols), *at;
    unsigned i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        at = strstr(line, labels[i]);
        if (at && column >= 1 + (int)(at - line) && column < 1 + (int)(at - line) + (int)strlen(labels[i]) && column < cols - 1) {
            *key = keys[i];
            if (i == 9 && column >= 1 + (int)(at - line) + 3) *key = KEY_F(4);
            return *key >= 256 ? KEY_CODE_YES : OK;
        }
    }
    return ERR;
}

int chat_ui(Chat *c) {
    ChatEditor *editor = calloc(1, sizeof(*editor));
    ChatMentions *matches = calloc(1, sizeof(*matches));
    char *input = editor ? editor->text : NULL;
    Row *rows = calloc(UI_LINES, sizeof(*rows));
    unsigned count, scroll = 0, chosen = c->selected, at, i, w, side, top;
    unsigned long revision = ~0UL;
    int dirty = 1, focus = 0, pasting = 0, mention_dirty = 1, kind, height, cols, cy = 0, cx = 0, old_rows = 0,
        old_cols = 0, pending_kind = ERR;
    uint32_t pending_key = 0;
    uint32_t key;
    unsigned char encoded[5];
    chtype bar = A_REVERSE, accent = A_BOLD;
    if (!input || !rows || !matches) {
        free(editor);
        free(matches);
        free(rows);
        return 1;
    }
    if (!initscr()) {
        fprintf(stderr, "chat: cannot start terminal (check /etc/termcap and tty)\n");
        free(editor);
        free(matches);
        free(rows);
        return 1;
    }
    raw();
    noecho();
    keypad(stdscr, 1);
    wtimeout(stdscr, 25);
    ripple_curses_bracketed_paste(1);
    mousemask(BUTTON1_PRESSED | BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
    if (start_color() == OK) {
        init_pair(1, COLOR_WHITE, COLOR_BLUE);
        init_pair(2, COLOR_CYAN, COLOR_BLACK);
        bar = COLOR_PAIR(1) | A_BOLD;
        accent = COLOR_PAIR(2) | A_BOLD;
    }
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    stopped = 0;
    while (!stopped) {
        chat_step(c);
        if (mention_dirty) {
            if (!pasting && !focus) chat_mentions(c, editor, matches);
            else matches->active = 0;
            mention_dirty = 0;
        }
        if (c->revision != revision) {
            revision = c->revision;
            dirty = 1;
        }
        ripple_curses_resize();
        if (old_rows != LINES || old_cols != COLS) {
            old_rows = LINES;
            old_cols = COLS;
            dirty = 1;
        }
        if (dirty) {
            /* werase fills blanks using the window's current attributes. */
            wattrset(stdscr, A_NORMAL);
            erase();
            cols = COLS;
            /* Tab temporarily reveals the chooser even in a narrow mux pane. */
            side = focus || c->sidebar == CHAT_SIDEBAR_VISIBLE ||
                (c->sidebar == CHAT_SIDEBAR_AUTO && cols >= 72) ? (cols >= 72 ? 20 : cols / 2) : 0;
            height = LINES - 9;
            text(0, 1, "Fuse Chat / GPT-6.1 Sol Light", sizeof("Fuse Chat / GPT-6.1 Sol Light") - 1,
                 cols - 2, bar);
            {
                char mode[600];
                snprintf(mode, sizeof(mode), c->workspace[0] ? "Workspace: %s" : "Chat | /workspace DIR enables guest tools", c->workspace);
                text(1, 1, mode, strlen(mode), cols - 2, accent);
            }
            if (LINES < 14 || cols < 32) {
                text(3, 1, "Please enlarge to 32x14.",
                     sizeof("Please enlarge to 32x14.") - 1, cols - 2, accent);
                curs_set(0);
            } else {
                if (side) {
                    text(2, 1, "Conversations", sizeof("Conversations") - 1, side - 2, accent);
                    top = chosen >= (unsigned)height ? chosen - height + 1 : 0;
                    for (i = 0; i < (unsigned)height && top + i < c->count; i++)
                        text(3 + i, 1, c->titles[top + i], strlen(c->titles[top + i]), side - 2,
                             focus && top + i == chosen ? bar
                             : top + i == c->selected   ? A_BOLD
                                                        : 0);
                }
                w = cols - side - 3;
                count = wrap(rows, 0, c->display, w);
                if (c->reply_size) {
                    count = wrap(rows, count,
                                 c->worker ? "Assistant (streaming)\n" : "Assistant (unsaved)\n", w);
                    count = wrap(rows, count, c->reply, w);
                }
                if (!c->count) {
                    text(3, side + 2, "Start a conversation. Your history is empty.",
                         sizeof("Start a conversation. Your history is empty.") - 1, w, accent);
                    text(5, side + 2, "Ctrl-N new | F2 import sign-in | F10 quit",
                         sizeof("Ctrl-N new | F2 import sign-in | F10 quit") - 1, w, 0);
                }
                if (scroll > count)
                    scroll = count;
                at = count > (unsigned)height + scroll ? count - height - scroll : 0;
                for (i = 0; i < (unsigned)height && at + i < count; i++)
                    text(2 + i, side + 2, rows[at + i].p, rows[at + i].n, w, 0);
                if (matches->active && !focus) {
                    int y = LINES - 13, line, column;
                    char label[280];
                    const char *title = "Files (@) | Tab/Enter choose | Esc close";
                    top = matches->selected >= 5 ? matches->selected - 4 : 0;
                    for (line = 0; line < 6; line++) {
                        wmove(stdscr, y + line, 1); wattrset(stdscr, A_NORMAL);
                        for (column = 1; column < cols - 1; column++) waddch(stdscr, ' ');
                    }
                    text(y, 1, title, strlen(title), cols - 2, bar);
                    if (!matches->count) {
                        const char *empty = matches->error[0] ? matches->error : "No matching files";
                        text(y + 1, 2, empty, strlen(empty), cols - 4, accent);
                    }
                    for (i = 0; i < 5 && top + i < matches->count; i++) {
                        snprintf(label, sizeof(label), "%s%s", matches->entries[top + i].name,
                            matches->entries[top + i].directory ? "/" : "");
                        text(y + 1 + i, 2, label, strlen(label), cols - 4,
                            top + i == matches->selected ? bar : A_NORMAL);
                    }
                    if (matches->more) {
                        const char *more = "More matches: type a longer prefix";
                        text(LINES - 7, 1, more, strlen(more), cols - 2, accent);
                    }
                }
                if (!matches->active || !matches->more)
                    text(LINES - 7, 1, c->status, strlen(c->status), cols - 2, accent);
                /* Composer scrolls with its cursor and retains exact UTF-8. */
                count = wrap(rows, 0, input, cols - 4);
                at = 0;
                for (i = 0; i < count; i++)
                    if ((unsigned long)(rows[i].p - input) <= editor->cursor)
                        at = i;
                top = at >= 3 ? at - 2 : 0;
                for (i = 0; i < 3 && top + i < count; i++)
                    text(LINES - 5 + i, 2, rows[top + i].p, rows[top + i].n, cols - 4,
                         focus ? 0 : A_BOLD);
                cy = LINES - 5 + (int)(at - top);
                cx = 2 +
                     ru_columns((const unsigned char *)rows[at].p, editor->cursor - (rows[at].p - input));
                if (cx >= cols - 1)
                    cx = cols - 2;
                {
                    const char *keys = shortcuts(cols);
                    text(LINES - 1, 1, keys, strlen(keys), cols - 2, bar);
                }
                curs_set(!focus);
                if (!focus)
                    wmove(stdscr, cy, cx);
            }
            refresh();
            dirty = 0;
        }
        errno = 0;
        if (pending_kind != ERR) {
            kind = pending_kind; key = pending_key; pending_kind = ERR;
        } else kind = wget_wch(stdscr, &key);
        if (kind == ERR) {
            if (feof(stdin) || errno == EIO || errno == EBADF)
                break;
            continue;
        }
        dirty = 1;
        if (kind == KEY_CODE_YES && key == KEY_MOUSE) {
            MEVENT mouse;
            unsigned selected_top, index, visible, maximum;
            int wheel;
            if (getmouse(&mouse) != OK || pasting || LINES < 14 || COLS < 32) continue;
            wheel = mouse.bstate & BUTTON4_PRESSED ? -1 : mouse.bstate & BUTTON5_PRESSED ? 1 : 0;
            if (mouse.x < 0 || mouse.x >= COLS || mouse.y < 0 || mouse.y >= LINES) continue;
            if (mouse.y == LINES - 1 && (mouse.bstate & BUTTON1_PRESSED)) {
                kind = shortcut_key(cols, mouse.x, &key);
                if (kind == ERR) continue;
                /* Continue through the normal keyboard action policy. */
            } else if (matches->active && !focus && mouse.y >= LINES - 13 && mouse.y < LINES - 7) {
                if (wheel && matches->count) {
                    if (wheel < 0 && matches->selected) --matches->selected;
                    if (wheel > 0 && matches->selected + 1 < matches->count) ++matches->selected;
                } else if ((mouse.bstate & BUTTON1_PRESSED) && mouse.y > LINES - 13 && mouse.x >= 2 && mouse.x < cols - 2) {
                    selected_top = matches->selected >= 5 ? matches->selected - 4 : 0;
                    index = selected_top + mouse.y - (LINES - 12);
                    if (index < matches->count) {
                        matches->selected = index;
                        if (chat_mention_apply(editor, matches)) mention_dirty = 1;
                    }
                }
                continue;
            } else if (side && mouse.x < (int)side && mouse.y >= 3 && mouse.y < 3 + height) {
                if (wheel && c->count) {
                    if (wheel < 0) chosen = chosen > 3 ? chosen - 3 : 0;
                    else chosen = chosen + 3 < c->count ? chosen + 3 : c->count - 1;
                    focus = 1;
                    mention_dirty = 1;
                } else if (mouse.bstate & BUTTON1_PRESSED) {
                    selected_top = chosen >= (unsigned)height ? chosen - height + 1 : 0;
                    index = selected_top + mouse.y - 3;
                    if (index < c->count) {
                        chosen = index;
                        chat_select(c, chosen);
                        focus = 0;
                        scroll = 0;
                        mention_dirty = 1;
                    }
                }
                continue;
            } else if (mouse.y >= LINES - 5 && mouse.y < LINES - 2 && mouse.x >= 2 && mouse.x < cols - 2 && (mouse.bstate & BUTTON1_PRESSED)) {
                count = wrap(rows, 0, input, cols - 4);
                at = 0;
                for (i = 0; i < count; ++i)
                    if ((unsigned long)(rows[i].p - input) <= editor->cursor) at = i;
                selected_top = at >= 3 ? at - 2 : 0;
                index = selected_top + mouse.y - (LINES - 5);
                if (index >= count) index = count - 1;
                editor->cursor = rows[index].p - input + ru_prefix((const unsigned char *)rows[index].p, rows[index].n, mouse.x - 2);
                focus = 0;
                mention_dirty = 1;
                continue;
            } else if (wheel && mouse.x >= (int)side + 2 && mouse.y >= 2 && mouse.y < 2 + height) {
                count = wrap(rows, 0, c->display, cols - side - 3);
                if (c->reply_size) {
                    count = wrap(rows, count, c->worker ? "Assistant (streaming)\n" : "Assistant (unsaved)\n", cols - side - 3);
                    count = wrap(rows, count, c->reply, cols - side - 3);
                }
                visible = height;
                maximum = count > visible ? count - visible : 0;
                if (wheel < 0) scroll = scroll + 3 < maximum ? scroll + 3 : maximum;
                else scroll = scroll > 3 ? scroll - 3 : 0;
                continue;
            } else continue;
        }
        if (kind == KEY_CODE_YES && key == KEY_RESIZE)
            continue;
        if (kind == KEY_CODE_YES && key == KEY_PASTE_BEGIN) {
            pasting = 1;
            matches->active = 0;
            continue;
        }
        if (kind == KEY_CODE_YES && key == KEY_PASTE_END) {
            pasting = 0;
            mention_dirty = 1;
            continue;
        }
        if (pasting) {
            if (kind == OK && (key == 10 || key == 13)) chat_editor_insert(editor, "\n", 1);
            else if (kind == OK && key >= 32 && key != 127) {
                w = ru_encode(key, encoded);
                chat_editor_insert(editor, (char *)encoded, w);
            }
            continue;
        }
        if (!pasting && ((kind == KEY_CODE_YES && key == KEY_F(10)) || (kind == OK && key == 4)))
            break;
        if (!pasting && kind == KEY_CODE_YES && key == KEY_F(2)) {
            chat_import(c, "/mnt/host/chat-login.json");
            continue;
        }
        if (!pasting && kind == OK && key == 27) {
            int action = -1, next_kind;
            uint32_t next_key;
            wtimeout(stdscr, 0);
            next_kind = wget_wch(stdscr, &next_key);
            wtimeout(stdscr, 25);
            if (!focus && next_kind == OK) {
                if (next_key == 'b' || next_key == 'B') action = EDIT_WORD_LEFT;
                if (next_key == 'f' || next_key == 'F') action = EDIT_WORD_RIGHT;
                if (next_key == 'd' || next_key == 'D') action = EDIT_WORD_DELETE;
                if (next_key == 127 || next_key == 8) action = EDIT_WORD_ERASE;
            }
            if (!focus && next_kind == KEY_CODE_YES && next_key == KEY_BACKSPACE) action = EDIT_WORD_ERASE;
            if (action >= 0) {
                chat_editor_action(editor, action); mention_dirty = 1;
            } else {
                if (next_kind != ERR) { pending_kind = next_kind; pending_key = next_key; }
                if (matches->active) matches->active = 0;
                else { chat_cancel(c); focus = 0; }
            }
            continue;
        }
        if (kind == KEY_CODE_YES && key == KEY_F(4)) {
            chat_tool_view(c);
            scroll = 0;
            continue;
        }
        if (!pasting && kind == OK && key == 14) {
            chat_new(c);
            chosen = c->selected;
            focus = 0;
            scroll = 0;
            mention_dirty = 1;
            continue;
        }
        if (!pasting && kind == OK && key == 18) {
            chat_send(c, "");
            scroll = 0;
            continue;
        }
        if (!pasting && ((kind == OK && key == 2) || (kind == KEY_CODE_YES && key == KEY_F(3)))) {
            chat_sidebar(c, side != 0);
            focus = 0;
            continue;
        }
        if (matches->active && !focus) {
            if (kind == KEY_CODE_YES && (key == KEY_UP || key == KEY_DOWN || key == KEY_BTAB)) {
                if (matches->count) {
                    if (key == KEY_DOWN) matches->selected = (matches->selected + 1) % matches->count;
                    else matches->selected = (matches->selected + matches->count - 1) % matches->count;
                }
                continue;
            }
            if ((kind == OK && key == 9) || (kind == OK && (key == 13 || key == 10) && matches->count)) {
                if (chat_mention_apply(editor, matches)) mention_dirty = 1;
                else matches->active = 0;
                continue;
            }
        }
        if (!pasting && kind == OK && key == 9) {
            focus = !focus;
            chosen = c->selected;
            mention_dirty = 1;
            continue;
        }
        if (kind == KEY_CODE_YES && key == KEY_PPAGE) {
            scroll += height > 0 ? height : 1;
            continue;
        }
        if (kind == KEY_CODE_YES && key == KEY_NPAGE) {
            w = height > 0 ? height : 1;
            scroll = scroll > w ? scroll - w : 0;
            continue;
        }
        if (focus) {
            if (kind == KEY_CODE_YES && key == KEY_UP && chosen)
                chosen--;
            if (kind == KEY_CODE_YES && key == KEY_DOWN && chosen + 1 < c->count)
                chosen++;
            if (kind == OK && (key == 13 || key == 10)) {
                chat_select(c, chosen);
                focus = 0;
                scroll = 0;
                mention_dirty = 1;
            }
            continue;
        }
        if (!pasting && kind == OK && (key == 13 || key == 10)) {
            int accepted = 0;
            if (!strncmp(input, "/workspace ", 11))
                accepted = chat_workspace(c, input + 11);
            else if (!strcmp(input, "/chat"))
                accepted = chat_new(c);
            else if (*input)
                accepted = chat_send(c, input);
            if (accepted) {
                input[0] = 0;
                editor->cursor = 0;
                scroll = 0;
                chosen = c->selected;
                mention_dirty = 1;
            }
            continue;
        }
        mention_dirty = 1;
        if (kind == OK && key == 15) {
            chat_editor_insert(editor, "\n", 1);
            continue;
        }
        if (kind == KEY_CODE_YES && (key == KEY_UP || key == KEY_DOWN)) {
            count = wrap(rows, 0, input, cols - 4);
            at = 0;
            for (i = 0; i < count; i++)
                if ((unsigned long)(rows[i].p - input) <= editor->cursor) at = i;
            w = ru_columns((const unsigned char *)rows[at].p, editor->cursor - (rows[at].p - input));
            if (key == KEY_UP && at) --at;
            else if (key == KEY_DOWN && at + 1 < count) ++at;
            editor->cursor = rows[at].p - input + ru_prefix((const unsigned char *)rows[at].p, rows[at].n, w);
            continue;
        }
        {
            int action = -1;
            if (kind == KEY_CODE_YES) {
                if (key == KEY_BACKSPACE) action = EDIT_BACKSPACE;
                if (key == KEY_DC) action = EDIT_DELETE;
                if (key == KEY_LEFT) action = EDIT_LEFT;
                if (key == KEY_RIGHT) action = EDIT_RIGHT;
                if (key == KEY_HOME) action = EDIT_HOME;
                if (key == KEY_END) action = EDIT_END;
            } else if (kind == OK) {
                if (key == 127 || key == 8) action = EDIT_BACKSPACE;
                if (key == 1) action = EDIT_HOME;
                if (key == 5) action = EDIT_END;
                if (key == 23) action = EDIT_WORD_ERASE;
                if (key == 21) action = EDIT_KILL_BEFORE;
                if (key == 11) action = EDIT_KILL_AFTER;
                if (key == 25) action = EDIT_YANK;
                if (key == 12) { ripple_curses_invalidate(); continue; }
            }
            if (action >= 0) { chat_editor_action(editor, action); continue; }
        }
        if (kind == OK && key >= 32 && key != 127) {
            w = ru_encode(key, encoded);
            chat_editor_insert(editor, (char *)encoded, w);
        }
    }
    chat_cancel(c);
    ripple_curses_bracketed_paste(0);
    endwin();
    free(rows);
    free(editor);
    free(matches);
    return 0;
}
