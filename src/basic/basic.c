#include "basic.h"
#include "dvi.h"
#include "ps2kbd.h"
#include "net.h"
#include "sdcard.h"
#include "ff.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ----------------------------------------------------------------------------
// Program storage

typedef struct {
    int num;
    char *text;
} ProgLine;

#define MAX_LINES 400
#define MAX_INPUT 128

static ProgLine prog[MAX_LINES];
static int prog_count = 0;

static float vars[26];

#define FOR_STACK_DEPTH 8
typedef struct {
    int var;           // index 0-25 into vars[]
    float limit;
    float step;
    int body_line_idx; // program index right after FOR, where the loop body starts
} ForFrame;
static ForFrame for_stack[FOR_STACK_DEPTH];
static int for_sp = 0;

#define GOSUB_STACK_DEPTH 16
static int gosub_stack[GOSUB_STACK_DEPTH];
static int gosub_sp = 0;

static uint32_t rng_state = 0x1234abcdu;

static jmp_buf error_jmp;
static char error_msg[48];
#define ERROR(msg) do { snprintf(error_msg, sizeof(error_msg), "%s", (msg)); longjmp(error_jmp, 1); } while (0)

// ----------------------------------------------------------------------------
// Line editor (also used by INPUT)

static void read_line(char *buf, int maxlen) {
    int len = 0;
    for (;;) {
        char c = kbd_getc_blocking();
        if (c == '\n' || c == '\r') {
            dvi_putc('\n');
            buf[len] = '\0';
            return;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                dvi_putc('\b');
            }
        } else if (c >= 32 && c < 127 && len < maxlen - 1) {
            buf[len++] = c;
            dvi_putc(c);
        }
    }
}

// ----------------------------------------------------------------------------
// Tokenizer helpers

static void skip_spaces(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}

static bool match_keyword(const char **p, const char *kw) {
    const char *s = *p;
    size_t n = strlen(kw);
    for (size_t i = 0; i < n; i++) {
        if (toupper((unsigned char)s[i]) != kw[i]) return false;
    }
    if (isalnum((unsigned char)s[n])) return false; // "FORX" must not match "FOR"
    *p = s + n;
    skip_spaces(p);
    return true;
}

static bool peek_keyword(const char *p, const char *kw) {
    return match_keyword(&p, kw);
}

static int var_index(char c) {
    c = (char)toupper((unsigned char)c);
    if (c < 'A' || c > 'Z') return -1;
    return c - 'A';
}

// ----------------------------------------------------------------------------
// Expression parser: expr -> term ((+|-) term)*  ; term -> factor ((*|/) factor)*
// factor -> number | var | ABS/INT/SGN/RND(expr) | (expr) | -factor | +factor

static float parse_expr(const char **p);

static float parse_factor(const char **p) {
    skip_spaces(p);
    if (**p == '(') {
        (*p)++;
        float v = parse_expr(p);
        skip_spaces(p);
        if (**p != ')') ERROR("?SYNTAX ERROR");
        (*p)++;
        return v;
    }
    if (**p == '-') { (*p)++; return -parse_factor(p); }
    if (**p == '+') { (*p)++; return parse_factor(p); }

    if (isdigit((unsigned char)**p) || **p == '.') {
        char *end;
        float v = strtof(*p, &end);
        if (end == *p) ERROR("?SYNTAX ERROR");
        *p = end;
        return v;
    }
    if (peek_keyword(*p, "ABS")) {
        match_keyword(p, "ABS");
        if (**p != '(') ERROR("?SYNTAX ERROR");
        (*p)++;
        float v = parse_expr(p);
        skip_spaces(p);
        if (**p != ')') ERROR("?SYNTAX ERROR");
        (*p)++;
        return v < 0 ? -v : v;
    }
    if (peek_keyword(*p, "INT")) {
        match_keyword(p, "INT");
        if (**p != '(') ERROR("?SYNTAX ERROR");
        (*p)++;
        float v = parse_expr(p);
        skip_spaces(p);
        if (**p != ')') ERROR("?SYNTAX ERROR");
        (*p)++;
        return (float)(long)v;
    }
    if (peek_keyword(*p, "SGN")) {
        match_keyword(p, "SGN");
        if (**p != '(') ERROR("?SYNTAX ERROR");
        (*p)++;
        float v = parse_expr(p);
        skip_spaces(p);
        if (**p != ')') ERROR("?SYNTAX ERROR");
        (*p)++;
        return v > 0 ? 1.0f : (v < 0 ? -1.0f : 0.0f);
    }
    if (peek_keyword(*p, "RND")) {
        match_keyword(p, "RND");
        if (**p != '(') ERROR("?SYNTAX ERROR");
        (*p)++;
        float v = parse_expr(p);
        skip_spaces(p);
        if (**p != ')') ERROR("?SYNTAX ERROR");
        (*p)++;
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 17;
        rng_state ^= rng_state << 5;
        float r = (float)(rng_state & 0xFFFFFFu) / (float)0x1000000;
        return r * v;
    }
    int vi = var_index(**p);
    if (vi >= 0 && !isalnum((unsigned char)(*p)[1])) {
        (*p)++;
        return vars[vi];
    }
    ERROR("?SYNTAX ERROR");
    return 0; // unreachable
}

