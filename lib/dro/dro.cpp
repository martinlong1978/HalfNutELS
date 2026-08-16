// STUB IMPLEMENTATION - deliberately wrong.
//
// This file exists only so that test/test_dro links. The real logic is written
// against the tests in test/test_dro/test_dro.cpp; every body here returns a
// fixed value so the suite fails on assertions rather than on link errors.
#include "dro.h"

DroDatumSource Dro::resolveSource(const DroInput& in) {
  (void)in;
  return DroDatumSource::Origin;  // STUB
}

int Dro::datumPulses(const DroInput& in) {
  (void)in;
  return 0;  // STUB
}

int Dro::relativePulses(const DroInput& in, int currentPulses) {
  (void)in;
  (void)currentPulses;
  return 0;  // STUB
}
