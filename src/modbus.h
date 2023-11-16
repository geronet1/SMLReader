#ifndef MODBUS_H
#define MODBUS_H

#ifndef USE_HARDWARESERIAL
#include <SoftwareSerial.h>
#endif
#include <SDM.h>

#include <jled.h>
#include "debug.h"

using namespace std;

enum modbus_types
{
    SDM630,
    unknown
};

const uint8_t NAME_LENGTH = 20;

typedef volatile struct
{
    volatile float regvalarr;
    const uint16_t regarr;
    char name[NAME_LENGTH];
    uint8_t prec;
} sdm_struct;

#define NBREG 19 // number of sdm registers to read
volatile sdm_struct sdmarr[NBREG] = {
    {NAN, SDM_PHASE_1_VOLTAGE, "voltage_L1", 1}, // V
    {NAN, SDM_PHASE_2_VOLTAGE, "voltage_L2", 1}, // V
    {NAN, SDM_PHASE_3_VOLTAGE, "voltage_L3", 1}, // V
    {NAN, SDM_PHASE_1_CURRENT, "current_L1", 3}, // A
    {NAN, SDM_PHASE_2_CURRENT, "current_L2", 3}, // A
    {NAN, SDM_PHASE_3_CURRENT, "current_L3", 3}, // A
    {NAN, SDM_PHASE_1_POWER, "power_L1", 0},     // W
    {NAN, SDM_PHASE_2_POWER, "power_L2", 0},     // W
    {NAN, SDM_PHASE_3_POWER, "power_L3", 0},     // W

    {NAN, SDM_SUM_LINE_CURRENT, "current_sum", 3},               // A
    {NAN, SDM_TOTAL_SYSTEM_POWER, "power_total", 0},             // W
    {NAN, SDM_TOTAL_SYSTEM_APPARENT_POWER, "power_apparent", 0}, // VA
    {NAN, SDM_TOTAL_SYSTEM_REACTIVE_POWER, "power_reactive", 0}, // VAr
    {NAN, SDM_TOTAL_SYSTEM_POWER_FACTOR, "power_factor", 3},     // None
    {NAN, SDM_TOTAL_SYSTEM_PHASE_ANGLE, "phase_angle", 0},       // Degr.
    {NAN, SDM_FREQUENCY, "frequency", 2},                        // Hz
    {NAN, SDM_IMPORT_ACTIVE_ENERGY, "energy_import", 3},         // kWh
    {NAN, SDM_EXPORT_ACTIVE_ENERGY, "energy_export", 3},         // kWh

    {NAN, SDM_NEUTRAL_CURRENT, "current_N", 3},

    //    {NAN, SDM_LINE_1_TO_LINE_2_VOLTS, "voltage_L1_L2", 1}, // V
    //    {NAN, SDM_LINE_2_TO_LINE_3_VOLTS, "voltage_L2_L3", 1}, // V
    //    {NAN, SDM_LINE_3_TO_LINE_1_VOLTS, "voltage_L3_L1", 1},
};

void clear_sdmarr()
{
    for (uint8_t i = 0; i < NBREG; i++)
        sdmarr[i].regvalarr = NAN;
}

void insert_result(uint16_t reg, float result)
{
    // DEBUG("insert [%X/%d]:%f", reg, reg, result);

    for (uint8_t i = 0; i < NBREG; i++)
    {
        if (sdmarr[i].regarr == reg)
        {
            sdmarr[i].regvalarr = result;
            // DEBUG("-->%s\n", sdmarr[i].name);
            return;
        }
    }
}

class ModbusConfig
{
public:
    int numSlaves = 0;
    int baud = 38400UL;
    int mode = SERIAL_8N1;
    int dere_pin = NOT_A_PIN;
    bool swapuart = false;
    int msTurnaround = WAITING_TURNAROUND_DELAY;
    int msTimeout = RESPONSE_TIMEOUT;
};

