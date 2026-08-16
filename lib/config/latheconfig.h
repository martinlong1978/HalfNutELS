#include <stdint.h>

#ifndef ELS_LATHECONFIG_H
#define ELS_LATHECONFIG_H

#define CHECKVALUE 0xDFEB0940

//#define ELS_OFFLINE
typedef struct LatheConfig {
    int32_t check;
    int spindleEncoderPpr = 1200;
    int stepperPpr = 400;
    bool invertDirection = true; // true => right = 1. 
    int gearboxRatioNumerator = 2;
    int gearboxRatioDenominator = 1;
    float leadscrewPitchMm = 2.54; //mm per turn
    int jogSpeed = 40; //mm/s
    int leadscrewAcceleration = 150; //mm/s2
    int leadscrewMaxSpeed = 40; // mm/s

} LatheConfig;


class LatheConfigDerived {
private:
    LatheConfig* config;
    // Derived values are precomputed once in the constructor (config is fixed at
    // runtime - changes require a reboot) so the hot leadscrew loop reads cached
    // values instead of recomputing divisions every update().
    float m_leadscrewStepsPerMm;
    float m_jogSpeedPps;
    float m_leadscrewMaxSpeedPps;
    float m_accellerationPulseSec;
    float m_leadscrewInitialPulseDelay;
    float m_gearboxRatio;
    int m_dirRight;
    int m_dirLeft;
public:
    LatheConfigDerived(LatheConfig* config);

    int spindleEncoderPpr();
    int stepperPpr();
    bool invertDirection(); // true => right = 1. 
    int gearboxRatioNumerator();
    int gearboxRatioDenominator();
    float leadscrewPitchMm(); //mm per turn
    int jogSpeed(); //mm/s
    int leadscrewAcceleration(); //mm/s2
    int leadscrewMaxSpeed(); // mm/s

    float leadscrewStepsPerMm();
    float jogSpeedPps();
    float leadscrewMaxSpeedPps();
    float accellerationPulseSec();
    float leadscrewInitialPulseDelay();
    float gearboxRatio();
    int dirRight();
    int dirLeft();

};

#endif