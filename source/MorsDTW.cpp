#include "MorsDTW.h"
#include "MorsDebug.h"

#include <vector>
#include <cmath>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <limits>

// The following code is an attempted translation from an offline implementation in Python
//==============================================================================
// Local cost matrix: C[n][m] = |X[n] - Y[m]|
//==============================================================================
// Build the local distance matrix between two 1D sequences X and Y.
// Each entry C[i][j] measures the pointwise mismatch between X[i] and Y[j].
//
// Input
// -----
// X: length N sequence (e.g., source onset times or features)
// Y: length M sequence (e.g., reference onset times or features)
//
// Output
// ------
// C: N x M matrix where C[i][j] = |X[i] - Y[j]|
//==============================================================================
std::vector<std::vector<double>> DTW::costMatrix(const std::vector<double>& X,
                                                 const std::vector<double>& Y)
{
    constexpr const char* kCat = "DTW";
    LOG_SCOPE(kCat);
    PERF_START(dtw_cost_matrix);

    const size_t N = X.size();
    const size_t M = Y.size();

    LOGD(kCat, "costMatrix(): begin | X size = " + juce::String((int) N)
                    + " | Y size = " + juce::String((int) M));

    if (N == 0 || M == 0)
    {
        LOGW(kCat, "costMatrix(): empty input detected | returning empty cost matrix"
                   " | X size = " + juce::String((int) N)
                   + " | Y size = " + juce::String((int) M));
        PERF_END(dtw_cost_matrix);
        return {};
    }

    // Allocate N x M matrix initialized to 0
    std::vector<std::vector<double>> C(N, std::vector<double>(M, 0.0));

    bool foundNonFinite = false;

    // Fill costs
    for (size_t i = 0; i < N; ++i)
    {
        const double xi = X[i];

        if (! std::isfinite(xi))
        {
            LOGW(kCat, "costMatrix(): non-finite value in X at index "
                       + juce::String((int) i)
                       + " | value = " + juce::String(xi));
        }

        for (size_t j = 0; j < M; ++j)
        {
            const double yj = Y[j];

            if (! std::isfinite(yj))
            {
                LOGW(kCat, "costMatrix(): non-finite value in Y at index "
                           + juce::String((int) j)
                           + " | value = " + juce::String(yj));
            }

            const double diff = xi - yj;

            // 1D Euclidean distance magnitude is just absolute difference
            C[i][j] = std::abs(diff);

            if (! std::isfinite(C[i][j]) && ! foundNonFinite)
            {
                foundNonFinite = true;
                LOGW(kCat, "costMatrix(): non-finite cost encountered"
                           " | first occurrence at C[" + juce::String((int) i)
                           + "][" + juce::String((int) j) + "]");
            }
        }
    }

    LOGD(kCat, "costMatrix(): completed | rows = " + juce::String((int) C.size())
                    + " | cols = " + juce::String((int) C[0].size())
                    + " | C[0][0] = " + juce::String(C[0][0])
                    + " | C[N-1][M-1] = " + juce::String(C[N - 1][M - 1]));

    PERF_END(dtw_cost_matrix);
    return C;
}

