#ifndef SCAN_H
#define SCAN_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One homebrew .nro found on the SD card, with its embedded NACP metadata. */
typedef struct {
    char name[64];    /* NACP application title (language entry 0) */
    char author[64];  /* NACP author */
    char version[48]; /* NACP display version */
    char path[512];   /* full sdmc path to the .nro */
} ScannedApp;

/* Scan sdmc:/switch for homebrew: loose "*.nro" plus one subdirectory deep
 * (the sdmc:/switch/App/App.nro convention). Each .nro's NACP is parsed for
 * name/author/version. Returns a malloc'd array (caller frees) and sets *count;
 * returns NULL if nothing was found. */
ScannedApp *scan_switch(int *count);

#ifdef __cplusplus
}
#endif

#endif /* SCAN_H */
