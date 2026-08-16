// DRO datum resolution - docs/ux-redesign.md section 8, "The DRO datum".
//
// Pure arithmetic over pulse positions. See dro.h for the resolution order.
#include "dro.h"

DroDatumSource Dro::resolveSource(const DroInput& in) {
  if (in.manualZeroSet) {
    return DroDatumSource::ManualZero;
  }
  if (in.leftStopSet && in.rightStopSet) {
    return in.preference == DroDatumPreference::Left ? DroDatumSource::LeftStop
                                                       : DroDatumSource::RightStop;
  }
  if (in.leftStopSet) {
    return DroDatumSource::LeftStop;
  }
  if (in.rightStopSet) {
    return DroDatumSource::RightStop;
  }
  return DroDatumSource::Origin;
}

int Dro::datumPulses(const DroInput& in) {
  switch (resolveSource(in)) {
    case DroDatumSource::ManualZero:
      return in.manualZeroPulses;
    case DroDatumSource::LeftStop:
      return in.leftStopPulses;
    case DroDatumSource::RightStop:
      return in.rightStopPulses;
    case DroDatumSource::Origin:
    default:
      return 0;
  }
}

int Dro::relativePulses(const DroInput& in, int currentPulses) {
  return currentPulses - datumPulses(in);
}
