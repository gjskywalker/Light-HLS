void f(int *A)
{
  int N = 100;
  int M = 50;
  // #pragma HLS PIPELINE II=1
  for (int j = 0; j < N; j++)
    // #pragma HLS PIPELINE II=1
    for (int i = 0; i < M; i++)
      // #pragma HLS PIPELINE II=1
      A[i + 8] = A[i] + 1;
}
