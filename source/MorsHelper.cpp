/*
  ==============================================================================

    MorsHelper.cpp
    Created: 27 Mar 2026 11:34:33am
    Author:  Edward Ryles

  ==============================================================================
*/
#include "MorsHelper.h"

std::vector<int> arange(int start, int stop, int step)
{
    std::vector<int> result;
    for (int i = start; i < stop; i += step)
    {   result.push_back(i);   }
    return result;
}
