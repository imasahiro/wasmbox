__attribute__((noinline)) int foo(int x) {
  return x + 100;
}

int _start(int v) {
  return foo(v);
}
