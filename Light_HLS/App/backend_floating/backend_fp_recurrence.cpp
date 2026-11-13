float dut(float a[16])
{
    float sum = 0.0f;
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        sum = sum + a[i] * 0.5f;
    }
    return sum;
}
