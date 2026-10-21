/* Included at the end of the adapted engine.c only in test builds.
 * Tests retain the real parser, assets, action queue, game rules and AI. */
#include "parser.h"

static int lg_test_failures;
#define LG_CHECK(expr, label) do { if (!(expr)) { \
    fprintf(stderr, "FAIL lgeneral: %s\n", label); ++lg_test_failures; \
} else { printf("PASS lgeneral: %s\n", label); } } while (0)

/* Hash logical file octets, never the C16 carrier's physical representation.
 * Explicit masking gives identical FNV-1a behavior on C16 and LP64 hosts. */
static int lg_test_save_digest(unsigned long *length, unsigned long *digest)
{
    FILE *file = fopen(slot_get_fname(0), "rb");
    unsigned char buffer[1024];
    size_t count, i;
    unsigned long hash = 2166136261UL, total = 0;
    FILE *snapshot = 0;
    static int snapshot_id;
    if (!file) return 0;
    if (getenv("LGENERAL_TEST_SAVE_SNAPSHOTS")) {
        char path[1024];
        snprintf(path, sizeof path, "%s/port-save-%d.bin", config.dir_name, snapshot_id++);
        snapshot = fopen(path, "wb");
    }
    while ((count = fread(buffer, 1, sizeof buffer, file)) != 0) {
        if (snapshot) fwrite(buffer, 1, count, snapshot);
        for (i = 0; i < count; ++i)
            hash = ((hash ^ ((unsigned long)buffer[i] & 255UL)) * 16777619UL) & 0xffffffffUL;
        total += (unsigned long)count;
    }
    if (snapshot) fclose(snapshot);
    if (ferror(file)) { fclose(file); return 0; }
    if (fclose(file) != 0) return 0;
    *length = total; *digest = hash;
    return 1;
}

static unsigned long lg_test_property_evaluations(void)
{
    unsigned long hash = 2166136261UL;
    ListIterator it = list_iterator(units);
    Unit *unit;
    int property, shift;
    while ((unit = list_iterator_next(&it)) != 0) {
        for (property = 0; property < 2; ++property) {
            unsigned long value = (unsigned long)(property ?
                unit->trsp_prop.eval_score : unit->prop.eval_score);
            for (shift = 0; shift < 32; shift += 8)
                hash = ((hash ^ ((value >> shift) & 255UL)) * 16777619UL) & 0xffffffffUL;
        }
    }
    return hash;
}

static int lg_test_drain_actions(void)
{
    int i, reinit = 0;
    for (i = 0; i < 20000 && !end_scen; ++i) {
        engine_handle_next_action(&reinit);
        engine_update(1000);
        if (!actions_count() && status == STATUS_NONE) return 1;
    }
    return 0;
}

static int lg_test_move(void)
{
    ListIterator it = list_iterator(units);
    Unit *unit;
    int x, y, original_x, original_y;
    while ((unit = list_iterator_next(&it)) != 0) {
        if (unit->player != cur_player || unit->killed || unit->cur_mov < 1) continue;
        engine_select_unit(unit);
        original_x = unit->x; original_y = unit->y;
        for (x = 0; x < map_w; ++x) for (y = 0; y < map_h; ++y) {
            if (x == original_x && y == original_y) continue;
            if (!mask[x][y].in_range || mask[x][y].blocked) continue;
            if (map[x][y].g_unit || map[x][y].a_unit) continue;
            action_queue_move(unit, x, y);
            if (!lg_test_drain_actions()) return 0;
            if (unit->x != original_x || unit->y != original_y) return 1;
        }
    }
    return 0;
}

static int lg_test_combat(void)
{
    ListIterator ours = list_iterator(units);
    Unit *attacker, *target;
    int direction, x, y, old_x, old_y, old_ammo, old_count;
    /* A controlled encounter uses actual K.u.k. unit properties and terrain.
     * Positioning is fixture setup; the attack itself goes through the queue. */
    while ((attacker = list_iterator_next(&ours)) != 0) {
        ListIterator theirs;
        if (attacker->player != cur_player || attacker->killed ||
            (attacker->prop.flags & (FLYING | SWIMMING))) continue;
        theirs = list_iterator(units);
        while ((target = list_iterator_next(&theirs)) != 0) {
            if (target->killed || player_is_ally(attacker->player, target->player) ||
                (target->prop.flags & (FLYING | SWIMMING))) continue;
            for (direction = 0; direction < 6; ++direction) {
                if (!get_close_hex_pos(target->x, target->y, direction, &x, &y) ||
                    map[x][y].g_unit) continue;
                old_x = attacker->x; old_y = attacker->y;
                map_remove_unit(attacker);
                attacker->x = x; attacker->y = y;
                attacker->terrain = map[x][y].terrain;
                map_insert_unit(attacker);
                scen_prep_unit(attacker, SCEN_PREP_UNIT_FIRST);
                map_set_spot_mask(); map_set_fog(F_SPOT);
                if (!unit_check_attack(attacker, target, UNIT_ACTIVE_ATTACK)) {
                    map_remove_unit(attacker);
                    attacker->x = old_x; attacker->y = old_y;
                    attacker->terrain = map[old_x][old_y].terrain;
                    map_insert_unit(attacker);
                    continue;
                }
                old_ammo = attacker->cur_ammo;
                old_count = attacker->cur_atk_count;
                action_queue_attack(attacker, target);
                if (!lg_test_drain_actions()) return 0;
                return attacker->cur_ammo != old_ammo ||
                       attacker->cur_atk_count != old_count;
            }
        }
    }
    return 0;
}

