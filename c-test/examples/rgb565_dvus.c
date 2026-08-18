// DVUS — two-block falling-piece puzzle (domino Tetris)
// RGB565 port of c-test/examples/dvus.luau
//
// Original is a 360x380 UI with bitmap text. This port uses Doom's 160x100
// RGB565 window and rectangle stand-ins for labels (draw_char is still a stub).
//
// Game state is file-scope. The linker concatenates each object's GP range
// so these do not overlap libruntime BSS (display_width/height, malloc, …).
//
// Controls (Ripple keys):
//   Left / Right  move
//   Down          soft-drop
//   Up / Z        rotate (horizontal <-> vertical)
//   X             hard-drop; also start / restart

#include <graphics.h>
#include <mmio.h>

#define WIDTH  160
#define HEIGHT 100

#define GRID_W 10
#define GRID_H 20
#define CELL   4

#define BOARD_X 8
#define BOARD_Y 12

#define PREVIEW_X 58
#define PREVIEW_Y 16
#define PREVIEW_W 28
#define PREVIEW_H 16

#define STATE_MENU     0
#define STATE_PLAY     1
#define STATE_GAMEOVER 2

#define COL_BG       RGB565(5, 7, 12)
#define COL_PANEL    RGB565(16, 19, 31)
#define COL_GRID     RGB565(23, 49, 59)
#define COL_ACCENT   RGB565(80, 246, 200)
#define COL_GOLD     RGB565(255, 240, 168)
#define COL_STROKE   RGB565(248, 244, 192)
#define COL_MUTED    RGB565(143, 155, 179)
#define COL_DANGER   RGB565(243, 111, 69)
#define COL_CHROME   RGB565(70, 82, 103)
#define COL_TITLEBAR RGB565(16, 42, 55)
#define COL_PREVIEW  RGB565(7, 22, 31)

unsigned short piece_color(int idx) {
    if (idx == 0) return RGB565(0, 240, 240);
    if (idx == 1) return RGB565(244, 228, 9);
    if (idx == 2) return RGB565(40, 216, 67);
    if (idx == 3) return RGB565(225, 59, 59);
    if (idx == 4) return RGB565(42, 118, 221);
    if (idx == 5) return RGB565(240, 138, 28);
    return RGB565(184, 76, 255);
}

int board_index(int x, int y) {
    return y * GRID_W + x;
}

int cell_dx(int rotation, int i) {
    if (i == 0) return 0;
    if ((rotation & 1) == 0) return 1;
    return 0;
}

int cell_dy(int rotation, int i) {
    if (i == 0) return 0;
    if ((rotation & 1) == 0) return 0;
    return 1;
}

int is_valid(unsigned char *board, int gx, int gy, int rotation) {
    int i;
    for (i = 0; i < 2; i++) {
        int x = gx + cell_dx(rotation, i);
        int y = gy + cell_dy(rotation, i);
        if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) {
            return 0;
        }
        if (board[board_index(x, y)]) {
            return 0;
        }
    }
    return 1;
}

int try_move(unsigned char *board, int *px, int *py, int rotation, int dx, int dy) {
    int nx = *px + dx;
    int ny = *py + dy;
    if (!is_valid(board, nx, ny, rotation)) {
        return 0;
    }
    *px = nx;
    *py = ny;
    return 1;
}

void clear_board(unsigned char *board) {
    int i;
    for (i = 0; i < 200; i++) {
        board[i] = 0;
    }
}

int clear_lines(unsigned char *board) {
    int lines_cleared = 0;
    int y;
    int x;
    int move_y;

    for (y = GRID_H - 1; y >= 0; y--) {
        int complete = 1;
        for (x = 0; x < GRID_W; x++) {
            if (!board[board_index(x, y)]) {
                complete = 0;
                break;
            }
        }
        if (complete) {
            for (move_y = y; move_y > 0; move_y--) {
                for (x = 0; x < GRID_W; x++) {
                    board[board_index(x, move_y)] = board[board_index(x, move_y - 1)];
                }
            }
            for (x = 0; x < GRID_W; x++) {
                board[x] = 0;
            }
            lines_cleared++;
            y++;
        }
    }
    return lines_cleared;
}

int line_score(int n) {
    if (n == 1) return 100;
    if (n == 2) return 300;
    if (n == 3) return 500;
    if (n >= 4) return 800;
    return 0;
}

