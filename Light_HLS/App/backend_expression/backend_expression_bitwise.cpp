void dut(unsigned int input[32], unsigned int output[16])
{
    for (unsigned int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        output[i] = input[(15u - i) & 7u] ^ ((i + 1u) | (31u - i));
    }
}
