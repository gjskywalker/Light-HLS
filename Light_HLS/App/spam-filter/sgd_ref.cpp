#include <iostream>

constexpr int NumFeatures = 800;

void compute(int thetaLocal[NumFeatures], int trainingInstance[NumFeatures],
             int gradient[NumFeatures], int probability, int step);

int main()
{
    int thetaLocal[NumFeatures];
    int trainingInstance[NumFeatures];
    int gradient[NumFeatures];
    for (int index = 0; index < NumFeatures; ++index)
    {
        thetaLocal[index] = index % 23;
        trainingInstance[index] = (index * 3 + 1) % 11;
        gradient[index] = 0;
    }

    compute(thetaLocal, trainingInstance, gradient, 3, 2);
    for (int index = 0; index < NumFeatures; ++index)
        std::cout << "RESULT gradient " << index << ' ' << gradient[index]
                  << '\n';
    for (int index = 0; index < NumFeatures; ++index)
        std::cout << "RESULT theta_local " << index << ' '
                  << thetaLocal[index] << '\n';
    return 0;
}
