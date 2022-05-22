

#include <cmath>

#include "targetfreq.h"

double getTargetFreq(double actualFreq,int counter) {
  // Frequency in Hz of A4
  double A4 = 440;
  // Frequency in Hz of C0
  //C4 is 440*2-9/12.
  //C0 is four octave lower = 2^-4 * C4
  double C0 = A4 * pow(2, -4.75);

  // Calculate semitones away from C0 using a logarithmic scale
  //h = 12 log2(P / C0).
  double semitonesFromC0 = 12 * log2(actualFreq / C0);
  // Calculate octave for later
  int octave = static_cast<int>(semitonesFromC0) / 12;
  
  // Map to one octave
  double semitonesModded = std::fmod(semitonesFromC0, 12);
  // Round to nearest integer semitone
  double semitonesRounded = std::round(semitonesModded);
  
  bool roundedUp = true;
  if (semitonesRounded < semitonesModded) {
    roundedUp = false;
  }
  // Cast to int for purposes of querying a map
  int semitone = static_cast<int>(semitonesRounded);

  
  int targetSemitone;
  
  if (roundedUp) {
      targetSemitone = semitone - 1;
    } else {
      targetSemitone = semitone + 1;
    }

  //keeping in range [0,11]
  if (targetSemitone < 0) {
    targetSemitone += 12;
  }
  targetSemitone %= 12;

   //The closest semitone might be in the octave below or above
  if (targetSemitone - semitone > 2) {
    targetSemitone -= 12;
  } else if (semitone - targetSemitone > 2) {
    targetSemitone += 12;
  }
  // Convert from semitone to frequency on Hz
  double target = static_cast<double>(targetSemitone + octave * 12);
  target = C0 * pow(2, target / 12);
  
  return target;
}
