void dut(unsigned int input[128], unsigned int output[8])
{
    for (unsigned int i = 1; i < 9; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i - 1] = (input[128u >> i] >> (i - 1)) ^ (1u << i);
    }
}
