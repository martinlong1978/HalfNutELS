#include "latheconfig.h"
#include "config.h"


LatheConfigDerived::LatheConfigDerived(LatheConfig* config) {
    this->config = config;
}


int   LatheConfigDerived::spindleEncoderPpr() {
    return config->spindleEncoderPpr;
}
int   LatheConfigDerived::stepperPpr() {
    return config->stepperPpr;
}
bool  LatheConfigDerived::invertDirection() {
    return config->invertDirection;
}
int   LatheConfigDerived::gearboxRatioNumerator() {
    return config->gearboxRatioNumerator;
}
int   LatheConfigDerived::gearboxRatioDenominator() {
    return config->gearboxRatioDenominator;
}
float LatheConfigDerived::leadscrewPitchMm() {
    return config->leadscrewPitchMm;
}
int   LatheConfigDerived::jogSpeed() {
    return config->jogSpeed;
}
int   LatheConfigDerived::leadscrewAcceleration() {
    return config->leadscrewAcceleration;
}
int   LatheConfigDerived::leadscrewMaxSpeed() {
    return config->leadscrewMaxSpeed;
}

float LatheConfigDerived::leadscrewStepsPerMm() {
    // steps per mm of leadscrew travel = (motor steps per leadscrew turn) / (mm per leadscrew turn)
    return (float)stepperPpr() * ((float)gearboxRatioNumerator() / (float)gearboxRatioDenominator()) / leadscrewPitchMm();
}

float LatheConfigDerived::jogSpeedPps() {
    return (float)jogSpeed() * leadscrewStepsPerMm();
}

float LatheConfigDerived::leadscrewMaxSpeedPps() {
    return leadscrewMaxSpeed() * leadscrewStepsPerMm();
}

float LatheConfigDerived::accellerationPulseSec() {
    return leadscrewAcceleration() * leadscrewStepsPerMm();
}

float LatheConfigDerived::leadscrewInitialPulseDelay() {
    return  ((float)US_PER_SECOND / ((float)LEADSCREW_JERK * (float)leadscrewStepsPerMm()));
}

float LatheConfigDerived::gearboxRatio() {
    return (float)gearboxRatioNumerator() / (float)gearboxRatioDenominator();
}

int LatheConfigDerived::dirRight() {
    return config->invertDirection ? 1 : 0;
}
int LatheConfigDerived::dirLeft() {
    return config->invertDirection ? 0 : 1;

}