int drop_delay_for_level(int level) {
    int delay = 24 - (level - 1) * 2;
    if (delay < 4) delay = 4;
    return delay;
}

void draw_block(int gx, int gy, unsigned short color) {
    int x = BOARD_X + gx * CELL;
    int y = BOARD_Y + gy * CELL;
    fill_rect(x, y, CELL, CELL, color);
    draw_rect(x, y, CELL, CELL, COL_STROKE);
    fill_rect(x + 1, y + 1, CELL - 2, 1, RGB_WHITE);
}

void draw_block_at(int px, int py, int size, unsigned short color) {
    fill_rect(px, py, size, size, color);
    draw_rect(px, py, size, size, COL_STROKE);
}

void draw_chrome(void) {
    draw_rect(2, 2, WIDTH - 4, HEIGHT - 4, COL_CHROME);
    fill_rect(6, 5, WIDTH - 12, 5, COL_TITLEBAR);
    fill_rect(8, 6, 6, 3, COL_ACCENT);
    fill_rect(16, 6, 6, 3, COL_ACCENT);
    fill_rect(24, 6, 6, 3, COL_GOLD);
    fill_rect(32, 6, 6, 3, COL_GOLD);
}

void draw_board_frame(unsigned char *board) {
    int x;
    int y;

    fill_rect(BOARD_X - 2, BOARD_Y - 2, GRID_W * CELL + 4, GRID_H * CELL + 4, COL_PANEL);
    draw_rect(BOARD_X - 3, BOARD_Y - 3, GRID_W * CELL + 6, GRID_H * CELL + 6, COL_ACCENT);
    fill_rect(BOARD_X, BOARD_Y, GRID_W * CELL, GRID_H * CELL, COL_BG);

    for (x = 0; x <= GRID_W; x++) {
        draw_vline(BOARD_X + x * CELL, BOARD_Y, GRID_H * CELL, COL_GRID);
    }
    for (y = 0; y <= GRID_H; y++) {
        draw_hline(BOARD_X, BOARD_Y + y * CELL, GRID_W * CELL, COL_GRID);
    }

    for (y = 0; y < GRID_H; y++) {
        for (x = 0; x < GRID_W; x++) {
            int v = board[board_index(x, y)];
            if (v > 0) {
                draw_block(x, y, piece_color(v - 1));
            }
        }
    }
}

void draw_active(int px, int py, int rotation, int color_idx) {
    int i;
    unsigned short color = piece_color(color_idx);
    for (i = 0; i < 2; i++) {
        draw_block(px + cell_dx(rotation, i), py + cell_dy(rotation, i), color);
    }
}

void draw_preview(int color_idx) {
    int size = 8;
    int origin_x = PREVIEW_X + (PREVIEW_W - size * 2) / 2;
    int origin_y = PREVIEW_Y + (PREVIEW_H - size) / 2;
    unsigned short color = piece_color(color_idx);

    fill_rect(PREVIEW_X - 8, PREVIEW_Y - 8, 8, 3, COL_GOLD);
    fill_rect(PREVIEW_X, PREVIEW_Y, PREVIEW_W, PREVIEW_H, COL_PREVIEW);
    draw_rect(PREVIEW_X - 1, PREVIEW_Y - 1, PREVIEW_W + 2, PREVIEW_H + 2, COL_ACCENT);
    draw_block_at(origin_x, origin_y, size, color);
    draw_block_at(origin_x + size, origin_y, size, color);
}

void draw_bar_panel(int x, int y, int w, int h, int filled, int max_filled, unsigned short color) {
    int i;
    int n = filled;
    if (n > max_filled) n = max_filled;
    fill_rect(x, y, w, h, COL_PANEL);
    draw_rect(x, y, w, h, COL_MUTED);
    for (i = 0; i < n; i++) {
        fill_rect(x + 2, y + h - 4 - i * 3, w - 4, 2, color);
    }
}