static float parse_term(const char **p) {
    float v = parse_factor(p);
    for (;;) {
        skip_spaces(p);
        if (**p == '*') { (*p)++; v *= parse_factor(p); }
        else if (**p == '/') { (*p)++; v /= parse_factor(p); }
        else break;
    }
    return v;
}

static float parse_expr(const char **p) {
    float v = parse_term(p);
    for (;;) {
        skip_spaces(p);
        if (**p == '+') { (*p)++; v += parse_term(p); }
        else if (**p == '-') { (*p)++; v -= parse_term(p); }
        else break;
    }
    return v;
}

// relop: 0=none 1=< 2=> 3=<= 4=>= 5== 6=<>
static int parse_relop(const char **p) {
    skip_spaces(p);
    if (**p == '<') {
        (*p)++;
        if (**p == '=') { (*p)++; return 3; }
        if (**p == '>') { (*p)++; return 6; }
        return 1;
    }
    if (**p == '>') {
        (*p)++;
        if (**p == '=') { (*p)++; return 4; }
        return 2;
    }
    if (**p == '=') { (*p)++; return 5; }
    return 0;
}

static bool eval_relop(int rel, float a, float b) {
    switch (rel) {
    case 1: return a < b;
    case 2: return a > b;
    case 3: return a <= b;
    case 4: return a >= b;
    case 5: return a == b;
    case 6: return a != b;
    default: return false;
    }
}

// ----------------------------------------------------------------------------
// Program line table helpers

static int find_line_idx(int num) {
    for (int i = 0; i < prog_count; i++) {
        if (prog[i].num == num) return i;
    }
    return -1;
}

static int find_line_idx_or_error(int num) {
    int idx = find_line_idx(num);
    if (idx < 0) ERROR("?UNDEFINED LINE");
    return idx;
}

static void program_set_line(int num, const char *text) {
    int idx = find_line_idx(num);
    if (*text == '\0') {
        if (idx >= 0) {
            free(prog[idx].text);
            memmove(&prog[idx], &prog[idx + 1], (size_t)(prog_count - idx - 1) * sizeof(ProgLine));
            prog_count--;
        }
        return;
    }
    char *dup = malloc(strlen(text) + 1);
    if (!dup) {
        dvi_puts("?OUT OF MEMORY\n");
        return;
    }
    strcpy(dup, text);
    if (idx >= 0) {
        free(prog[idx].text);
        prog[idx].text = dup;
        return;
    }
    if (prog_count >= MAX_LINES) {
        free(dup);
        dvi_puts("?PROGRAM TOO LONG\n");
        return;
    }
    int pos = 0;
    while (pos < prog_count && prog[pos].num < num) pos++;
    memmove(&prog[pos + 1], &prog[pos], (size_t)(prog_count - pos) * sizeof(ProgLine));
    prog[pos].num = num;
    prog[pos].text = dup;
    prog_count++;
}

static void program_new(void) {
    for (int i = 0; i < prog_count; i++) free(prog[i].text);
    prog_count = 0;
    for_sp = 0;
    gosub_sp = 0;
}

static void program_list(void) {
    char numbuf[16];
    for (int i = 0; i < prog_count; i++) {
        snprintf(numbuf, sizeof(numbuf), "%d ", prog[i].num);
        dvi_puts(numbuf);
        dvi_puts(prog[i].text);
        dvi_putc('\n');
    }
}

// ----------------------------------------------------------------------------
// SD card LOAD/SAVE (program text, one "<num> <text>" line per program line)

static bool do_save_file(const char *filename) {
    if (!sd_mount()) {
        dvi_puts("?NO SD CARD\n");
        return false;
    }
    FIL fil;
    if (!sd_file_open_write(&fil, filename)) {
        dvi_puts("?SAVE FAILED\n");
        return false;
    }
    char line[MAX_INPUT];
    for (int i = 0; i < prog_count; i++) {
        int n = snprintf(line, sizeof(line), "%d %s\n", prog[i].num, prog[i].text);
        UINT bw;
        f_write(&fil, line, (UINT)n, &bw);
    }
    f_close(&fil);
    return true;
}

