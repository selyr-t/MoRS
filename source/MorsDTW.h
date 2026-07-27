#pragma once

#include <vector>
#include <cmath>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <limits>
#include "MorsDebug.h"

class DTW
{
public:
    static std::vector<std::vector<double>> costMatrix(const std::vector<double>& X,
                                                       const std::vector<double>& Y);

    static std::vector<std::vector<double>> accumCostMatrix(const std::vector<std::vector<double>>& C);

    static std::vector<std::vector<int64_t>> optimalWarpingPath(const std::vector<std::vector<double>>& D);
};
