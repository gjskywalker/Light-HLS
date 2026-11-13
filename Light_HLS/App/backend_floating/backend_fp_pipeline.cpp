void dut(float a[16], float b[16], float out[16])
{
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        out[i] = __builtin_sqrtf(a[i] * a[i]) / (b[i] + 1.0f);
    }
}