static bool do_load_file(const char *filename) {
    if (!sd_mount()) {
        dvi_puts("?NO SD CARD\n");
        return false;
    }
    FIL fil;
    if (!sd_file_open_read(&fil, filename)) {
        dvi_puts("?LOAD FAILED\n");
        return false;
    }
    program_new();
    char line[MAX_INPUT];
    while (f_gets(line, sizeof(line), &fil)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        const char *lp = line;
        skip_spaces(&lp);
        if (!isdigit((unsigned char)*lp)) continue;
        int num = atoi(lp);
        while (isdigit((unsigned char)*lp)) lp++;
        skip_spaces(&lp);
        program_set_line(num, lp);
    }
    f_close(&fil);
    return true;
}

// ----------------------------------------------------------------------------
// Statement execution
//
// A statement either falls through (ACT_NONE, *p left just past the
// statement), requests a jump to an already-resolved program index
// (ACT_JUMP, *out_jump_idx set), or stops the program (ACT_END).
// GOTO/GOSUB/RETURN/FOR/NEXT are only legal inside a running program;
// in immediate mode they raise ?ILLEGAL IN IMMEDIATE MODE.

typedef enum { ACT_NONE, ACT_JUMP, ACT_END } Action;

static void print_number(float v) {
    char buf[32];
    if (v == (float)(long)v && v > -1e7f && v < 1e7f) {
        snprintf(buf, sizeof(buf), "%ld", (long)v);
    } else {
        snprintf(buf, sizeof(buf), "%g", v);
    }
    dvi_puts(buf);
}

static void skip_to_colon_or_eol(const char **p) {
    while (**p && **p != ':') (*p)++;
}

// Parses a "quoted string" argument (used by WIFI/TELNET/LOAD/SAVE).
static void parse_string_literal(const char **p, char *out, int outmax) {
    skip_spaces(p);
    if (**p != '"') ERROR("?SYNTAX ERROR");
    (*p)++;
    int n = 0;
    while (**p && **p != '"') {
        if (n < outmax - 1) out[n++] = *(*p);
        (*p)++;
    }
    if (**p == '"') (*p)++;
    out[n] = '\0';
}

static Action do_print(const char **p) {
    skip_spaces(p);
    bool last_was_sep = false;
    if (**p == '\0' || **p == ':') {
        dvi_putc('\n');
        return ACT_NONE;
    }
    for (;;) {
        skip_spaces(p);
        last_was_sep = false;
        if (**p == '"') {
            (*p)++;
            while (**p && **p != '"') dvi_putc(*(*p)++);
            if (**p == '"') (*p)++;
        } else if (**p && **p != ',' && **p != ';' && **p != ':') {
            float v = parse_expr(p);
            print_number(v);
        }
        skip_spaces(p);
        if (**p == ';') {
            (*p)++;
            last_was_sep = true;
        } else if (**p == ',') {
            (*p)++;
            last_was_sep = true;
            int col = dvi_get_col();
            int next_zone = ((col / 10) + 1) * 10;
            while (col < next_zone && col < DVI_COLS) { dvi_putc(' '); col++; }
        } else {
            break;
        }
        skip_spaces(p);
        if (**p == '\0' || **p == ':') break;
    }
    if (!last_was_sep) dvi_putc('\n');
    return ACT_NONE;
}

static Action do_input(const char **p) {
    skip_spaces(p);
    if (**p == '"') {
        (*p)++;
        while (**p && **p != '"') dvi_putc(*(*p)++);
        if (**p == '"') (*p)++;
        skip_spaces(p);
        if (**p == ';' || **p == ',') (*p)++;
    } else {
        dvi_puts("? ");
    }
    skip_spaces(p);
    int vi = var_index(**p);
    if (vi < 0) ERROR("?SYNTAX ERROR");
    (*p)++;
    char buf[MAX_INPUT];
    read_line(buf, sizeof(buf));
    vars[vi] = strtof(buf, NULL);
    return ACT_NONE;
}

