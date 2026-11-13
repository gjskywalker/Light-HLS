void dut(int input[16], int indices[16], int output[16])
{
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        const int index = indices[i] - 2;
        if (index >= 0 && index < 16)
            output[i] = input[index];
    }
}
