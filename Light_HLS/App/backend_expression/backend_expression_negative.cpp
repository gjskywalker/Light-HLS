void dut(int input[32], int output[8])
{
    for (int i = -4; i < 4; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i + 4] = input[12 - i];
    }
}
