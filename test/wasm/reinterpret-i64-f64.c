#include <stdint.h>

union v {
    float f32;
    double f64;
    int32_t i32;
    int64_t i64;
};

double _start(int64_t x) {
    union v v;
    v.i64 = x;
    return v.f64;
}
