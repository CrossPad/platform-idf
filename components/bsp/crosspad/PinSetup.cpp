#include "crosspad_s3.h"
#include "driver/gpio.h"

// extern CrosspadSettings *settings;
// extern CrosspadStatus status;
volatile bool button2Pressed = false;


void IRAM_ATTR Button2Interrupt(void)
{
    button2Pressed = true;
}

// ----------------------------------------------------------
// Non-interrupted averaging of ADC readings and decoding audio jack status
// ----------------------------------------------------------

static const uint16_t VCC = 3300;
// Common resistor to 3.3V
static const uint16_t R   = 10000; // 10kΩ
static const uint16_t resistances[4] = {
    1000,   // R1
    5100,   // R2
    10000,  // R3
    22000   // R4
};

static const uint8_t SAMPLES_TO_AVERAGE = 8;

static uint32_t g_sumADC       = 0;  // sum of ADC readings
static uint8_t  g_sampleCount  = 0;  // number of samples collected

// decoded jack status (0..15 -> 0bDCBA)
static uint8_t  g_jackStatus   = 0;

// Table of expected voltages for each combination of jacks
static double expectedVoltage[16];
static bool   g_initDone = false;

constexpr void initExpectedVoltageTable()
{
//   for all combinations of jacks
    for (uint8_t combo = 0; combo < 16; combo++)
    {
        // parrallel resistance calculation
        double sumInv = 0.0;
        for (uint8_t bit = 0; bit < 4; bit++)
        {
            if (combo & (1 << bit))  // if inserted
            {
                sumInv += 1.0 / (double)resistances[bit];
            }
        }

        // if none of the jacks is connected, R_par = 1e12 (very high)
        double R_par = (sumInv > 0.0) ? (1.0 / sumInv) : 1e12;

        // voltage on ADC input
        double vOut = (double)VCC * (R_par / (R + R_par));
        expectedVoltage[combo] = vOut;
    }
}

// Decode jack status based on measured voltage
static uint8_t decodeJackStatus(uint32_t measuredMV)
{
// find the best match
    uint8_t bestCombo = 0;
    double  bestDiff  = 1e12;

    for (uint8_t combo = 0; combo < 16; combo++)
    {
        // double diff = fabs((double)measuredMV - expectedVoltage[combo]);
        // TODO: implement proper diff calculation
        double diff = 0.0;
        if (diff < bestDiff)
        {
            bestDiff  = diff;
            bestCombo = combo;
        }
    }
    return bestCombo; // 0..15 -> 0bDCBA
}

// needs to be called periodically to update jack status, updates every `SAMPLES_TO_AVERAGE` samples
void updateJackStatusNonBlocking()
{
    if (!g_initDone)
    {
        initExpectedVoltageTable();
        g_initDone = true;
    }

    // uint16_t adcVal = analogRead(ADC_PIN);
    // TODO: implement ADC measurement
    uint16_t adcVal = 0;

    g_sumADC += adcVal;
    g_sampleCount++;

    if (g_sampleCount >= SAMPLES_TO_AVERAGE)
    {
        uint32_t avgADC = g_sumADC / g_sampleCount; // 0..4095
        uint32_t measuredMV = (avgADC * (uint32_t)VCC) / 4095;

        g_jackStatus = decodeJackStatus(measuredMV);

        g_sumADC      = 0;
        g_sampleCount = 0;
    }
}

uint8_t getJackStatus()
{
    return g_jackStatus;
}

bool getJackState(uint8_t jack)
{
    return (g_jackStatus & (1 << jack)) != 0;
}