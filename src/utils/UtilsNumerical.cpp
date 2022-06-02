/* ----------------------------------------------------------------------------
 * Copyright 2017, Massachusetts Institute of Technology,
 * Cambridge, MA 02139
 * All Rights Reserved
 * Authors: Luca Carlone, et al. (see THANKS for the full author list)
 * See LICENSE for the license information
 * -------------------------------------------------------------------------- */

/**
 * @file   UtilsNumerical.cpp
 * @brief  Utilities for numerical operations
 * @author Antoni Rosinol
 */

#include "kimera-vio/utils/UtilsNumerical.h"

#include <math.h>
#include <iomanip>

#include "kimera-vio/common/vio_types.h"

namespace VIO {

namespace UtilsNumerical {

// Rounds number to a specified number of decimal digits
// (digits specifies the number of digits to keep AFTER the decimal point)
double RoundToDigit(const double x, const int digits) {
  double dec = std::pow(10, digits);  // 10^digits
  double y = double(std::round(x * dec)) / dec;
  return y;
}

// Generate random float using random number generator between -sigma and sigma
double RandomFloatGenerator(const double sigma) {
  return ((double)rand() / RAND_MAX) * sigma - sigma/2.0;
}

// Converts doulbe to sting with desired number of digits (total number of
// digits)
std::string To_string_with_precision(const double a_value,
                                                  const int n) {
  std::ostringstream out;
  out << std::setprecision(n) << a_value;
  return out.str();
}

// Converts time from nanoseconds to seconds
double NsecToSec(const Timestamp& timestamp) {
  return double(timestamp) * 1e-9;
}

// (NOT TESTED) Converts time from seconds to nanoseconds
std::int64_t SecToNsec(const double timeInSec) {
  return double(timeInSec * 1e9);
}

// (NOT TESTED) Get current time in seconds
double getCurrentTimeInSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

}  // namespace UtilsNumerical

}  // namespace VIO