void draw_hud(int score, int lines, int level) {
    int score_bars = (score / 100) % 12;
    int line_bars = lines % 10;
    int level_bars = level;
    if (level_bars > 12) level_bars = 12;

    draw_bar_panel(58, 40, 28, 40, score_bars, 12, COL_GOLD);
    draw_bar_panel(92, 40, 28, 40, line_bars, 10, COL_ACCENT);
    draw_bar_panel(126, 40, 28, 40, level_bars, 12, RGB_GREEN);

    fill_rect(58, 38, 10, 2, COL_GOLD);
    fill_rect(92, 38, 10, 2, COL_ACCENT);
    fill_rect(126, 38, 10, 2, RGB_GREEN);

    fill_rect(58, 86, 96, 8, COL_PANEL);
    set_pixel(64, 88, RGB_WHITE);
    set_pixel(63, 89, RGB_WHITE);
    set_pixel(65, 89, RGB_WHITE);
    set_pixel(72, 89, RGB_WHITE);
    set_pixel(73, 88, RGB_WHITE);
    set_pixel(73, 90, RGB_WHITE);
    set_pixel(80, 90, RGB_WHITE);
    set_pixel(79, 89, RGB_WHITE);
    set_pixel(81, 89, RGB_WHITE);
    fill_rect(90, 88, 3, 3, COL_ACCENT);
    fill_rect(100, 88, 5, 3, COL_DANGER);
}

void draw_menu_overlay(int blink) {
    fill_rect(BOARD_X + 4, BOARD_Y + 18, GRID_W * CELL - 8, 44, COL_BG);
    draw_rect(BOARD_X + 4, BOARD_Y + 18, GRID_W * CELL - 8, 44, COL_GOLD);
    fill_rect(BOARD_X + 8, BOARD_Y + 24, 6, 14, COL_GOLD);
    fill_rect(BOARD_X + 16, BOARD_Y + 24, 6, 14, COL_GOLD);
    fill_rect(BOARD_X + 24, BOARD_Y + 24, 6, 14, COL_GOLD);
    fill_rect(BOARD_X + 32, BOARD_Y + 24, 6, 14, COL_GOLD);
    if (blink) {
        fill_rect(BOARD_X + 10, BOARD_Y + 46, GRID_W * CELL - 20, 6, COL_ACCENT);
    }
}

void draw_gameover_overlay(void) {
    fill_rect(BOARD_X + 4, BOARD_Y + 24, GRID_W * CELL - 8, 36, COL_BG);
    draw_rect(BOARD_X + 4, BOARD_Y + 24, GRID_W * CELL - 8, 36, COL_DANGER);
    fill_rect(BOARD_X + 8, BOARD_Y + 30, GRID_W * CELL - 16, 8, COL_DANGER);
    fill_rect(BOARD_X + 10, BOARD_Y + 46, GRID_W * CELL - 20, 6, COL_ACCENT);
}

void lock_piece(unsigned char *board, int px, int py, int rotation, int color_idx) {
    int i;
    for (i = 0; i < 2; i++) {
        int x = px + cell_dx(rotation, i);
        int y = py + cell_dy(rotation, i);
        if (x >= 0 && x < GRID_W && y >= 0 && y < GRID_H) {
            board[board_index(x, y)] = (unsigned char)(color_idx + 1);
        }
    }
}

unsigned char board[200];
int state;
int score;
int lines;
int level;
int drop_timer;
int drop_delay;
int color_idx;
int next_color;
int px;
int py;
int rotation;
int last_left;
int last_right;
int last_up;
int last_down;
int last_z;
int last_x;
int move_delay;
int repeat_delay;
int soft_acc;
int blink;

