#include "npit_internal.h"
#include <ftw.h>

static bool update_command(char **arguments) {
    GError *error = NULL;
    int status = 0;
    if (!g_spawn_sync(NULL, arguments, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN, NULL, NULL, NULL, NULL, &status, &error)) {
        fprintf(stderr, "npit: %s\n", error->message);
        g_error_free(error);
        return false;
    }
    if (!g_spawn_check_wait_status(status, &error)) {
        fprintf(stderr, "npit: %s\n", error->message);
        g_error_free(error);
        return false;
    }
    return true;
}

static bool update_tool(const char *name, bool privileged, const char *first, const char *second, const char *third) {
    char *program = g_find_program_in_path(name);
    if (!program) { fprintf(stderr, "npit: required tool missing: %s\n", name); return false; }
    char *arguments[9];
    int count = 0;
    if (privileged) { arguments[count++] = "sudo"; arguments[count++] = "--"; }
    arguments[count++] = program;
    arguments[count++] = (char *)first;
    arguments[count++] = "--";
    arguments[count++] = (char *)second;
    if (third) arguments[count++] = (char *)third;
    arguments[count] = NULL;
    bool success = update_command(arguments);
    g_free(program);
    return success;
}

static bool writable_directory(const char *directory) {
    char *path = g_strdup(directory);
    while (access(path, F_OK) && strcmp(path, "/")) {
        char *parent = g_path_get_dirname(path);
        g_free(path);
        path = parent;
    }
    bool writable = access(path, W_OK) == 0;
    g_free(path);
    return writable;
}

static bool update_file(const char *path, bool executable) {
    struct stat details;
    return !lstat(path, &details) && S_ISREG(details.st_mode) && (!executable || access(path, X_OK) == 0);
}

static int remove_update_file(const char *path, const struct stat *details, int type, struct FTW *walk) {
    (void)details; (void)type; (void)walk;
    return remove(path);
}

int update_app(void) {
    if (!strcmp(NPIT_PREFIX, "/usr") || !strcmp(NPIT_BINDIR, "/usr/bin") || !strcmp(NPIT_BINDIR, "/bin")) {
        fputs("npit: update this installation through your package manager\n", stderr);
        return 1;
    }
    if (geteuid() == 0) {
        fputs("npit: run npit --update without sudo; installation will request privileges if needed\n", stderr);
        return 1;
    }
    const char *requirements[] = {"git", "make", "pkg-config", "install", "mv", "rm"};
    for (size_t index = 0; index < sizeof(requirements) / sizeof(*requirements); index++) {
        char *program = g_find_program_in_path(requirements[index]);
        if (!program) { fprintf(stderr, "npit: install %s before updating\n", requirements[index]); return 1; }
        g_free(program);
    }
    bool privileged = !writable_directory(NPIT_BINDIR) || !writable_directory(NPIT_DATADIR);
    if (privileged) {
        char *sudo_path = g_find_program_in_path("sudo");
        if (!sudo_path) { fputs("npit: sudo is required to update this installation\n", stderr); return 1; }
        g_free(sudo_path);
    }
    char *directory = g_build_filename(g_get_tmp_dir(), "npit-update-XXXXXX", NULL);
    if (!mkdtemp(directory)) { perror("npit: could not create update directory"); g_free(directory); return 1; }
    char *checkout = g_build_filename(directory, "source", NULL);
    char *binary = g_build_filename(checkout, "npit", NULL);
    char *settings = g_build_filename(checkout, "settings.toml", NULL);
    char *cava = g_build_filename(checkout, "cava.conf", NULL);
    char *target = g_build_filename(NPIT_BINDIR, "npit", NULL);
    char *settings_target = g_build_filename(NPIT_DATADIR, "settings.toml", NULL);
    char *cava_target = g_build_filename(NPIT_DATADIR, "cava.conf", NULL);
    char *temporary_name = g_strdup_printf(".npit-update-%s", strrchr(directory, '-') + 1);
    char *temporary_binary = g_build_filename(NPIT_BINDIR, temporary_name, NULL);
    char *prefix_argument = g_strdup_printf("PREFIX=%s", NPIT_PREFIX);
    char *bindir_argument = g_strdup_printf("BINDIR=%s", NPIT_BINDIR);
    char *datadir_argument = g_strdup_printf("DATADIR=%s", NPIT_DATADIR);
    long processors = sysconf(_SC_NPROCESSORS_ONLN);
    char jobs[16];
    snprintf(jobs, sizeof(jobs), "-j%ld", processors < 1 ? 1 : processors > 4 ? 4 : processors);
    bool installed = false;
    bool staged = false;
    puts("Downloading the latest NPIT from GitHub main..."); fflush(stdout);
    char *clone[] = {"git", "clone", "--depth", "1", "--single-branch", "--branch", "main", "--", "https://github.com/buscook/npit.git", checkout, NULL};
    if (!update_command(clone)) goto done;
    puts("Building NPIT..."); fflush(stdout);
    char *build[] = {"make", "-C", checkout, jobs, prefix_argument, bindir_argument, datadir_argument, NULL};
    if (!update_command(build)) goto done;
    if (!update_file(binary, true) || !update_file(settings, false) || !update_file(cava, false)) {
        fputs("npit: the update build is incomplete\n", stderr);
        goto done;
    }
    char *verify[] = {binary, "--version", NULL};
    if (!update_command(verify)) goto done;
    puts(privileged ? "Installing NPIT (sudo may ask for your password)..." : "Installing NPIT..."); fflush(stdout);
    staged = true;
    if (!update_tool("install", privileged, "-Dm755", binary, temporary_binary)) goto done;
    if (!update_tool("install", privileged, "-Dm644", settings, settings_target)) goto done;
    if (!update_tool("install", privileged, "-Dm644", cava, cava_target)) goto done;
    if (!update_tool("mv", privileged, "-f", temporary_binary, target)) goto done;
    installed = true;
    printf("NPIT updated: %s\nYour personal settings and Spotify tokens were preserved.\n", target);
done:
    if (staged && !installed) update_tool("rm", privileged, "-f", temporary_binary, NULL);
    if (!installed) fputs("npit: update failed; the installed app was not replaced\n", stderr);
    nftw(directory, remove_update_file, 32, FTW_DEPTH | FTW_PHYS);
    g_free(datadir_argument); g_free(bindir_argument); g_free(prefix_argument);
    g_free(temporary_binary); g_free(temporary_name);
    g_free(cava_target); g_free(settings_target); g_free(target);
    g_free(cava); g_free(settings); g_free(binary); g_free(checkout); g_free(directory);
    return installed ? 0 : 1;
}
