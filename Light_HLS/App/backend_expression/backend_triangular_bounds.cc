void dut(int input[64], int output[64])
{
    for (int row = 0; row < 8; ++row)
        for (int col = row; col < 8; ++col)
            output[8 * row + col] = input[8 * row + col] + row;
}
