#pragma once

#include <stdbool.h>
#include "ff.h"

// Mounts the SD card on first use (idempotent). Returns false if no card
// is present / mount failed - callers should treat that as "no SD card",
// not a fatal error, since the rest of the machine works fine without one.
bool sd_mount(void);

// Prints the root directory's file names to the DVI console.
void sd_list_files(void);

bool sd_file_open_read(FIL *fil, const char *filename);
bool sd_file_open_write(FIL *fil, const char *filename);
