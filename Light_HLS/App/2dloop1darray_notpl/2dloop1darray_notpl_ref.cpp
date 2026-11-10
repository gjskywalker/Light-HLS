#include <iostream>

void f(int *A);

int main()
{
    int memory[58];
    for (int index = 0; index < 58; ++index)
        memory[index] = (index * 5 + 3) % 29;
    memory[4] = 8;

    f(memory);
    for (int index = 0; index < 58; ++index)
        std::cout << "RESULT A " << index << ' ' << memory[index] << '\n';
    return 0;
}
