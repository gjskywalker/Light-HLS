void dut(int input[32], int output[16])
{
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = input[15 - i];
    }
}
