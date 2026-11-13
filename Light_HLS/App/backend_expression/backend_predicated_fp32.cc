void dut(float input[16], int enabled[16], float output[16])
{
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        if (enabled[i])
            output[i] = input[i] * 1.5f + 0.25f;
    }
}