int main() {
    int frame;
    unsigned short seed;

    graphics_init(WIDTH, HEIGHT);

    seed = 12345;
    seed = seed ^ (key_left_pressed() << 1);
    seed = seed ^ (key_right_pressed() << 2);
    seed = seed ^ (key_up_pressed() << 3);
    seed = seed ^ (key_down_pressed() << 4);
    seed = seed ^ (key_z_pressed() << 5);
    seed = seed ^ (key_x_pressed() << 6);
    rng_set_seed(seed);

    state = STATE_MENU;
    score = 0;
    lines = 0;
    level = 1;
    drop_timer = 0;
    drop_delay = drop_delay_for_level(1);
    color_idx = 0;
    next_color = rng_get() % 7;
    px = 4;
    py = 0;
    rotation = 0;
    last_left = 0;
    last_right = 0;
    last_up = 0;
    last_down = 0;
    last_z = 0;
    last_x = 0;
    move_delay = 0;
    repeat_delay = 0;
    soft_acc = 0;
    blink = 1;
    clear_board(board);

    for (frame = 0; frame < 30000; frame++) {
        int left = key_left_pressed();
        int right = key_right_pressed();
        int up = key_up_pressed();
        int down = key_down_pressed();
        int z = key_z_pressed();
        int x = key_x_pressed();
        int pressed_x = x && !last_x;
        int pressed_z = z && !last_z;
        int pressed_up = up && !last_up;
        int start_key = pressed_x || pressed_z;
        int settled = 0;

        if (state == STATE_MENU) {
            if (start_key) {
                clear_board(board);
                score = 0;
                lines = 0;
                level = 1;
                drop_delay = drop_delay_for_level(1);
                drop_timer = 0;
                next_color = rng_get() % 7;
                color_idx = next_color;
                next_color = rng_get() % 7;
                px = 4;
                py = 0;
                rotation = 0;
                state = STATE_PLAY;
                if (!is_valid(board, px, py, rotation)) {
                    state = STATE_GAMEOVER;
                }
            }
        } else if (state == STATE_GAMEOVER) {
            if (start_key) {
                state = STATE_MENU;
            }
        } else {
            if (move_delay == 0) {
                if (left && (!last_left || repeat_delay == 0)) {
                    try_move(board, &px, &py, rotation, -1, 0);
                    move_delay = 3;
                    repeat_delay = last_left ? 2 : 8;
                } else if (right && (!last_right || repeat_delay == 0)) {
                    try_move(board, &px, &py, rotation, 1, 0);
                    move_delay = 3;
                    repeat_delay = last_right ? 2 : 8;
                } else if (pressed_up || pressed_z) {
                    int next_rot = rotation + 1;
                    if (is_valid(board, px, py, next_rot)) {
                        rotation = next_rot;
                    }
                    move_delay = 5;
                } else if (pressed_x) {
                    int rows = 0;
                    while (try_move(board, &px, &py, rotation, 0, 1)) {
                        rows++;
                    }
                    score += rows * 2;
                    settled = 1;
                    move_delay = 4;
                }
            }

            if (state == STATE_PLAY && !settled && down && move_delay == 0) {
                soft_acc++;
                if (!last_down || soft_acc >= 2) {
                    soft_acc = 0;
                    if (try_move(board, &px, &py, rotation, 0, 1)) {
                        score++;
                        drop_timer = 0;
                    } else {
                        settled = 1;
                    }
                    move_delay = 2;
                }
            } else if (!down) {
                soft_acc = 0;
            }

            if (move_delay > 0) move_delay--;
            if (repeat_delay > 0) repeat_delay--;
            if (!left && !right) repeat_delay = 0;

            if (state == STATE_PLAY && !settled) {
                drop_timer++;
                if (drop_timer >= drop_delay) {
                    drop_timer = 0;
                    if (try_move(board, &px, &py, rotation, 0, 1)) {
                        score++;
                    } else {
                        settled = 1;
                    }
                }
            }

            if (settled) {
                int cleared;
                lock_piece(board, px, py, rotation, color_idx);
                cleared = clear_lines(board);
                if (cleared > 0) {
                    lines += cleared;
                    level = 1 + (lines / 10);
                    drop_delay = drop_delay_for_level(level);
                    score += line_score(cleared) * level;
                }
                color_idx = next_color;
                next_color = rng_get() % 7;
                px = 4;
                py = 0;
                rotation = 0;
                drop_timer = 0;
                if (!is_valid(board, px, py, rotation)) {
                    state = STATE_GAMEOVER;
                }
            }
        }

        last_left = left;
        last_right = right;
        last_up = up;
        last_down = down;
        last_z = z;
        last_x = x;

        if ((frame & 15) == 0) {
            blink = 1 - blink;
        }

        clear_screen(COL_BG);
        draw_chrome();
        draw_board_frame(board);
        if (state == STATE_PLAY) {
            draw_active(px, py, rotation, color_idx);
        }
        draw_preview(next_color);
        draw_hud(score, lines, level);
        if (state == STATE_MENU) {
            draw_menu_overlay(blink);
        } else if (state == STATE_GAMEOVER) {
            draw_gameover_overlay();
        }
        graphics_flush();
    }

    return 0;
}
