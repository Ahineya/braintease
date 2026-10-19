/* NetHack's writable prefixes belong to the current user's home. */
#include "hack.h"
#include <sys/stat.h>
#include <errno.h>

static char homepath[PATHLEN], state[PATHLEN], data[PATHLEN];
static void make_dir(const char *name)
{
    struct stat st;
    if (mkdir(name,0700) < 0 && errno != EEXIST) error("Cannot create %s.",name);
    if (stat(name,&st) < 0 || !S_ISDIR(st.st_mode)) error("Not a directory: %s.",name);
}
static void touch_state(const char *name)
{
    char path[PATHLEN];
    int fd;
    if (snprintf(path,sizeof path,"%s%s",state,name) >= sizeof path) error("Home path is too long.");
    fd = open(path,O_WRONLY|O_CREAT,0600);
    if (fd < 0) error("Cannot open %s.",path);
    close(fd);
}
void ripple_nethack_paths(const char **directory, const char *program)
{
    const char *home = getenv("HOME"), *slash;
    int i;
    struct stat st;
    if (!home || home[0] != '/') error("Set HOME to your home directory before starting NetHack.");
    if (snprintf(homepath,sizeof homepath,"%s/",home) >= sizeof homepath ||
        snprintf(state,sizeof state,"%s/.nethack/",home) >= sizeof state) error("Home path is too long.");
    /* NetHack derives its startup umask from file permissions, which strips
     * directory execute bits. Our per-user directories need owner search. */
    umask(0077);
    make_dir(state);
    if (snprintf(data,sizeof data,"%ssave",state) >= sizeof data) error("Home path is too long.");
    make_dir(data);
    for (i = 0; i < PREFIX_COUNT; ++i) fqn_prefix[i] = state;
    fqn_prefix[CONFIGPREFIX] = homepath;
    /* -d and NETHACKDIR retain their normal meaning. Otherwise find assets
     * beside the executable, then fall back to the installed data directory. */
    if (!*directory && program && (slash = strrchr(program,'/')) != 0) {
        unsigned long length = slash - program;
        if (length + sizeof "/nethack-data" < sizeof data) {
            memcpy(data,program,length);
            strcpy(data + length,"/nethack-data");
            if (stat(data,&st) == 0 && S_ISDIR(st.st_mode)) *directory = data;
        }
    }
    if (!*directory) *directory = HACKDIR;
    /* Canonical absolute prefixes keep options, help and level data valid even
     * if another subsystem changes the working directory. */
    if (chdir(*directory) < 0) error("Cannot open NetHack data directory %s.",*directory);
    if (!getcwd(data,sizeof data - 2)) error("Cannot resolve NetHack data directory.");
    strcat(data,"/");
    *directory = data;
    fqn_prefix[HACKPREFIX] = fqn_prefix[DATAPREFIX] = fqn_prefix[SYSCONFPREFIX] = data;
    touch_state("record"); touch_state("perm"); touch_state("logfile"); touch_state("xlogfile");
}
