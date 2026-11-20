#include "dynamatic/Integration.h"
#include "gesummv_float_unroll.h"
#include <stdlib.h>

#define LOOP_BODY(i, j)                                                        \
  t_x = x[j];                                                                  \
  t_tmp = A[i][j] * t_x + t_tmp;                                               \
  t_y = B[i][j] * t_x + t_y;

#define INNER_LOOP(i)                                                          \
  t_tmp = 0;                                                                   \
  t_y = 0;                                                                     \
  for (j = 0; j < 30; j += 15) {                                               \
    LOOP_BODY(i, j)                                                            \
    LOOP_BODY(i, j + 1)                                                        \
    LOOP_BODY(i, j + 2)                                                        \
    LOOP_BODY(i, j + 3)                                                        \
    LOOP_BODY(i, j + 4)                                                        \
    LOOP_BODY(i, j + 5)                                                        \
    LOOP_BODY(i, j + 6)                                                        \
    LOOP_BODY(i, j + 7)                                                        \
    LOOP_BODY(i, j + 8)                                                        \
    LOOP_BODY(i, j + 9)                                                        \
    LOOP_BODY(i, j + 10)                                                       \
    LOOP_BODY(i, j + 11)                                                       \
    LOOP_BODY(i, j + 12)                                                       \
    LOOP_BODY(i, j + 13)                                                       \
    LOOP_BODY(i, j + 14)                                                       \
  }                                                                            \
  tmp[i] = t_tmp;                                                              \
  y[i] = alpha * t_tmp + beta * t_y;

void gesummv_float_unroll(in_float_t alpha, in_float_t beta,
                          in_float_t A[30][30], in_float_t B[30][30],
                          out_float_t tmp[30], out_float_t y[30],
                          in_float_t x[30]) {
  int i, j, k;
  float t_x;
  float t_tmp;
  float t_y;

  for (i = 0; i < 30; i += 5) {
    INNER_LOOP(i)
    INNER_LOOP(i + 1)
    INNER_LOOP(i + 2)
    INNER_LOOP(i + 3)
    INNER_LOOP(i + 4)
  }
}

#define AMOUNT_OF_TEST 1

int main(void) {
  in_float_t alpha;
  in_float_t beta;
  in_float_t A[30][30];
  in_float_t B[30][30];
  out_float_t tmp[30];
  out_float_t y[30];
  in_float_t x[30];

  alpha = 1;
  beta = 1;
  for (int j = 0; j < 30; ++j) {
    tmp[j] = 1;
    x[j] = rand() % 2;
    y[j] = rand() % 2;
    for (int k = 0; k < 30; ++k) {
      A[j][k] = rand() % 3;
      B[j][k] = rand() % 2;
    }
  }

  CALL_KERNEL(gesummv_float_unroll, alpha, beta, A, B, tmp, y, x);
  return 0;
}
