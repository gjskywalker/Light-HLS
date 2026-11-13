void dut(int input[32], int output[16])
{
    for (int i = 15; i >= 0; --i)
    {
#pragma HLS PIPELINE II=1
        output[15 - i] = input[31 - i];
    }
}
