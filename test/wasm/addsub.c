__attribute__((noinline))
int addOne(int a) {
    return a + 1;
}

__attribute__((noinline))
int subOne(int a) {
    return a - 1;
}

int _start(int v) {
    if (v %2 == 0) {
        return addOne(v);
    } else {
        return subOne(v);
    }
}
