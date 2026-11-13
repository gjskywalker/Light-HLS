void dut(int input[32], int output[8])
{
    for (int i = 3; i < 11; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i - 3] = input[20 - i];
    }
}
