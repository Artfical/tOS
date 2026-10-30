# A real terminal for programs, and Git that works through

## terminal | Terminal
- Programs that draw whole screens work in Terminal: colours (16, 256 and true colour, shown with the Terminal's 16), the cursor, a second screen they leave when they end, box drawing and UTF-8 text.
- The keys reach them as a terminal sends them: arrows, Home, End, Page Up and Down, Insert, Delete, F1 to F12, Alt with a key, Esc.
- Programs can put the Terminal in raw mode (termios), ask its size, read keys one at a time and get ^C. When a program ends, the shell's Terminal is as it was.
- ./program and /path/to/program run from the shell, and so do the commands npm install -g adds (they live in the bin folder of the package that installed them).
- Scripts that start with #!/usr/bin/env node (or any command tpkg knows) run when a program starts them.

## terminal | Git
- git clone, commit, log, status, mv and sparse checkouts work; Git no longer waits for a pager tOS does not have, and files it creates keep their executable bit.
- tpkg update tells you that a newer Git is out: sudo tpkg upgrade installs it.

## settings | Network
- Big HTTPS downloads no longer break in the middle (a memory-resize bug damaged data waiting to be read), and the window update that tells a server to go on is sent when a program has read what was waiting.
- Writing a big file (a 15 MB download in /tmp) no longer copies it over and over, and no longer fails when memory is tight.
