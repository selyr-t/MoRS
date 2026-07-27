/*
  ==============================================================================

    MorsConstants.h
    Created: 26 Mar 2026 2:45:31pm
    Author:  Edward Ryles

  ==============================================================================
*/

#pragma once

namespace MorsConstants
{
    // Compile-time defaults
    constexpr int kDefaultFFTOrder  = 11;
    constexpr int kDefaultFFTSize   = 2048;
    constexpr int kDefaultHopSize   = 128;
    constexpr int kDefaultWinLength = 2048;
    constexpr int kDefaultLifter    = 256;
    constexpr float kEps            = 1.0e-8f;

    // Runtime config — passed to prepare() and updated on change
    struct Config
    {
        int fftOrder  = kDefaultFFTOrder;
        int fftSize   = kDefaultFFTSize;
        int numBins   = kDefaultFFTSize / 2 + 1;
        int hopSize   = kDefaultHopSize;
        int winLength = kDefaultWinLength;
        int lifter    = kDefaultLifter;
    };
}
