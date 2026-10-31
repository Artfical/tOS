# A C and C++ compiler in tOS

## terminal | Compiler
- New packages: gcc (GCC 15.3: gcc, g++, cpp), binutils (as, ld, ar, nm, objdump...), make (GNU make 4.4.1) and glibc-dev (the C library's headers and link files). sudo tpkg install gcc brings the other three.
- gcc hello.c -o hello && ./hello compiles and runs a C program inside tOS; g++ does C++ (programs carry their own libstdc++, so they run as they are); make builds projects with several files.

## terminal | Files
- Programs can seek backwards from the current position or from the end of a file (lseek with a negative offset). The assembler and linker need it, and so does much other software that writes files.
