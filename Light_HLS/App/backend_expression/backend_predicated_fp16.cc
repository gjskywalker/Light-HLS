void dut(_Float16 input[16], int enabled[16], _Float16 output[16])
{
    for (int i = 0; i < 16; ++i)
    {
#pragma HLS PIPELINE II=1
        if (enabled[i])
            output[i] = input[i] * (_Float16)1.5 + (_Float16)0.25;
    }
}