class ModbusSlaveConfig
{
public:
    char *name;
    uint8_t slave_id;
    char *type;
    uint16_t interval;
    unsigned long lastReadTime;

    uint16_t cnterrors;
    uint16_t cntsuccess;
    uint16_t lasterror;
};

class Modbus
{
    unsigned long readtime;
    const ModbusConfig *config;
    ModbusSlaveConfig *slave_config;
    SDM *sdm;
#ifndef USE_HARDWARESERIAL
    SoftwareSerial swSerSDM;
#endif

public:
    Modbus(const ModbusConfig *config, ModbusSlaveConfig slave_config[], void (*callback)(uint8_t slave))
    {
        DEBUG("Initializing modbus");
        this->config = config;
        this->slave_config = slave_config;
        this->callback = callback;

#ifdef USE_HARDWARESERIAL
        sdm = new SDM(Serial, config->baud, config->dere_pin, config->mode, config->swapuart);
#else
        sdm = new SDM(swSerSDM, config->baud, config->dere_pin, config->mode, SDM_RX_PIN, SDM_TX_PIN); // esp32 default pins for Serial0 => RX pin 3, TX pin 1
#endif
        sdm->begin();

        sdm->setMsTurnaround(config->msTurnaround);
        sdm->setMsTimeout(config->msTimeout);

        for (uint8_t i = 0; i < config->numSlaves; i++)
        {
            slave_config[i].lastReadTime = millis();
        }
        DEBUG("Initialized Modbus with %d baud and mode %d, swapped = %s", config->baud, config->mode, config->swapuart ? "true" : "false");
    }

    void sdmRead(int index)
    {
        for (uint8_t i = 0; i < NBREG; i++)
        {
            sdmarr[i].regvalarr = sdm->readVal(sdmarr[i].regarr, slave_config[index].slave_id);
            yield();
        }
        slave_config[index].cnterrors = sdm->getErrCount();
        slave_config[index].cntsuccess = sdm->getSuccCount();
        slave_config[index].lasterror = sdm->getErrCode(true);
    }

    void loop()
    {
        for (uint8_t i = 0; i < config->numSlaves; i++)
        {
            if (slave_config[i].interval <= 0)
                continue;

            if (millis() - slave_config[i].lastReadTime >= slave_config[i].interval * 1000)
            {
                slave_config[i].lastReadTime = millis();
                uint8_t error = 0;
                clear_sdmarr();

                // 1. Registerblock von 0x00 - 0x10
                error = sdm->readValues(SDM_PHASE_1_VOLTAGE, SDM_PHASE_3_POWER, slave_config[i].slave_id, insert_result);
                if (error == SDM_ERR_NO_ERROR)
                {
                    // 2. Registerblock von
                    error = sdm->readValues(SDM_SUM_LINE_CURRENT, SDM_EXPORT_ACTIVE_ENERGY, slave_config[i].slave_id, insert_result);
                    if (error == SDM_ERR_NO_ERROR)
                    {
                        // 30224
                        float res = 0;
                        res = sdm->readVal(SDM_NEUTRAL_CURRENT, slave_config[i].slave_id);
                        insert_result(SDM_NEUTRAL_CURRENT, res);
                    }
                }

                if (error != SDM_ERR_NO_ERROR)
                    DEBUG("Modbus readValues error:%d\n", error);

                slave_config[i].cnterrors = sdm->getErrCount();
                slave_config[i].cntsuccess = sdm->getSuccCount();
                slave_config[i].lasterror = sdm->getErrCode(true);
                
                /*
                    DEBUG("Modbus slave [%s] ID: %d:", slave_config[i].name, slave_config[i].slave_id);
                    for (uint8_t j = 0; j < NBREG; j++)
                    {
                        DEBUG("%s: %f", sdmarr[j].name, sdmarr[j].regvalarr);
                    }
                */
                // Call listener
                if (this->callback != NULL)
                {
                    this->callback(i);
                }
            }
            yield();
        }
    }

private:
    void (*callback)(uint8_t index) = NULL;
};

#endif