static int lg_test_capture(void)
{
    ListIterator it = list_iterator(units);
    Unit *unit;
    int x, y, prestige;
    while ((unit = list_iterator_next(&it)) != 0) {
        if (unit->player != cur_player || unit->killed ||
            (unit->sel_prop->flags & (FLYING | SWIMMING))) continue;
        for (x = 0; x < map_w; ++x) for (y = 0; y < map_h; ++y) {
            if (!map[x][y].nation || !map[x][y].player ||
                player_is_ally(map[x][y].player, cur_player)) continue;
            /* Controlled capture fixture: remove a defending unit if all
             * enemy flags are initially occupied. The saved game is reloaded
             * immediately after this rules test, before running the AI. */
            if (map[x][y].g_unit) map_remove_unit(map[x][y].g_unit);
            map_remove_unit(unit);
            unit->x = x; unit->y = y; unit->terrain = map[x][y].terrain;
            map_insert_unit(unit);
            prestige = cur_player->cur_prestige;
            return engine_capture_flag(unit) &&
                map[x][y].player == cur_player &&
                cur_player->cur_prestige > prestige;
        }
    }
    return 0;
}

static int lg_test_restore(int expected_units, int expected_turn, int expected_prestige)
{
    engine_shutdown();
    setup.type = SETUP_LOAD_GAME;
    setup.slot_id = 0;
    if (!engine_init()) return 0;
    return units->count == expected_units && turn == expected_turn &&
           cur_player && cur_player->cur_prestige == expected_prestige;
}

static void lg_test_resources(void)
{
    static const char *scenarios[] = {
        "Balkans", "Caporetto", "Carpathians", "South_Tyrol", "Gallipolli",
        "Kerensky", "Isonzo_1917", "Otranto_1917", "Albania", "Ancona",
        "Otranto_1918", "The_Collapse", "Piave", "Pelagosa", "Bosnia",
        "Romania", "Verona", "Serbia_1915", "Serbia_1914", "Brusilov",
        "Isonzo_1915", "Gorlice"
    };
    char path[512];
    PData *pd;
    int i, count = 0, wide_value = 0;
    LG_CHECK(GROUND_TRSP_OK == 0x08000000L && CARPET_BOMBING == 0x10000000L,
             "high unit flag bits retain 32-bit semantics");
    snprintf(path, sizeof(path), "%s/units/kukgen.udb", get_gamedir());
    pd = parser_read_file("units", path);
    if (pd) {
        List *values = list_create(LIST_AUTO_DELETE, LIST_NO_CALLBACK);
        list_add(values, strdup("70000"));
        parser_insert_new_pdata(pd, "port_integer_width", values);
        LG_CHECK(parser_get_int(pd, "port_integer_width", &wide_value) && wide_value == 70000L,
                 "parser integers exceed 16-bit range without truncation");
        parser_free(&pd);
    } else LG_CHECK(0, "complete 151 KiB unit database loads");
    for (i = 0; i < (int)(sizeof(scenarios)/sizeof(scenarios[0])); ++i) {
        snprintf(path, sizeof(path), "%s/scenarios/kukgen/%s", get_gamedir(), scenarios[i]);
        pd = parser_read_file(scenarios[i], path);
        if (pd) { ++count; parser_free(&pd); }
        else fprintf(stderr, "%s\n", parser_get_error());
    }
    LG_CHECK(count == (int)(sizeof(scenarios)/sizeof(scenarios[0])),
             "all 22 distributed K.u.k. scenario resources parse");
    if (getenv("LGENERAL_TEST_ALL_SCENARIOS")) {
        for (i = 0; i < (int)(sizeof(scenarios)/sizeof(scenarios[0])); ++i) {
            setup.type = SETUP_DEFAULT_SCEN;
            snprintf(setup.fname, sizeof(setup.fname), "kukgen/%s", scenarios[i]);
            if (engine_init()) {
                LG_CHECK(map_w > 0 && map_h > 0 && units->count > 0,
                         "full scenario loads with map, nations, units and graphics");
                printf("LGENERAL SCENARIO %s %dx%d units=%d\n",
                       scenarios[i], map_w, map_h, units->count);
            } else {
                fprintf(stderr, "Failed scenario: %s\n", scenarios[i]);
                LG_CHECK(0, "full scenario loads with map, nations, units and graphics");
            }
            engine_shutdown();
        }
    }
}

