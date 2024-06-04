#ifndef SENSOR_H
#define SENSOR_H

#include <jled.h>
#include "sml_debug.h"

using namespace std;

// SML constants
const uint8_t START_SEQUENCE[] = {0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01};
const uint8_t END_SEQUENCE[] = {0x1B, 0x1B, 0x1B, 0x1B, 0x1A};
const size_t BUFFER_SIZE = 1000; // Max bytes per second at 9600 Baud
const uint8_t READ_TIMEOUT = 30;

// States
enum State
{
    INIT,
    STANDBY,
    IDLE,
    WAIT_FOR_MESSAGE,
    READ_MESSAGE,
    PROCESS_MESSAGE,
};

uint64_t millis64()
{
    static uint32_t low32, high32;
    uint32_t new_low32 = millis();
    if (new_low32 < low32)
        high32++;
    low32 = new_low32;
    return (uint64_t)high32 << 32 | low32;
}

class SensorConfig
{
public:
    uint8_t pin;
    char* name;
    bool numeric_only;
    bool status_led_inverted;
    int status_led_pin;
    uint16_t interval;
};

class Sensor
{
public:
    const SensorConfig *config;
    Sensor(const SensorConfig *config, void (*callback)(uint8_t *buffer, size_t len, Sensor *sensor))
    {
        this->config = config;
        DEBUG("Initializing sensor %s...", this->config->name);
        this->callback = callback;
        //this->serial = unique_ptr<SoftwareSerial>(new SoftwareSerial());
        //this->serial->begin(9600, SWSERIAL_8N1, this->config->pin, -1, false);
        //this->serial->enableTx(false);
        //this->serial->enableRx(true);
        DEBUG("Initialized sensor %s.", this->config->name);

        if (this->config->status_led_pin != NOT_A_PIN)
        {
            this->status_led = unique_ptr<JLed>(new JLed(this->config->status_led_pin));
            if (this->config->status_led_inverted)
            {
                this->status_led->LowActive();
            }
            status_led->Off().Update();
        }

        this->init_state();
    }

    bool hasProcessedMessage()
    {
        return processedMessage;
    }

    void loop()
    {
        this->run_current_state();
        yield();
        if (this->config->status_led_pin != NOT_A_PIN)
        {
            this->status_led->Update();
            yield();
        }
    }

    void new_data(uint8_t *data, uint8_t len)
    {
        if (this->state == IDLE)
            this->set_state(WAIT_FOR_MESSAGE);

        if (this->state != WAIT_FOR_MESSAGE)
            return;

        if ((this->position + len) >= BUFFER_SIZE)
        {
            this->reset_state("Buffer will overflow, starting over.");
            return;
        }        
        for (uint8_t i = 0; i < len; i++)
            this->buffer[this->position++] = data[i];
    }

    void finish_data()
    {
        if (this->state != WAIT_FOR_MESSAGE)
            return;

        DEBUG("finish: %d", this->position);
        this->set_state(READ_MESSAGE);
    }

    unique_ptr<JLed> status_led;

private:
    uint8_t buffer[BUFFER_SIZE];
    size_t position = 0;
    uint64_t standby_until = 0;
    uint8_t bytes_until_checksum = 0;
    uint8_t loop_counter = 0;
    State state = INIT;
    void (*callback)(uint8_t *buffer, size_t len, Sensor *sensor) = NULL;
    bool processedMessage;

    void run_current_state()
    {
        if (this->state != INIT)
        {
            switch (this->state)
            {
            case STANDBY:
                this->standby();
                break;
            case IDLE:
                this->idle();
                break;
            case WAIT_FOR_MESSAGE:
                this->wait_for_message();
                break;
            case READ_MESSAGE:
                this->read_message();
                break;
            case PROCESS_MESSAGE:
                this->process_message();
                break;
            default:
                break;
            }
        }
    }

    // Set state
    void set_state(State new_state)
    {
        if (new_state == STANDBY)
        {
            DEBUG("State of sensor %s is 'STANDBY'.", this->config->name);
        }
        if (new_state == IDLE)
        {
            DEBUG("State of sensor %s is 'IDLE'.", this->config->name);
        }
        else if (new_state == WAIT_FOR_MESSAGE)
        {
            DEBUG("State of sensor %s is 'WAIT_FOR_MESSAGE'.", this->config->name);
        }
        else if (new_state == READ_MESSAGE)
        {
            DEBUG("State of sensor %s is 'READ_MESSAGE'.", this->config->name);
        }
        else if (new_state == PROCESS_MESSAGE)
        {
            DEBUG("State of sensor %s is 'PROCESS_MESSAGE'.", this->config->name);
        };
        this->state = new_state;
    }

    // Initialize state machine
    void init_state()
    {
        this->set_state(IDLE);
        this->position = 0;
    }

    // Start over and wait for the start sequence
    void reset_state(const char *message = NULL)
    {
        if (message != NULL && strlen(message) > 0)
        {
            DEBUG(message);
        }
        this->init_state();
    }

    void standby()
    {
        if (millis64() >= this->standby_until)
        {
            this->reset_state();
        }
    }

    void idle()
    {
        ; // do nothing
    }

    void wait_for_message()
    {
        ; // do nothing
    }

    void read_message()
    {
        // check length of message
        if (this->position < sizeof(START_SEQUENCE) + sizeof(END_SEQUENCE))
        {
            this->reset_state("Buffer not long enough");
            return;
        }

        for (uint16_t i = 0; i < sizeof(START_SEQUENCE); i++)
        {
            if (this->buffer[i] != START_SEQUENCE[i])
            {
                this->reset_state("Start sequence error");
                return;
            }
        }

        DEBUG("Start sequence found.");
        if (this->config->status_led_pin != NOT_A_PIN)
        {
            this->status_led->Blink(50, 50).Update();
        }


        // Check for end sequence
        int start_of_end_sequence = this->position - 8;
        for (uint16_t i = 0; i < sizeof(END_SEQUENCE); i++)
        {
            if (this->buffer[i + start_of_end_sequence] != END_SEQUENCE[i])
            {
                this->reset_state("End sequence error");
                return;
            }
        }

        DEBUG("End sequence found.");
    

        DEBUG_DUMP_BUFFER(this->buffer, this->position);
        this->set_state(PROCESS_MESSAGE);
    }

    void process_message()
    {
        DEBUG("Message is being processed.");

        if (this->config->interval > 0)
        {
            this->standby_until = millis64() + (this->config->interval * 1000);
        }

        // Call listener
        if (this->callback != NULL)
        {
            this->processedMessage = true;
            this->callback(this->buffer, this->position, this);
        }

        // Go to standby mode, if throttling is enabled
        if (this->config->interval > 0)
        {
            this->set_state(STANDBY);
            return;
        }

        // Start over if throttling is disabled
        this->reset_state();
    }
};

#endif