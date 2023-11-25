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
    SDM_EXAMPLE,
};

const uint8_t NAME_LENGTH = 20;

typedef volatile struct
{
    volatile float regvalarr;
    const uint16_t regarr;
    char name[NAME_LENGTH];
    uint8_t prec;
} sdm_struct;

typedef volatile struct
{
    const uint16_t start;
    const uint16_t end;
} register_struct;

register_struct register_blocks[] = {
    {SDM_PHASE_1_VOLTAGE, SDM_PHASE_3_POWER},                 // 1st block
    {SDM_SUM_LINE_CURRENT, SDM_FREQUENCY},                    // 2nd block
    {SDM_LINE_1_TO_LINE_2_VOLTS, SDM_LINE_3_TO_LINE_1_VOLTS}, // 3rd block
    {SDM_IMPORT_ACTIVE_ENERGY, SDM_EXPORT_ACTIVE_ENERGY},     // 4th block
    {SDM_NEUTRAL_CURRENT, 0},                                 // single value
};

#define NBREG 22 // number of sdm registers to read
volatile sdm_struct sdmarr[NBREG] = {
    // float value, register number, mqtt name, precision
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

    {NAN, SDM_NEUTRAL_CURRENT, "current_N", 3},

    {NAN, SDM_LINE_1_TO_LINE_2_VOLTS, "voltage_L1_L2", 1}, // V
    {NAN, SDM_LINE_2_TO_LINE_3_VOLTS, "voltage_L2_L3", 1}, // V
    {NAN, SDM_LINE_3_TO_LINE_1_VOLTS, "voltage_L3_L1", 1}, // V

    {NAN, SDM_IMPORT_ACTIVE_ENERGY, "energy_import", 3}, // kWh
    {NAN, SDM_EXPORT_ACTIVE_ENERGY, "energy_export", 3}, // kWh
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

// States
enum ModbusState
{
    MODBUS_STANDBY,
    MODBUS_IDLE,
    MODBUS_PREPARE_TO_TRANSMIT,
    MODBUS_START_TRANSMIT,
    MODBUS_TRANSMIT,
    MODBUS_RECEIVE,
    MODBUS_PROCESS_MESSAGE,
    MODBUS_FINISH,
    MODBUS_PUBLISH,
};

#define MODBUS_PUBLISH_ERROR 0xFE
#define MODBUS_PUBLISH_ID_SERIAL 0xFF

class ModbusConfig
{
public:
    uint8_t numSlaves = 0;
    int baud = 38400UL;
    uint8_t mode = SERIAL_8N1;
    int direction_pin = NOT_A_PIN;
    bool swapuart = false;
    uint16_t msDelay = SDM_TRANSMIT_DELAY;
    uint16_t msTimeout = SDM_RESPONSE_TIMEOUT;
};

class ModbusSlaveConfig
{
public:
    char *name;
    uint8_t id;
    uint32_t serial;
    uint8_t type;
    uint16_t interval;
    int status_led_pin;
    bool status_led_inverted;
    unique_ptr<JLed> status_led;
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
    ModbusState state = MODBUS_IDLE;
#ifndef USE_HARDWARESERIAL
    SoftwareSerial swSerSDM;
#endif

public:
    Modbus(const ModbusConfig *config, ModbusSlaveConfig slave_config[], void (*callback)(uint8_t index, uint8_t slave_index))
    {
        DEBUG("Initializing modbus");
        this->config = config;
        this->slave_config = slave_config;
        this->callback = callback;

#ifdef USE_HARDWARESERIAL
        sdm = new SDM(Serial, config->baud, config->direction_pin, config->mode, config->swapuart);
#else
        sdm = new SDM(swSerSDM, config->baud, config->direction_pin, config->mode, SDM_RX_PIN, SDM_TX_PIN); // esp32 default pins for Serial0 => RX pin 3, TX pin 1
#endif
        sdm->begin();

        sdm->setMsDelay(config->msDelay);
        sdm->setMsTimeout(config->msTimeout);

        for (uint8_t i = 0; i < config->numSlaves; i++)
        {
            ModbusSlaveConfig *slave = &(slave_config[i]);
            slave->lastReadTime = millis();
            if (slave->status_led_pin != NOT_A_PIN)
            {
                slave->status_led = unique_ptr<JLed>(new JLed(slave->status_led_pin));
                if (slave->status_led_inverted)
                    slave->status_led->LowActive();
            }
        }
        DEBUG("Initialized Modbus with %d baud and mode %d, swapped = %s", config->baud, config->mode, config->swapuart ? "true" : "false");
    }

    void sdmRead(int index)
    {
        for (uint8_t i = 0; i < NBREG; i++)
        {
            sdmarr[i].regvalarr = sdm->readVal(sdmarr[i].regarr, slave_config[index].id);
            yield();
        }
        slave_config[index].cnterrors = sdm->getErrCount();
        slave_config[index].cntsuccess = sdm->getSuccCount();
        slave_config[index].lasterror = sdm->getErrCode(true);
    }

    void loop()
    {
        static uint8_t slave_index = 0;
        static uint8_t block_index = 0;
        static unsigned long time = 0;
        static uint8_t error = SDM_ERR_NO_ERROR;
        ModbusSlaveConfig *slave = &(slave_config[slave_index]);

        switch (state)
        {
        case MODBUS_STANDBY:
            break;
        case MODBUS_IDLE:
            for (uint8_t i = 0; i < config->numSlaves; i++)
            {
                slave = &(slave_config[i]);

                if (slave->interval <= 0)
                    continue;

                if (slave->serial == 0)
                {
                    slave->serial = sdm->getSerialNumber(slave->id);
                    return;
                }

                if (millis() - slave->lastReadTime >= slave->interval * 1000)
                {
                    slave->lastReadTime = millis();
                    slave_index = i;
                    block_index = 0;
                    state = MODBUS_PREPARE_TO_TRANSMIT;
                }
            }
            break;
        case MODBUS_PREPARE_TO_TRANSMIT:
            time = millis(); // delay for pre-transmit
            sdm->enableTransmit();
            state = MODBUS_START_TRANSMIT;
            break;
        case MODBUS_START_TRANSMIT:
            // fix for issue (nan reading) by sjfaustino: https://github.com/reaper7/SDM_Energy_Meter/issues/7#issuecomment-272111524
            if (millis() > time + config->msDelay)
            {
                sdm->Transmit(register_blocks[block_index].start, register_blocks[block_index].end, slave->id, SDM_READ_INPUT_REGISTER);
                time = millis(); // time for transmit
                state = MODBUS_TRANSMIT;
            }
            break;
        case MODBUS_TRANSMIT:
            if (millis() > time + int(8 * 10 * 1000 / config->baud)) // 8 bytes * 10 bit/byte * 1000 ms / baudrate
            {
                sdm->disableTransmit();
                time = millis(); // timeout for receiving
                // clear_sdmarr();
                state = MODBUS_RECEIVE;
            }
            break;
        case MODBUS_RECEIVE:
            if (sdm->Receive())
            {
                state = MODBUS_PROCESS_MESSAGE;
            }
            else
            {
                if (millis() > time + config->msTimeout)
                {
                    // not enough data received
                    slave->cnterrors++;
                    if (sdm->available() == 5)
                        slave->lasterror = sdm->ReceiveError();
                    else
                        slave->lasterror = SDM_ERR_NOT_ENOUGHT_BYTES;

                    if (slave->status_led_pin != NOT_A_PIN)
                        slave->status_led->Blink(100, 50).Repeat(3);

                    error = SDM_ERR_NOT_ENOUGHT_BYTES;
                    state = MODBUS_PUBLISH;
                }
            }
            break;
        case MODBUS_PROCESS_MESSAGE:
            error = sdm->Process(insert_result);
            if (error == SDM_ERR_NO_ERROR)
            {
                slave->cntsuccess++;
                state = MODBUS_FINISH;
            }
            else
            {
                slave->cnterrors++;
                slave->lasterror = error;
                if (slave->status_led_pin != NOT_A_PIN)
                    slave->status_led->Blink(100, 50).Repeat(3);

                state = MODBUS_PUBLISH;
            }
            break;
        case MODBUS_FINISH:
            if (++block_index < sizeof(register_blocks - 1))
            {
                state = MODBUS_PREPARE_TO_TRANSMIT;
            }
            else
            {
                state = MODBUS_PUBLISH;
                if (slave->status_led_pin != NOT_A_PIN)
                    slave->status_led->Blink(50, 50).Repeat(2);
            }
            break;
        case MODBUS_PUBLISH:
            if (this->callback != NULL)
            {
                for (uint8_t i = 0; i < NBREG; i++)
                {
                    if (error != SDM_ERR_NO_ERROR)
                    {
                        this->callback(MODBUS_PUBLISH_ERROR, slave_index);
                        state = MODBUS_IDLE;
                        return;
                    }
                    if (!isnan(sdmarr[i].regvalarr))
                    {
                        this->callback(i, slave_index);
                        return; // publish only one topic at a time
                                // and don't advance to next slave until all are published
                    }
                }
                // no error and nothing to publish any more
                this->callback(MODBUS_PUBLISH_ID_SERIAL, slave_index);
                state = MODBUS_IDLE;

            }
            break;
        }

        for (uint8_t i = 0; i < config->numSlaves; i++)
        {
            ModbusSlaveConfig *slave = &(slave_config[i]);

            if (slave->status_led_pin != NOT_A_PIN)
                slave->status_led->Update();
        }
    }

    /*
            void loop_old()
            {
                for (uint8_t i = 0; i < config->numSlaves; i++)
                {
                    ModbusSlaveConfig *slave = &(slave_config[i]);

                    if (slave->status_led_pin != NOT_A_PIN)
                        slave->status_led->Update();

                    if (slave->interval <= 0)
                        continue;

                    if (millis() - slave->lastReadTime >= slave->interval * 1000)
                    {
                        slave->lastReadTime = millis();
                        uint8_t error = SDM_ERR_NO_ERROR;
                        clear_sdmarr();

                        if (slave->type == SDM630_V)
                        {
                            // 1. Registerblock von
                            error = sdm->readValues(SDM_PHASE_1_VOLTAGE, SDM_PHASE_3_POWER, slave->id, insert_result);
                            if (error == SDM_ERR_NO_ERROR)
                            {
                                // 2. Registerblock
                                error = sdm->readValues(SDM_SUM_LINE_CURRENT, SDM_FREQUENCY, slave->id, insert_result);
                                if (error == SDM_ERR_NO_ERROR)
                                {
                                    // 3. Registerblock
                                    error = sdm->readValues(SDM_LINE_1_TO_LINE_2_VOLTS, SDM_LINE_3_TO_LINE_1_VOLTS, slave->id, insert_result);
                                    if (error == SDM_ERR_NO_ERROR)
                                    {
                                        // 30224
                                        float res = 0;
                                        res = sdm->readVal(SDM_NEUTRAL_CURRENT, slave->id);
                                        insert_result(SDM_NEUTRAL_CURRENT, res);
                                    }
                                }
                            }
                        }
                        else if (slave->type == SDM630_E)
                        {
                            error = sdm->readValues(SDM_IMPORT_ACTIVE_ENERGY, SDM_EXPORT_ACTIVE_ENERGY, slave->id, insert_result);
                        }
                        else if (slave->type == SDM_EXAMPLE)
                        {
                            error = sdm->readValues(SDM_LINE_1_TO_LINE_2_VOLTS, SDM_LINE_3_TO_LINE_1_VOLTS, slave->id, insert_result);
                        }

                        if (error != SDM_ERR_NO_ERROR)
                        {
                            DEBUG("Modbus readValues error:%d\n", error);
                            if (slave->status_led_pin != NOT_A_PIN)
                                slave->status_led->Blink(200, 200).Repeat(2);
                        }
                        else
                        {
                            if (slave->serial == 0)
                            {
                                slave->serial = sdm->getSerialNumber(slave->id);
                            }

                            if (slave->status_led_pin != NOT_A_PIN)
                                slave->status_led->Blink(40, 40).Repeat(3);
                        }

                        slave->cnterrors = sdm->getErrCount();
                        slave->cntsuccess = sdm->getSuccCount();

                        if (error == SDM_ERR_NO_ERROR)
                            error = sdm->getErrCode(true);

                        switch (error)
                        {
                        case SDM_ERR_CRC_ERROR:
                            slave->crc_errors++;
                            break;
                        case SDM_ERR_WRONG_BYTES:
                            slave->wb_errors++;
                            break;
                        case SDM_ERR_NOT_ENOUGHT_BYTES:
                            slave->neb_errors++;
                            break;
                        case SDM_ERR_TIMEOUT:
                            slave->tmt_errors++;
                            break;
                        }

                        if (error != SDM_ERR_NO_ERROR)
                            slave->lasterror = error;

                        /
                            DEBUG("Modbus slave [%s] ID: %d:", slave->name, slave->slave_id);
                            for (uint8_t j = 0; j < NBREG; j++)
                            {
                                DEBUG("%s: %f", sdmarr[j].name, sdmarr[j].regvalarr);
                            }
                        *
                        // Call listener
                        if (this->callback != NULL)
                        {
                            this->callback(i);
                        }
                    }
                    yield();
                }
            }
    */
private:
    void (*callback)(uint8_t index, uint8_t slave_index) = NULL;
};

#endif