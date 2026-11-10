#include <math.h>

#define N 30
#define DATA_TYPE float

void kernel_jacobi_2d(int tsteps, int n, DATA_TYPE A[N][N], DATA_TYPE B[N][N])
{
#pragma HLS ARRAY_PARTITION variable=A type=cyclic factor=2 dim=2
#pragma HLS ARRAY_PARTITION variable=B type=block factor=2 dim=2
    int i, j;

    for (i = 1; i < N - 1; i++)
    {
        for (j = 1; j < N - 1; j++)
        {
#pragma HLS PIPELINE II=1
#pragma HLS UNROLL factor=2
            B[i][j] =
                (DATA_TYPE)(0.2) *
                (A[i][j] + A[i][j - 1] + A[i][1 + j] + A[1 + i][j] + A[i - 1][j]);
        }
    }

    for (i = 1; i < N - 1; i++)
        for (j = 1; j < N - 1; j++)
            A[i][j] =
                (DATA_TYPE)(0.2) *
                (B[i][j] + B[i][j - 1] + B[i][1 + j] + B[1 + i][j] + B[i - 1][j]);
}
