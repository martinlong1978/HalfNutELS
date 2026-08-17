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
uint8_t LatheConfigDerived::theme() {
    return config->theme;
}
DroDatumPreference LatheConfigDerived::droDatum() {
    return toDroDatumPreference(config->droDatum);
}

// See latheconfig.h: DRO_DATUM_LEFT/DRO_DATUM_RIGHT are the only valid stored
// bytes. Anything else - flash garbage from a pre-CHECKVALUE-bump blob, a
// short read, bit rot - must fall back to the safe default (Left) rather than
// being cast into the enum, which would otherwise let an arbitrary byte value
// masquerade as a DroDatumPreference.
DroDatumPreference toDroDatumPreference(uint8_t storedValue) {
    switch (storedValue) {
        case DRO_DATUM_RIGHT:
            return DroDatumPreference::Right;
        case DRO_DATUM_LEFT:
            return DroDatumPreference::Left;
        default:
            return DroDatumPreference::Left;
    }
}

uint8_t fromDroDatumPreference(DroDatumPreference pref) {
    return pref == DroDatumPreference::Right ? DRO_DATUM_RIGHT : DRO_DATUM_LEFT;
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


