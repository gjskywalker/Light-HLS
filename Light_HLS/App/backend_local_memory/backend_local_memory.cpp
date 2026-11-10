int backend_local_memory(int seed, int selector)
{
    int local[8];
    for (int index = 0; index < 8; ++index)
        local[index] = seed + index * 3;

    const int first = local[selector & 7];
    const int second = local[(selector + 3) & 7];
    return first + second;
}