//==============================================================================
// Accumulated cost matrix D (DTW dynamic programming)
//==============================================================================
// Convert local costs C into accumulated costs D using DTW recurrence.
// D[n][m] stores the minimum total path cost to reach cell (n, m) from (0, 0).
//
// Recurrence
// ----------
// D[0][0] = C[0][0]
//
// D[n][0] = C[n][0] + D[n-1][0]   for n>0   (only "up" moves)
// D[0][m] = C[0][m] + D[0][m-1]   for m>0   (only "left" moves)
//
// For interior cells (n>0, m>0):
// D[n][m] = C[n][m] + min( D[n-1][m], D[n][m-1], D[n-1][m-1] )
//
// Inputs / Assumptions
// --------------------
// - C must be non-empty and rectangular (all rows same size).
//
// Output
// ------
// D: N x M accumulated cost matrix
//==============================================================================
std::vector<std::vector<double>> DTW::accumCostMatrix(const std::vector<std::vector<double>>& C)
{
    constexpr const char* kCat = "DTW";
    LOG_SCOPE(kCat);
    PERF_START(dtw_accum_matrix);

    const int N = static_cast<int>(C.size());

    LOGD(kCat, "accumCostMatrix(): begin | rows = " + juce::String(N));

    // Error Check: must have at least one row and one column
    if (N <= 0)
    {
        LOGW(kCat, "accumCostMatrix(): empty cost matrix | returning empty accumulated matrix");
        PERF_END(dtw_accum_matrix);
        return {};
    }

    const int M = static_cast<int>(C[0].size());

    if (M <= 0)
    {
        LOGW(kCat, "accumCostMatrix(): cost matrix has zero columns | returning empty accumulated matrix");
        PERF_END(dtw_accum_matrix);
        return {};
    }

    // Validate rectangular input
    for (int n = 0; n < N; ++n)
    {
        const int rowSize = static_cast<int>(C[(size_t) n].size());

        if (rowSize != M)
        {
            LOGE(kCat, "accumCostMatrix(): non-rectangular cost matrix"
                       " | row " + juce::String(n)
                       + " has size " + juce::String(rowSize)
                       + " but expected " + juce::String(M));
            PERF_END(dtw_accum_matrix);
            throw std::runtime_error("DTW::accumCostMatrix(): cost matrix must be rectangular");
        }
    }

    if (! std::isfinite(C[0][0]))
    {
        LOGW(kCat, "accumCostMatrix(): base cost C[0][0] is non-finite | value = "
                   + juce::String(C[0][0]));
    }

    // Allocate D initialized to +infinity
    std::vector<std::vector<double>> D(
        (size_t) N,
        std::vector<double>((size_t) M, std::numeric_limits<double>::infinity()));

    // Base case
    D[0][0] = C[0][0];

    // First column
    for (int n = 1; n < N; ++n)
        D[(size_t) n][0] = C[(size_t) n][0] + D[(size_t) (n - 1)][0];

    // First row
    for (int m = 1; m < M; ++m)
        D[0][(size_t) m] = C[0][(size_t) m] + D[0][(size_t) (m - 1)];

    // Interior
    for (int n = 1; n < N; ++n)
    {
        for (int m = 1; m < M; ++m)
        {
            const double up   = D[(size_t) (n - 1)][(size_t) m];
            const double left = D[(size_t) n][(size_t) (m - 1)];
            const double diag = D[(size_t) (n - 1)][(size_t) (m - 1)];

            D[(size_t) n][(size_t) m] = C[(size_t) n][(size_t) m] + std::min({ up, left, diag });
        }
    }

    const double terminalCost = D[(size_t) (N - 1)][(size_t) (M - 1)];

    if (! std::isfinite(terminalCost))
    {
        LOGW(kCat, "accumCostMatrix(): terminal accumulated cost is non-finite"
                   " | D[N-1][M-1] = " + juce::String(terminalCost));
    }

    LOGD(kCat, "accumCostMatrix(): completed | rows = " + juce::String(N)
                    + " | cols = " + juce::String(M)
                    + " | D[0][0] = " + juce::String(D[0][0])
                    + " | D[N-1][M-1] = " + juce::String(terminalCost));

    PERF_END(dtw_accum_matrix);
    return D;
}

