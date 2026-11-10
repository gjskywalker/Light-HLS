void backend_memory_depth(int *memory)
{
    for (int index = 0; index < 4; ++index)
        memory[index] = memory[index] + index + 1;
}
