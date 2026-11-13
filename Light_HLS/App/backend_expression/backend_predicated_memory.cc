void dut(int input[16], int enabled[16], int output[16])
{
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        if (enabled[i])
            output[i] = input[i] + 3;
    }
}