int lgeneral_engine_test(void)
{
    int count, saved_turn, saved_prestige, i, reinit = 0;
    int ai_actions = 0, ai_moves = 0, ai_attacks = 0;
    Player *first;
    Action *next;
    unsigned long save_length = 0, save_digest = 0;
    unsigned long restored_length = 0, restored_digest = 0;
    unsigned long property_evaluations = 0;
    lgeneral_testing = 1;
    srand(1);
    config.deploy_turn = 0;
    config.show_cpu_turn = 0;
    config.anim_speed = 100;
    config.ai_debug = 0;
    config.fog_of_war = 0;
    lg_test_resources();
    setup.type = SETUP_DEFAULT_SCEN;
    strcpy(setup.fname, "kukgen/Serbia_1914");
    if (!engine_init()) {
        LG_CHECK(0, "load complete K.u.k. Serbia 1914 scenario");
        return 1;
    }
    LG_CHECK(map_w > 0 && map_h > 0 && players->count >= 2 && units->count > 10,
             "load complete K.u.k. Serbia 1914 scenario");
    first = cur_player;
    first->ctrl = PLAYER_CTRL_HUMAN;
    engine_begin_turn(first, 0);
    blind_cpu_turn = 1;
    LG_CHECK(lg_test_move(), "queued legal movement updates unit and map");
    engine_select_unit(0);
    count = units->count; saved_turn = turn;
    saved_prestige = cur_player->cur_prestige;
    property_evaluations = lg_test_property_evaluations();
    LG_CHECK(slot_save(0, "Port smoke"), "save full scenario state");
    LG_CHECK(lg_test_save_digest(&save_length, &save_digest) && save_length > 0,
             "read canonical save octets and record length and digest");
    LG_CHECK(lg_test_combat(), "queued combat consumes ammunition or attack action");
    LG_CHECK(lg_test_capture(), "capture hostile flag and award prestige");
    LG_CHECK(lg_test_restore(count, saved_turn, saved_prestige),
             "reload restores units, turn, current player and prestige");
    if (!cur_player) return 1;
    LG_CHECK(lg_test_property_evaluations() == property_evaluations,
             "unit and transporter AI evaluations retain loaded library normalization");
    LG_CHECK(slot_save(0, "Port smoke"), "atomically overwrite existing save slot after reload");
    LG_CHECK(lg_test_save_digest(&restored_length, &restored_digest) &&
             restored_length == save_length && restored_digest == save_digest,
             "complete canonical save survives load and overwrite unchanged");
    printf("LGENERAL SAVE octets=%lu hash=%lu restored_octets=%lu restored_hash=%lu\n",
           save_length, save_digest, restored_length, restored_digest);
    first = cur_player;
    first->ctrl = PLAYER_CTRL_HUMAN;
    for (i = 0; i < players->count; ++i) {
        Player *player = player_get_by_index(i);
        if (player != first) {
            player->ctrl = PLAYER_CTRL_CPU;
            player->ai_init = ai_init;
            player->ai_run = ai_run;
            player->ai_finalize = ai_finalize;
        }
    }
    engine_begin_turn(first, 1);
    action_queue_end_turn();
    for (i = 0; i < 100000 && !end_scen; ++i) {
        engine_check_events(&reinit);
        next = actions_top();
        if (next && cur_ctrl == PLAYER_CTRL_CPU) {
            ++ai_actions;
            if (next->type == ACTION_MOVE) ++ai_moves;
            if (next->type == ACTION_ATTACK) ++ai_attacks;
        }
        engine_handle_next_action(&reinit);
        engine_update(1000);
        if (cur_player == first && turn > saved_turn && status == STATUS_NONE) break;
    }
    printf("LGENERAL AI actions=%d moves=%d attacks=%d steps=%d\n",
           ai_actions, ai_moves, ai_attacks, i);
    LG_CHECK(cur_player == first && turn > saved_turn && ai_actions > 1,
             "end turn, execute built-in AI, return control to human");
    engine_shutdown();
    LG_CHECK(camp_load("kukgen"), "load branching K.u.k. campaign");
    camp_set_cur(0);
    LG_CHECK(camp_cur_scen && camp_cur_scen->scen &&
             !strcmp(camp_cur_scen->scen, "kukgen/Serbia_1914"), "campaign starts at Serbia 1914");
    LG_CHECK(camp_set_next("major") && camp_cur_scen && camp_cur_scen->scen &&
             !strcmp(camp_cur_scen->scen, "kukgen/Carpathians"), "major victory advances campaign branch");
    if (camp_cur_scen && camp_cur_scen->scen) {
        strcpy(setup.fname, camp_cur_scen->scen);
        setup.type = SETUP_DEFAULT_SCEN;
        LG_CHECK(engine_init(), "next campaign scenario loads into engine");
    }
    printf("LGENERAL ENGINE %s failures=%d\n", lg_test_failures ? "FAIL" : "PASS", lg_test_failures);
    return lg_test_failures ? 1 : 0;
}