//==============================================================================
// Optimal warping path P: sequence of (n, m) indices
//==============================================================================
// Backtrack through the accumulated cost matrix D to recover the minimum-cost
// DTW path. The path is a sequence of index pairs (n, m) from (0,0) to (N-1,M-1).
//
// Backtracking rule
// -----------------
// Starting at (n, m) = (N-1, M-1), at each step move to the predecessor with the
// smallest accumulated cost among:
//   (n-1, m-1) diagonal
//   (n-1, m)   up
//   (n, m-1)   left
//
// Edge handling
// ------------
// If we reach the top row (n==0), we can only move left.
// If we reach the left column (m==0), we can only move up.
//
// Output
// ------
// P: vector of pairs [ [n0,m0], [n1,m1], ..., [nK,mK] ] in forward order
//==============================================================================
std::vector<std::vector<int64_t>> DTW::optimalWarpingPath(const std::vector<std::vector<double>>& D)
{
    constexpr const char* kCat = "DTW";
    LOG_SCOPE(kCat);
    PERF_START(dtw_optimal_path);

    // Error Check: must have at least one row and one column
    const int N = static_cast<int>(D.size());

    LOGD(kCat, "optimalWarpingPath(): begin | rows = " + juce::String(N));

    if (N <= 0)
    {
        LOGW(kCat, "optimalWarpingPath(): empty accumulated matrix | returning empty path");
        PERF_END(dtw_optimal_path);
        return {};
    }

    const int M = static_cast<int>(D[0].size());

    if (M <= 0)
    {
        LOGW(kCat, "optimalWarpingPath(): accumulated matrix has zero columns | returning empty path");
        PERF_END(dtw_optimal_path);
        return {};
    }

    // Validate rectangular input
    for (int n = 0; n < N; ++n)
    {
        const int rowSize = static_cast<int>(D[(size_t) n].size());

        if (rowSize != M)
        {
            LOGE(kCat, "optimalWarpingPath(): non-rectangular accumulated matrix"
                       " | row " + juce::String(n)
                       + " has size " + juce::String(rowSize)
                       + " but expected " + juce::String(M));
            PERF_END(dtw_optimal_path);
            throw std::runtime_error("DTW::optimalWarpingPath(): accumulated matrix must be rectangular");
        }
    }

    const double terminalCost = D[(size_t) (N - 1)][(size_t) (M - 1)];

    if (! std::isfinite(terminalCost))
    {
        LOGW(kCat, "optimalWarpingPath(): terminal accumulated cost is non-finite"
                   " | D[N-1][M-1] = " + juce::String(terminalCost));
    }

    LOGD(kCat, "optimalWarpingPath(): backtracking from"
                    " | start n = " + juce::String(N - 1)
                    + " | start m = " + juce::String(M - 1)
                    + " | terminal cost = " + juce::String(terminalCost));

    // Start from bottom-right
    int n = N - 1;
    int m = M - 1;

    // Store path in reverse order as we backtrack; then reverse at end
    std::vector<int> Pn = { n };
    std::vector<int> Pm = { m };

    int diagonalMoves = 0;
    int upMoves = 0;
    int leftMoves = 0;

    // Backtrack until reaching (0,0)
    while (n > 0 || m > 0)
    {
        // If on top row, must move left
        if (n == 0)
        {
            --m;
            ++leftMoves;
        }
        // If on left column, must move up
        else if (m == 0)
        {
            --n;
            ++upMoves;
        }
        else
        {
            // Choose best predecessor by comparing accumulated costs
            const double diag = D[(size_t) (n - 1)][(size_t) (m - 1)];
            const double up   = D[(size_t) (n - 1)][(size_t) m];
            const double left = D[(size_t) n][(size_t) (m - 1)];

            if (diag <= up && diag <= left)
            {
                --n;
                --m;
                ++diagonalMoves;
            }
            else if (up <= left)
            {
                --n;
                ++upMoves;
            }
            else
            {
                --m;
                ++leftMoves;
            }
        }

        Pn.push_back(n);
        Pm.push_back(m);
    }

    // Reverse to forward order: (0,0) -> (N-1, M-1)
    std::reverse(Pn.begin(), Pn.end());
    std::reverse(Pm.begin(), Pm.end());

    // Convert to required output format: vector of [n, m] pairs
    std::vector<std::vector<int64_t>> P(Pn.size(), std::vector<int64_t>(2));

    for (size_t k = 0; k < Pn.size(); ++k)
    {
        P[k][0] = static_cast<int64_t>(Pn[k]);
        P[k][1] = static_cast<int64_t>(Pm[k]);
    }

    LOGD(kCat, "optimalWarpingPath(): completed"
                    " | path length = " + juce::String((int) P.size())
                    + " | diagonal moves = " + juce::String(diagonalMoves)
                    + " | up moves = " + juce::String(upMoves)
                    + " | left moves = " + juce::String(leftMoves)
                    + " | first = (" + juce::String((int) P.front()[0]) + ", " + juce::String((int) P.front()[1]) + ")"
                    + " | last = (" + juce::String((int) P.back()[0]) + ", " + juce::String((int) P.back()[1]) + ")");

    PERF_END(dtw_optimal_path);
    return P;
}
