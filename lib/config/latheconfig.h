#include <stdint.h>

#ifndef ELS_LATHECONFIG_H
#define ELS_LATHECONFIG_H

#define CHECKVALUE 0xDFEB093F

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