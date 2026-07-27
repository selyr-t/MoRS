#include "MorsOnsetMapper.h"
#include <cmath>
#include <limits>
#include <algorithm>


//==============================================================================
// DTW PATH → MAPPED ONSETS
//==============================================================================
// Given a DTW warping path of index pairs produce a vector newOnsets where
// each source onset gets assigned a reference time
std::vector<double> OnsetMapper::buildNewOnsets(
        const std::vector<double>& sourceOnsets,
        const std::vector<double>& referenceOnsets,
        const std::vector<std::vector<int64_t>>& path)
{
    const size_t N = sourceOnsets.size();
    std::vector<double> newOnsets(N, -1.0);

    for (const auto& pair : path)
    {
        const size_t srcIdx = static_cast<size_t>(pair[0]);
        const size_t refIdx = static_cast<size_t>(pair[1]);

        if (srcIdx < N && refIdx < referenceOnsets.size())
            newOnsets[srcIdx] = referenceOnsets[refIdx];
    }

    std::vector<size_t> mapped;
    for (size_t i = 0; i < N; ++i)
        if (newOnsets[i] >= 0.0)
            mapped.push_back(i);

    if (mapped.empty())
        return sourceOnsets;

    if (mapped.size() == 1)
    {
        const double offset = newOnsets[mapped[0]] - sourceOnsets[mapped[0]];
        for (size_t i = 0; i < N; ++i)
            newOnsets[i] = sourceOnsets[i] + offset;

        return newOnsets;
    }

    for (size_t i = 0; i < N; ++i)
    {
        if (newOnsets[i] >= 0.0)
            continue;

        if (i <= mapped.front())
        {
            newOnsets[i] = newOnsets[mapped.front()];
            continue;
        }

        if (i >= mapped.back())
        {
            newOnsets[i] = newOnsets[mapped.back()];
            continue;
        }

        size_t prev = mapped.front();
        size_t next = mapped.back();

        for (size_t j = 0; j + 1 < mapped.size(); ++j)
        {
            if (mapped[j] <= i && i <= mapped[j + 1])
            {
                prev = mapped[j];
                next = mapped[j + 1];
                break;
            }
        }

        const double t = static_cast<double>(i - prev) / static_cast<double>(next - prev);
        newOnsets[i] = (1.0 - t) * newOnsets[prev] + t * newOnsets[next];
    }

    return newOnsets;
}

//==============================================================================
// ENFORCE MONOTONICITY
//==============================================================================
// Ensure onsets are strictly increasing by at least "guard" seconds.
void OnsetMapper::enforceMonotonicity(std::vector<double>& onsets, double guard)
{
    if (onsets.size() < 2) return;

    // Forward pass: enforce lower bound
    for (size_t i = 1; i < onsets.size(); ++i)
        if (onsets[i] <= onsets[i - 1] + guard)
            onsets[i]  = onsets[i - 1] + guard;

    // Backward pass: enforce upper bound on previous elements
    for (size_t i = onsets.size() - 1; i > 0; --i)
        if (onsets[i - 1] >= onsets[i] - guard)
            onsets[i - 1]  = onsets[i] - guard;
}

