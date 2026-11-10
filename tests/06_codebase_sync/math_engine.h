#ifndef MATH_ENGINE_H
#define MATH_ENGINE_H

#include <stddef.h>

double vector_dot(const double *a, const double *b, size_t n);
void vector_add_scalar(double *a, double scalar, size_t n);

#endif

