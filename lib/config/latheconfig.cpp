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


int   LatheConfigDerived::spindleEncoderPpr() const {
    return config->spindleEncoderPpr;
}
int   LatheConfigDerived::stepperPpr() const {
    return config->stepperPpr;
}
bool  LatheConfigDerived::invertDirection() const {
    return config->invertDirection;
}
int   LatheConfigDerived::gearboxRatioNumerator() const {
    return config->gearboxRatioNumerator;
}
int   LatheConfigDerived::gearboxRatioDenominator() const {
    return config->gearboxRatioDenominator;
}
float LatheConfigDerived::leadscrewPitchMm() const {
    return config->leadscrewPitchMm;
}
int   LatheConfigDerived::jogSpeed() const {
    return config->jogSpeed;
}
int   LatheConfigDerived::leadscrewAcceleration() const {
    return config->leadscrewAcceleration;
}
int   LatheConfigDerived::leadscrewMaxSpeed() const {
    return config->leadscrewMaxSpeed;
}
uint8_t LatheConfigDerived::theme() const {
    return config->theme;
}
DroDatumPreference LatheConfigDerived::droDatum() const {
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
float LatheConfigDerived::leadscrewStepsPerMm() const { return m_leadscrewStepsPerMm; }

float LatheConfigDerived::jogSpeedPps() const { return m_jogSpeedPps; }

float LatheConfigDerived::leadscrewMaxSpeedPps() const { return m_leadscrewMaxSpeedPps; }

float LatheConfigDerived::accellerationPulseSec() const { return m_accellerationPulseSec; }

float LatheConfigDerived::leadscrewInitialPulseDelay() const { return m_leadscrewInitialPulseDelay; }

float LatheConfigDerived::gearboxRatio() const { return m_gearboxRatio; }

int LatheConfigDerived::dirRight() const { return m_dirRight; }
int LatheConfigDerived::dirLeft() const { return m_dirLeft; }