//==============================================================================
// GAUSSIAN WEIGHTED DISTRIBUTION
//==============================================================================
// Compute a weighted average of the proposed new times.
std::vector<double> OnsetMapper::gaussianWeightedMapping(const std::vector<double>& ref,
                                                         const std::vector<double>& target,
                                                         float sigmaScale,
                                                         float sigmaEps,
                                                         double guard,
                                                         float displacementBoost,
                                                         float drawRange)
{
    // Work on matched prefix size
    const size_t m = std::min(ref.size(), target.size());
    if (m < 2) return ref;

    // Compute inter-onset intervals (IOI) from ref
    // refIOI[i] ~ ref[i+1] - ref[i]
    std::vector<double> refIOI(m);
    for (size_t i = 0; i < m - 1; ++i)
        refIOI[i] = ref[i + 1] - ref[i];

    // For last entry, repeat previous IOI (avoid undefined)
    refIOI[m - 1] = std::max(refIOI[m - 2], 1e-6);

    // Sigma per i: proportional to local IOI (so kernel width adapts to density)
    // sigma[i] = max(sigmaScale * refIOI[i], sigmaEps)
    std::vector<double> sigma(m);
    for (size_t i = 0; i < m; ++i)
        sigma[i] = std::max(sigmaScale * refIOI[i],
                            static_cast<double>(sigmaEps));

    // Accumulators for weighted sum (numerator) and weight sum (denominator)
    std::vector<double> acc(m, 0.0);
    std::vector<double> wsum(m, 0.0);

    // For each i: define proposed offset and distribute it to every j
    for (size_t i = 0; i < m; ++i)
    {
        // Proposed shift at i (how far target wants to move relative to ref)
        const double offset = target[i] - ref[i];

        for (size_t j = 0; j < m; ++j)
        {
            // Distance in time domain (using ref's timeline as coordinate system)
            const double d = ref[j] - ref[i];

            // Gaussian kernel weight:
            // w = exp( - d^2 / (2 sigma[i]^2) )
            const double w = std::exp(-(d * d) /
                                      (2.0 * sigma[i] * sigma[i]));

            // Accumulate plain Gaussian-weighted proposal (ref[j] + offset).
            // NOTE: displacementBoost is NOT applied here — doing so would
            // multiply the boost by near-zero weights for distant onsets,
            // completely neutralising the effect. It is applied post-hoc below.
            acc[j]  += w * (ref[j] + offset);
            wsum[j] += w;
        }
    }

    // Normalize to get Gaussian-weighted average at each j
    std::vector<double> out(m);
    for (size_t i = 0; i < m; ++i)
        out[i] = (wsum[i] > 0.0) ? acc[i] / wsum[i] : ref[i];

    // Post-hoc displacement boost — applied AFTER the weighted average so it
    // is fully independent of Gaussian weights and produces a clear, audible
    // effect at any sigmaScale setting.
    //
    //  displacementBoost : overall amplification multiplier on each onset's
    //                      displacement from its reference position.
    //                      0 = no extra push,  2 = triple the displacement.
    //  drawRange (sec)   : exponential saturation scale.  Onsets displaced by
    //                      more than drawRange seconds get near-full boost;
    //                      smaller displacements are boosted proportionally less.
    if (displacementBoost > 0.0f && drawRange > 1e-6f)
    {
        const double safeRange = static_cast<double>(drawRange);
        const double boost     = static_cast<double>(displacementBoost);

        for (size_t i = 0; i < m; ++i)
        {
            const double displacement = out[i] - ref[i];
            if (std::abs(displacement) < 1e-9) continue;

            // Saturation curve:
            //   |displacement| << drawRange  →  saturation ≈ 0  (minimal boost)
            //   |displacement| >> drawRange  →  saturation ≈ 1  (full boost)
            const double saturation = 1.0 - std::exp(-std::abs(displacement) / safeRange);
            const double boostMult  = 1.0 + boost * saturation;
            out[i] = ref[i] + displacement * boostMult;
        }
    }

    // Fix any order violations
    enforceMonotonicity(out, guard);
    return out;
}

//==============================================================================
// [ONSET MAPPER] Resolve Transient Collisions (Proportional)
//------------------------------------------------------------------------------
// Redistribute clusters of collapsed mapped onsets using the original onset
// geometry scaled by spreadFactor around the cluster center.
//==============================================================================

std::vector<double> OnsetMapper::resolveTransientCollisions(const std::vector<double>& oldPositions,
                                                            const std::vector<double>& referencePositions,
                                                            float spreadFactor,
                                                            double guard)
{
    const size_t N = std::min(oldPositions.size(), referencePositions.size());
    if (N < 2)
        return referencePositions;

    std::vector<double> out(N, 0.0);

    size_t start = 0;
    while (start < N)
    {
        size_t end = start;

        while (end + 1 < N && referencePositions[end + 1] == referencePositions[start])
            ++end;

        const size_t clusterSize = end - start + 1;

        if (clusterSize == 1)
        {
            out[start] = referencePositions[start];
        }
        else
        {
            const double targetPos = referencePositions[start];
            const double oldSpan   = oldPositions[end] - oldPositions[start];

            double spreadAmount = 0.0;
            if (oldSpan > 0.0)
                spreadAmount = oldSpan * spreadFactor;
            else
                spreadAmount = guard * static_cast<double>(clusterSize - 1);

            const double left  = targetPos - 0.5 * spreadAmount;
            const double step  = (clusterSize > 1) ? spreadAmount / static_cast<double>(clusterSize - 1) : 0.0;

            for (size_t k = 0; k < clusterSize; ++k)
                out[start + k] = left + step * static_cast<double>(k);
        }

        start = end + 1;
    }

    enforceMonotonicity(out, guard);
    return out;
}

//==============================================================================
// [ONSET MAPPER] Resolve With Local Density
//------------------------------------------------------------------------------
// Redistribute clustered onsets using the original local IOI pattern while
// enforcing a minimum spacing across the cluster.
//==============================================================================

