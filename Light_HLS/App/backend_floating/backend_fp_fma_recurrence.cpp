float dut(float a[16])
{
    float sum = 0.0f;
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        sum = __builtin_elementwise_fma(a[i], (float)0.5, sum);
    }
    return sum;
}
