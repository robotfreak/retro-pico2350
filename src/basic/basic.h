#pragma once

// A small line-numbered BASIC interpreter (Tiny-BASIC-ish subset) that
// drives the DVI text console and PS/2 keyboard. Variables are 26 global
// floats (A-Z); no strings variables or arrays in this first version.
//
// Supported statements: LET, PRINT, INPUT, IF/THEN, FOR/NEXT/STEP, GOTO,
// GOSUB/RETURN, REM, END, CLS, COLOR, LIST, RUN, NEW.
// Functions in expressions: ABS(x), INT(x), SGN(x), RND(x).

void basic_init(void);

// Runs the interactive line editor / REPL forever. Never returns.
void basic_repl(void);
