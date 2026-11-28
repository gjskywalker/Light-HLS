void triangular_trip(int data[900])
{
    for (int row = 0; row < 30; ++row)
        for (int col = 0; col <= row; ++col)
        {
#pragma HLS PIPELINE II=1
            data[row * 30 + col] += col;
        }
}
