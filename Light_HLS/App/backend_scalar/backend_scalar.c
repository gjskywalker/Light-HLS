int backend_scalar(int a, int b, int mode)
{
    int accumulator = (mode & 1) ? a : b;
    for (int index = 0; index < 4; ++index)
        accumulator = accumulator * 3 + (a ^ index);

    if (mode & 2)
        accumulator -= b;
    else
        accumulator += b;
    return accumulator;
}
