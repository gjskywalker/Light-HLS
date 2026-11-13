static const int table[8] = {19, -3, 7, 41, 0, 8, -15, 64};
void dut(int indices[16], int output[16])
{
    for (int i = 0; i < 16; ++i)
        output[i] = table[indices[i] & 7];
}
