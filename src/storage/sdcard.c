#include "sdcard.h"
#include "dvi.h"
#include "f_util.h"

static bool mounted = false;
static FATFS fs;

bool sd_mount(void) {
    if (mounted) return true;
    FRESULT fr = f_mount(&fs, "", 1);
    mounted = (fr == FR_OK);
    return mounted;
}

void sd_list_files(void) {
    if (!sd_mount()) {
        dvi_puts("?NO SD CARD\n");
        return;
    }
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, "/") != FR_OK) {
        dvi_puts("?DIR ERROR\n");
        return;
    }
    for (;;) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
        if (fno.fattrib & AM_DIR) continue;
        dvi_puts(fno.fname);
        dvi_putc('\n');
    }
    f_closedir(&dir);
}

bool sd_file_open_read(FIL *fil, const char *filename) {
    if (!sd_mount()) return false;
    return f_open(fil, filename, FA_READ) == FR_OK;
}

bool sd_file_open_write(FIL *fil, const char *filename) {
    if (!sd_mount()) return false;
    return f_open(fil, filename, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK;
}
