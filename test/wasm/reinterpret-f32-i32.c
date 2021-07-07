#include <stdint.h>

union v {
  float f32;
  double f64;
  int32_t i32;
  int64_t i64;
};

int32_t _start(float x) {
  union v v;
  v.f32 = x;
  return v.i32;
}
