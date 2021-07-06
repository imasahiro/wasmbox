#define WASM 1
#ifdef WASM
#define MAIN _start
#else
#define MAIN factorial
#endif
int MAIN(int x) {
    if (x <= 1) {
        return 1;
    }
    return MAIN(x - 1) * x;
}

#ifndef WASM
#include <stdio.h>

int main(int argc, char* argv[])
{
    fprintf(stderr, "%d\n", MAIN(10));
    return 0;
}
#endif
