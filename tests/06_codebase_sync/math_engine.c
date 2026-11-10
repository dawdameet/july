#include "math_engine.h"

double vector_dot(const double *a, const double *b, size_t n) {
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) {
        sum += a[i] * b[i];
    }
    return sum;
}

void vector_add_scalar(double *a, double scalar, size_t n) {
    for (size_t i = 0; i < n; i++) {
        a[i] += scalar;
    }
}
