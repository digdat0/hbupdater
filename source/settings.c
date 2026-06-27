#include "settings.h"
#include "config.h"
#include "jsonutil.h"
#include "fsutil.h"


#include <stdio.h>
#include <stdlib.h>

static void defaults(Settings *s) {
    s->first_run_done = false;
    s->scan_on_launch = false; /* manual scanning by default */
    s->install_overlays = true;
    s->install_sysmodules = false; /* risky: off by default */
    s->install_payloads = false;   /* risky: off by default */
    s->auto_backup = true;
    s->github_token[0] = '\0';
}

void settings_load(Settings *s) {
    defaults(s);

    size_t len = 0;
    char *js = json_read_file(SETTINGS_PATH, &len);
    if (!js) {
        settings_save(s); /* seed a default file so users can find the knobs */
        return;
    }
    int ntok = 0;
    jsmntok_t *tok = json_parse_alloc(js, len, &ntok);
    if (tok && tok[0].type == JSMN_OBJECT) {
        int i = json_obj_get(js, tok, 0, "first_run_done");
        if (i >= 0) {
            s->first_run_done = json_bool(js, tok, i);
        }
        i = json_obj_get(js, tok, 0, "scan_on_launch");
        if (i >= 0) {
            s->scan_on_launch = json_bool(js, tok, i);
        }
        i = json_obj_get(js, tok, 0, "install_overlays");
        if (i >= 0) {
            s->install_overlays = json_bool(js, tok, i);
        }
        i = json_obj_get(js, tok, 0, "install_sysmodules");
        if (i >= 0) {
            s->install_sysmodules = json_bool(js, tok, i);
        }
        i = json_obj_get(js, tok, 0, "install_payloads");
        if (i >= 0) {
            s->install_payloads = json_bool(js, tok, i);
        }
        i = json_obj_get(js, tok, 0, "auto_backup");
        if (i >= 0) {
            s->auto_backup = json_bool(js, tok, i);
        }
        i = json_obj_get(js, tok, 0, "github_token");
        if (i >= 0) {
            json_copy(js, tok, i, s->github_token, sizeof(s->github_token));
        }
    }
    free(tok);
    free(js);
}

bool settings_save(const Settings *s) {
    fs_mkdir_p(CONFIG_DIR);
    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (!f) {
        return false;
    }
    fprintf(f,
            "{\n"
            "  \"first_run_done\": %s,\n"
            "  \"scan_on_launch\": %s,\n"
            "  \"install_overlays\": %s,\n"
            "  \"install_sysmodules\": %s,\n"
            "  \"install_payloads\": %s,\n"
            "  \"auto_backup\": %s,\n"
            "  \"github_token\": ",
            s->first_run_done ? "true" : "false",
            s->scan_on_launch ? "true" : "false",
            s->install_overlays ? "true" : "false",
            s->install_sysmodules ? "true" : "false",
            s->install_payloads ? "true" : "false",
            s->auto_backup ? "true" : "false");
    json_write_escaped(f, s->github_token);
    fputs("\n}\n", f);
    fclose(f);
    return true;
}
