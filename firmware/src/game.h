#ifndef _GAME_H
#define _GAME_H

// The console's text adventure ("Lil Chompy and the World's Fair"), and the
// command shell around it. Everything the console receives goes through
// game_command().

// Printed when the console wakes.
void game_banner(void);

// One line of input, NUL-terminated, without the line ending. The game may
// modify the buffer.
void game_command(char *line);

// Physical badge events. These advance the game even while the console is
// asleep; any narration is simply dropped then.
void game_on_scan_verified(void);
void game_on_chompy_tap(void);

#endif
