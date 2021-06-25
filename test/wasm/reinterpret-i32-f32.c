#include <stdint.h>

union v {
    float f32;
    double f64;
    int32_t i32;
    int64_t i64;
};

float _start(int32_t x) {
    union v v;
    v.i32 = x;
    return v.f32;
}
