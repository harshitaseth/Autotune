

#ifndef _TARGET_FREQ_H_
#define _TARGET_FREQ_H_

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "fourier.h"

/**
 * This file contains utilities to work with musical notes
 */

/**
 * Notes expressed as semitones above C
 */





/**
 * Calculates nearest frequency that corresponds to a note in the key
 * @param actualFreq The frequency of the signal (can be estimated using
 *     fundamental())
 * @param key The desired key in which to search for the closest acceptable note
 */
double getTargetFreq(double actualFreq,int counter);

#endif
