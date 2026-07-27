#pragma once

#include <JuceHeader.h>
#include <vector>
#include <cmath>
#include <algorithm>

#include "MorsDTW.h"
#include "MorsDebug.h"

//==============================================================================
// Result struct returned by mapOnsetsWithGaussian.
//==============================================================================
struct OnsetMapResult
{
    std::vector<double> morphedOnsets;
    int    dtwPathLength = 0;
    double dtwFinalCost  = 0.0;
    double dtwAvgCost    = 0.0;

    // Export payload — only populated when captureMatrices == true.
    std::vector<std::vector<double>>   dtwCostMatrix;
    std::vector<std::vector<double>>   dtwAccumMatrix;
    std::vector<std::vector<int64_t>>  dtwPath;
};

class OnsetMapper
{
public:
    //==========================================================================
    // Collision redistribution mode
    //==========================================================================
    enum class CollisionMode
    {
        None,
        Proportional,
        LocalDensity
    };

    //==========================================================================
    // Map reference onsets onto source timeline using:
    // DTW -> collision resolution -> Gaussian weighting -> alpha blend
    //==========================================================================
    static OnsetMapResult mapOnsetsWithGaussian(const std::vector<double>& sourceOnsets,
                                                const std::vector<double>& referenceOnsets,
                                                float  alpha             = 1.0f,
                                                float  sigmaScale        = 0.25f,
                                                float  sigmaEps          = 1.0f,
                                                double guard             = 0.125,
                                                float  displacementBoost = 0.5f,
                                                float  drawRange         = 0.5f,
                                                bool   captureMatrices   = false,
                                                CollisionMode collisionMode = CollisionMode::LocalDensity,
                                                float  spreadFactor      = 0.5f,
                                                double minSpacing        = 0.125);

private:
    //==========================================================================
    // DTW-based initial mapping
    //==========================================================================
    static std::vector<double> buildNewOnsets(const std::vector<double>& sourceOnsets,
                                              const std::vector<double>& referenceOnsets,
                                              const std::vector<std::vector<int64_t>>& path);

    //==========================================================================
    // Python-style collision handling methods
    //==========================================================================
    static std::vector<double> resolveTransientCollisions(const std::vector<double>& oldPositions,
                                                          const std::vector<double>& referencePositions,
                                                          float spreadFactor,
                                                          double guard);

    static std::vector<double> resolveWithLocalDensity(const std::vector<double>& oldPositions,
                                                       const std::vector<double>& referencePositions,
                                                       double minSpacing,
                                                       double guard);

    //==========================================================================
    // Gaussian weighted redistribution
    //==========================================================================
    static std::vector<double> gaussianWeightedMapping(const std::vector<double>& ref,
                                                       const std::vector<double>& target,
                                                       float sigmaScale,
                                                       float sigmaEps,
                                                       double guard,
                                                       float displacementBoost,
                                                       float drawRange);

    //==========================================================================
    // Enforce monotonicity
    //==========================================================================
    static void enforceMonotonicity(std::vector<double>& onsets,
                                    double guard);
};