static Action exec_one_statement(const char **p, bool in_program, int current_line_idx, int *out_jump_idx) {
    skip_spaces(p);
    if (**p == '\0' || **p == ':') return ACT_NONE;

    if (match_keyword(p, "REM")) {
        while (**p) (*p)++;
        return ACT_NONE;
    }
    if (match_keyword(p, "CLS")) {
        dvi_clear();
        return ACT_NONE;
    }
    if (match_keyword(p, "COLOR")) {
        float fg = parse_expr(p);
        float bg = 0;
        skip_spaces(p);
        if (**p == ',') { (*p)++; bg = parse_expr(p); }
        dvi_set_colors((uint8_t)fg, (uint8_t)bg);
        return ACT_NONE;
    }
    if (match_keyword(p, "PRINT")) return do_print(p);
    if (match_keyword(p, "INPUT")) return do_input(p);

    if (match_keyword(p, "DIR") || match_keyword(p, "FILES")) {
        sd_list_files();
        return ACT_NONE;
    }
    if (match_keyword(p, "LOAD")) {
        char fname[64];
        parse_string_literal(p, fname, sizeof(fname));
        do_load_file(fname);
        return ACT_NONE;
    }
    if (match_keyword(p, "SAVE")) {
        char fname[64];
        parse_string_literal(p, fname, sizeof(fname));
        do_save_file(fname);
        return ACT_NONE;
    }
    if (match_keyword(p, "WIFI")) {
        char ssid[64], pass[64];
        parse_string_literal(p, ssid, sizeof(ssid));
        skip_spaces(p);
        if (**p != ',') ERROR("?SYNTAX ERROR");
        (*p)++;
        parse_string_literal(p, pass, sizeof(pass));
        dvi_puts("CONNECTING TO WIFI...\n");
        if (net_wifi_connect(ssid, pass, 30000)) {
            dvi_puts("WIFI CONNECTED\n");
        } else {
            dvi_puts("?WIFI CONNECT FAILED\n");
        }
        return ACT_NONE;
    }
    if (match_keyword(p, "TELNET")) {
        char host[64];
        parse_string_literal(p, host, sizeof(host));
        skip_spaces(p);
        int port = 23;
        if (**p == ',') { (*p)++; port = (int)parse_expr(p); }
        net_telnet_session(host, (uint16_t)port);
        return ACT_NONE;
    }

    if (match_keyword(p, "LET")) {
        skip_spaces(p);
        int vi = var_index(**p);
        if (vi < 0) ERROR("?SYNTAX ERROR");
        (*p)++;
        skip_spaces(p);
        if (**p != '=') ERROR("?SYNTAX ERROR");
        (*p)++;
        vars[vi] = parse_expr(p);
        return ACT_NONE;
    }

    if (match_keyword(p, "GOTO")) {
        if (!in_program) ERROR("?ILLEGAL IN IMMEDIATE MODE");
        int n = (int)parse_expr(p);
        *out_jump_idx = find_line_idx_or_error(n);
        return ACT_JUMP;
    }
    if (match_keyword(p, "GOSUB")) {
        if (!in_program) ERROR("?ILLEGAL IN IMMEDIATE MODE");
        int n = (int)parse_expr(p);
        if (gosub_sp >= GOSUB_STACK_DEPTH) ERROR("?GOSUB TOO DEEP");
        gosub_stack[gosub_sp++] = current_line_idx + 1;
        *out_jump_idx = find_line_idx_or_error(n);
        return ACT_JUMP;
    }
    if (match_keyword(p, "RETURN")) {
        if (!in_program) ERROR("?ILLEGAL IN IMMEDIATE MODE");
        if (gosub_sp == 0) ERROR("?RETURN WITHOUT GOSUB");
        int ret = gosub_stack[--gosub_sp];
        if (ret >= prog_count) return ACT_END;
        *out_jump_idx = ret;
        return ACT_JUMP;
    }
    if (match_keyword(p, "FOR")) {
        if (!in_program) ERROR("?ILLEGAL IN IMMEDIATE MODE");
        skip_spaces(p);
        int vi = var_index(**p);
        if (vi < 0) ERROR("?SYNTAX ERROR");
        (*p)++;
        skip_spaces(p);
        if (**p != '=') ERROR("?SYNTAX ERROR");
        (*p)++;
        float start = parse_expr(p);
        skip_spaces(p);
        if (!match_keyword(p, "TO")) ERROR("?SYNTAX ERROR");
        float limit = parse_expr(p);
        float step = 1.0f;
        skip_spaces(p);
        if (match_keyword(p, "STEP")) step = parse_expr(p);
        if (for_sp >= FOR_STACK_DEPTH) ERROR("?FOR TOO DEEP");
        vars[vi] = start;
        for_stack[for_sp].var = vi;
        for_stack[for_sp].limit = limit;
        for_stack[for_sp].step = step;
        for_stack[for_sp].body_line_idx = current_line_idx + 1;
        for_sp++;
        return ACT_NONE;
    }
    if (match_keyword(p, "NEXT")) {
        if (!in_program) ERROR("?ILLEGAL IN IMMEDIATE MODE");
        skip_spaces(p);
        int vi = -1;
        if (isalpha((unsigned char)**p)) {
            vi = var_index(**p);
            (*p)++;
        }
        if (for_sp == 0) ERROR("?NEXT WITHOUT FOR");
        ForFrame *f = &for_stack[for_sp - 1];
        if (vi >= 0 && vi != f->var) ERROR("?NEXT MISMATCH");
        vars[f->var] += f->step;
        bool cont = (f->step >= 0) ? (vars[f->var] <= f->limit) : (vars[f->var] >= f->limit);
        if (cont) {
            *out_jump_idx = f->body_line_idx;
            return ACT_JUMP;
        }
        for_sp--;
        return ACT_NONE;
    }
    if (match_keyword(p, "IF")) {
        float left = parse_expr(p);
        int rel = parse_relop(p);
        if (rel == 0) ERROR("?SYNTAX ERROR");
        float right = parse_expr(p);
        bool cond = eval_relop(rel, left, right);
        skip_spaces(p);
        if (!match_keyword(p, "THEN")) ERROR("?SYNTAX ERROR");
        skip_spaces(p);
        if (!cond) {
            skip_to_colon_or_eol(p);
            return ACT_NONE;
        }
        if (isdigit((unsigned char)**p)) {
            if (!in_program) ERROR("?ILLEGAL IN IMMEDIATE MODE");
            int n = (int)parse_expr(p);
            *out_jump_idx = find_line_idx_or_error(n);
            return ACT_JUMP;
        }
        return exec_one_statement(p, in_program, current_line_idx, out_jump_idx);
    }
    if (match_keyword(p, "END") || match_keyword(p, "STOP")) {
        return ACT_END;
    }

    ERROR("?SYNTAX ERROR");
    return ACT_NONE; // unreachable
}