std::vector<double> OnsetMapper::resolveWithLocalDensity(const std::vector<double>& oldPositions,
                                                         const std::vector<double>& referencePositions,
                                                         double minSpacing,
                                                         double guard)
{
    const size_t N = std::min(oldPositions.size(), referencePositions.size());
    if (N < 2)
        return referencePositions;

    std::vector<double> out(referencePositions.begin(),
                            referencePositions.begin() + (int) N);

    size_t start = 0;

    while (start < N)
    {
        size_t end = start;

        while (end + 1 < N && referencePositions[end + 1] == referencePositions[start])
            ++end;

        const size_t clusterSize = end - start + 1;

        if (clusterSize > 1)
        {
            const double clusterCenter = 0.5 * (out[start] + out[end]);

            // Compute original cluster bounds
            const double oldClusterMin = oldPositions[start];
            const double oldClusterMax = oldPositions[end];

            // Compute spread exactly like Python
            const double spread = std::max(minSpacing * static_cast<double>(clusterSize - 1),
                                           oldClusterMax - oldClusterMin);

            // Left boundary of redistributed cluster
            const double left = clusterCenter - 0.5 * spread;
            out[start] = left;

            // Normalize positions inside cluster (Python-style)
            if (oldClusterMax > oldClusterMin)
            {
                for (size_t i = start + 1; i <= end; ++i)
                {
                    const double norm = (oldPositions[i] - oldClusterMin) /
                                        (oldClusterMax - oldClusterMin);

                    out[i] = left + norm * spread;
                }
            }
            else
            {
                // Degenerate case: all old positions identical
                for (size_t i = start + 1; i <= end; ++i)
                    out[i] = out[i - 1] + minSpacing;
            }
        }

        start = end + 1;
    }

    enforceMonotonicity(out, guard);
    return out;
}
//==============================================================================
// [ONSET MAPPER] DTW Mapping With Collision Resolution + Gaussian Weighting
//------------------------------------------------------------------------------
OnsetMapResult OnsetMapper::mapOnsetsWithGaussian(const std::vector<double>& sourceOnsets,
                                                  const std::vector<double>& referenceOnsets,
                                                  float alpha,
                                                  float sigmaScale,
                                                  float sigmaEps,
                                                  double guard,
                                                  float displacementBoost,
                                                  float drawRange,
                                                  bool captureMatrices,
                                                  CollisionMode collisionMode,
                                                  float spreadFactor,
                                                  double minSpacing)
{
    OnsetMapResult result;

    if (sourceOnsets.empty() || referenceOnsets.empty())
    {
        result.morphedOnsets = sourceOnsets;
        return result;
    }

    //==========================================================================
    // DTW alignment
    //==========================================================================
    DTW dtw;
    auto C = dtw.costMatrix(sourceOnsets, referenceOnsets);
    auto D = dtw.accumCostMatrix(C);
    auto P = dtw.optimalWarpingPath(D);

    result.dtwPathLength = (int) P.size();
    result.dtwFinalCost  = D.back().back();

    double avgCost = 0.0;
    if (!C.empty() && !C[0].empty())
    {
        for (const auto& row : C)
            for (double val : row)
                avgCost += val;

        avgCost /= (double) (C.size() * C[0].size());
    }
    result.dtwAvgCost = avgCost;

    //==========================================================================
    // DTW -> raw mapped onsets
    //==========================================================================
    auto mappedOnsets = buildNewOnsets(sourceOnsets,
                                       referenceOnsets,
                                       P);

    //==========================================================================
    // Collision / density redistribution
    //==========================================================================
    std::vector<double> resolvedOnsets = mappedOnsets;

    switch (collisionMode)
    {
        case CollisionMode::Proportional:
            resolvedOnsets = resolveTransientCollisions(sourceOnsets,
                                                        mappedOnsets,
                                                        spreadFactor,
                                                        guard);
            break;

        case CollisionMode::LocalDensity:
            resolvedOnsets = resolveWithLocalDensity(sourceOnsets,
                                                     mappedOnsets,
                                                     minSpacing,
                                                     guard);
            break;

        case CollisionMode::None:
        default:
            break;
    }

    //==========================================================================
    // Gaussian redistribution
    //==========================================================================
    auto smoothedOnsets = gaussianWeightedMapping(sourceOnsets,
                                                  resolvedOnsets,
                                                  sigmaScale,
                                                  sigmaEps,
                                                  guard,
                                                  displacementBoost,
                                                  drawRange);

    //==========================================================================
    // Alpha interpolation
    //==========================================================================
    const size_t N = sourceOnsets.size();
    std::vector<double> blendedOnsets(N);

    for (size_t i = 0; i < N; ++i)
    {
        const double smoothed = (i < smoothedOnsets.size())
                              ? smoothedOnsets[i]
                              : sourceOnsets[i];

        blendedOnsets[i] = (1.0 - alpha) * sourceOnsets[i]
                         + alpha * smoothed;
    }

    //==========================================================================
    // Final monotonicity
    //==========================================================================
    enforceMonotonicity(blendedOnsets, guard);

    result.morphedOnsets = blendedOnsets;

    if (captureMatrices)
    {
        result.dtwCostMatrix  = C;
        result.dtwAccumMatrix = D;
        result.dtwPath        = P;
    }

    return result;
}
