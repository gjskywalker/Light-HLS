void invalid_dimension(int input[8], int output[8])
{
#pragma HLS ARRAY_PARTITION variable=input type=cyclic factor=2 dim=2
    for (int i = 0; i < 8; ++i)
        output[i] = input[i];
}
