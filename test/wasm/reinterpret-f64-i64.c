#include <stdint.h>

union v {
    float f32;
    double f64;
    int32_t i32;
    int64_t i64;
};

int64_t _start(double x) {
    union v v;
    v.f64 = x;
    return v.i64;
}
