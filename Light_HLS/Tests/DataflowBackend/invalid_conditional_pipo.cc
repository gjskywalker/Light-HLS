static void produce(int enable, int channel[4])
{
    if (enable)
    {
        for (int index = 0; index < 4; ++index)
            channel[index] = index;
    }
}

static void consume(const int channel[4], int output[4])
{
    for (int index = 0; index < 4; ++index)
        output[index] = channel[index];
}

void invalid_conditional_pipo(int enable, int output[4])
{
    int channel[4];

#pragma HLS DATAFLOW
    produce(enable, channel);
    consume(channel, output);
}