// Executes a whole program line (possibly several ':'-separated statements).
static Action exec_line(int line_idx, int *out_jump_idx) {
    const char *p = prog[line_idx].text;
    for (;;) {
        skip_spaces(&p);
        if (*p == ':') { p++; continue; }
        if (*p == '\0') return ACT_NONE;
        Action a = exec_one_statement(&p, true, line_idx, out_jump_idx);
        if (a != ACT_NONE) return a;
    }
}

// ----------------------------------------------------------------------------
// RUN

static void run_program(void) {
    if (prog_count == 0) return;
    for_sp = 0;
    gosub_sp = 0;
    int line_idx = 0;
    while (line_idx < prog_count) {
        if (kbd_getc_nonblock() == 3) { // Ctrl-C
            dvi_puts("\n?BREAK\n");
            return;
        }
        int jump_idx = -1;
        Action a;
        if (setjmp(error_jmp) == 0) {
            a = exec_line(line_idx, &jump_idx);
        } else {
            char numbuf[16];
            snprintf(numbuf, sizeof(numbuf), "%d", prog[line_idx].num);
            dvi_puts(error_msg);
            dvi_puts(" IN LINE ");
            dvi_puts(numbuf);
            dvi_putc('\n');
            return;
        }
        if (a == ACT_END) return;
        if (a == ACT_JUMP) line_idx = jump_idx;
        else line_idx++;
    }
}

// ----------------------------------------------------------------------------
// REPL

void basic_init(void) {
    memset(vars, 0, sizeof(vars));
    program_new();
    dvi_puts("RETRO-PICO BASIC\n");
    dvi_puts("READY\n");
}

void basic_repl(void) {
    char line[MAX_INPUT];
    for (;;) {
        dvi_putc('>');
        read_line(line, sizeof(line));

        const char *p = line;
        skip_spaces(&p);
        if (*p == '\0') continue;

        if (isdigit((unsigned char)*p)) {
            int num = atoi(p);
            while (isdigit((unsigned char)*p)) p++;
            skip_spaces(&p);
            program_set_line(num, p);
            continue;
        }

        if (match_keyword(&p, "RUN")) {
            // run_program() installs its own setjmp target per line and
            // reports errors internally, always returning normally.
            run_program();
            continue;
        }
        if (match_keyword(&p, "LIST")) {
            program_list();
            continue;
        }
        if (match_keyword(&p, "NEW")) {
            program_new();
            continue;
        }

        // Immediate-mode statement(s)
        if (setjmp(error_jmp) == 0) {
            const char *q = p;
            for (;;) {
                skip_spaces(&q);
                if (*q == ':') { q++; continue; }
                if (*q == '\0') break;
                int dummy_jump;
                exec_one_statement(&q, false, -1, &dummy_jump);
            }
        } else {
            dvi_puts(error_msg);
            dvi_putc('\n');
        }
    }
}
