int dut(int delta)
{
    static int counter = 7;
    counter += delta;
    return counter;
}
