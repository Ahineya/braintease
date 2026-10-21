"""Share identical weather artwork within one LGeneral terrain type.

The K.u.k. database has twelve weather variants and generally three distinct
images per terrain type. Their fogged images are identical too. Keep separate
tables for weather rules, but load and precompute each artwork only once.
"""

from pathlib import Path


def _replace(text: str, old: str, new: str, name: str) -> str:
    if text.count(old) != 1:
        raise ValueError(f"LGeneral terrain source changed at {name}")
    return text.replace(old, new, 1)


def adapt(root: Path, c16: bool) -> None:
    del c16
    path = Path(root) / "src/terrain.c"
    text = path.read_text()
    text = _replace(text, "int terrain_load( char *fname )", """/* Weather aliases are local to a terrain type; clear aliases before freeing
 * their surface, including when a partially populated load fails. */
static void lg_free_weather_images(SDL_Surface **images, int count)
{
    int i, j;
    if (!images) return;
    for (i = 0; i < count; i++) {
        SDL_Surface *surface = images[i];
        if (!surface) continue;
        for (j = i; j < count; j++)
            if (images[j] == surface) images[j] = 0;
        SDL_FreeSurface(surface);
    }
    free(images);
}

int terrain_load( char *fname )""", "alias cleanup helper")
    text = _replace(text, "    int i, j, k;", "    int i, j, k, alias;\n    const char **image_names = 0;\n    Uint32 lg_started = SDL_GetTicks();\n    unsigned long lg_unique_images = 0;", "loader locals")
    start = text.index("        /* each weather type got its own image")
    end = text.index("        /* spot cost */", start)
    text = text[:start] + """        /* Weather variants can share artwork without sharing their rules.
         * Parser-owned names remain valid until parser_free(pd) below. */
        terrain_types[i].images = calloc(weather_type_count, sizeof(SDL_Surface *));
        image_names = calloc(weather_type_count, sizeof(*image_names));
        if (!terrain_types[i].images || !image_names) goto failure;
        for (j = 0; j < weather_type_count; j++) {
            sprintf(path, "image/%s", weather_types[j].id);
            if (!parser_get_value(sub, path, &str, 0)) goto parser_failure;
            if (j > 0 && STRCMP("default", str)) {
                terrain_types[i].images[j] = terrain_types[i].images[0];
                image_names[j] = image_names[0];
                continue;
            }
            image_names[j] = str;
            for (alias = 0; alias < j; alias++)
                if (image_names[alias] && !strcmp(image_names[alias], str)) break;
            if (alias < j) {
                terrain_types[i].images[j] = terrain_types[i].images[alias];
                continue;
            }
            sprintf(path, "terrain/%s", str);
            if (!(terrain_types[i].images[j] = load_surf(path, SDL_SWSURFACE))) goto failure;
            lg_unique_images++;
            SDL_SetColorKey(terrain_types[i].images[j], SDL_SRCCOLORKEY,
                            get_pixel(terrain_types[i].images[j], 0, 0));
        }
        /* One fogged derivative per unique source surface. */
        terrain_types[i].images_fogged = calloc(weather_type_count, sizeof(SDL_Surface *));
        if (!terrain_types[i].images_fogged) goto failure;
        for (j = 0; j < weather_type_count; j++) {
            for (alias = 0; alias < j; alias++)
                if (terrain_types[i].images[alias] == terrain_types[i].images[j]) break;
            if (alias < j) {
                terrain_types[i].images_fogged[j] = terrain_types[i].images_fogged[alias];
                continue;
            }
            terrain_types[i].images_fogged[j] = create_surf(
                terrain_types[i].images[j]->w, terrain_types[i].images[j]->h, SDL_SWSURFACE);
            if (!terrain_types[i].images_fogged[j]) goto failure;
            FULL_DEST(terrain_types[i].images_fogged[j]);
            FULL_SOURCE(terrain_types[i].images[j]);
            blit_surf();
            count = terrain_types[i].images[j]->w / hex_w;
            for (k = 0; k < count; k++) {
                DEST(terrain_types[i].images_fogged[j], k * hex_w, 0, hex_w, hex_h);
                SOURCE(terrain_icons->fog, 0, 0);
                alpha_blit_surf(FOG_ALPHA);
            }
            SDL_SetColorKey(terrain_types[i].images_fogged[j], SDL_SRCCOLORKEY,
                            get_pixel(terrain_types[i].images[j], 0, 0));
        }
        free(image_names); image_names = 0;
""" + text[end:]
    text = _replace(text, "failure:\n    terrain_delete();", "failure:\n    free(image_names);\n    terrain_delete();", "failed load temporary names")
    start = text.index("            if ( terrain_types[i].images ) {")
    end = text.index("            if ( terrain_types[i].flags )", start)
    text = text[:start] + """            lg_free_weather_images(terrain_types[i].images, weather_type_count);
            lg_free_weather_images(terrain_types[i].images_fogged, weather_type_count);
""" + text[end:]
    text = _replace(text, "void terrain_delete( void )\n{\n    int i, j;",
                    "void terrain_delete( void )\n{\n    int i;", "destructor locals")
    text = _replace(text, "    free(domain);\n    return 1;", """    free(domain);
    if (getenv("LGENERAL_TRACE"))
        fprintf(stderr, "LGENERAL TERRAIN_TIMING %lu images %lu ms\\n",
                lg_unique_images, (unsigned long)(SDL_GetTicks() - lg_started));
    return 1;""", "terrain load timing")
    path.write_text(text)
