#include "latheconfig.h"
#include "config.h"


LatheConfigDerived::LatheConfigDerived(LatheConfig* config) {
    this->config = config;
    // Precompute derived values once. Arithmetic mirrors the original per-call
    // expressions exactly so cached results are bit-identical.
    m_gearboxRatio = (float)config->gearboxRatioNumerator / (float)config->gearboxRatioDenominator;
    m_leadscrewStepsPerMm = (float)config->stepperPpr * m_gearboxRatio / config->leadscrewPitchMm;
    m_jogSpeedPps = (float)config->jogSpeed * m_leadscrewStepsPerMm;
    m_leadscrewMaxSpeedPps = config->leadscrewMaxSpeed * m_leadscrewStepsPerMm;
    m_accellerationPulseSec = config->leadscrewAcceleration * m_leadscrewStepsPerMm;
    m_leadscrewInitialPulseDelay = ((float)US_PER_SECOND / ((float)LEADSCREW_JERK * m_leadscrewStepsPerMm));
    m_dirRight = config->invertDirection ? 1 : 0;
    m_dirLeft = config->invertDirection ? 0 : 1;
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

// steps per mm of leadscrew travel = (motor steps per leadscrew turn) / (mm per leadscrew turn)
float LatheConfigDerived::leadscrewStepsPerMm() { return m_leadscrewStepsPerMm; }

float LatheConfigDerived::jogSpeedPps() { return m_jogSpeedPps; }

float LatheConfigDerived::leadscrewMaxSpeedPps() { return m_leadscrewMaxSpeedPps; }

float LatheConfigDerived::accellerationPulseSec() { return m_accellerationPulseSec; }

float LatheConfigDerived::leadscrewInitialPulseDelay() { return m_leadscrewInitialPulseDelay; }

float LatheConfigDerived::gearboxRatio() { return m_gearboxRatio; }

int LatheConfigDerived::dirRight() { return m_dirRight; }
int LatheConfigDerived::dirLeft() { return m_dirLeft; }


