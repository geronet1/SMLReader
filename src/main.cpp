#include <list>
#include <sml/sml_file.h>
#include <IotWebConf.h>
#include "debug.h"
#include "MqttPublisher.h"
#include "config.h"
#include "webconf.h"
#include "Sensor.h"
#include "re.h"

#ifdef MODBUS
#include "modbus.h"
#endif

void wifiConnected();
void status(WebServer*);
void configSaved();

WebConf *webConf;

MqttConfig mqttConfig;
MqttPublisher publisher;

uint8_t numOfSensors;
SensorConfig sensorConfigs[MAX_SENSORS];
Sensor *sensors[MAX_SENSORS];

#ifdef MODBUS
uint8_t numOfModbusSensors;
ModbusConfig modbusConfig;
ModbusSlaveConfig modbusSlaveConfigs[MAX_MODBUS];
Modbus *modbus;
#endif

uint16_t deepSleepInterval;
uint64_t lastMessageTime = 0;
bool connected = false;
re_t pattern;
#define PATTERN_LENGTHS 3
int pattern_length[PATTERN_LENGTHS] = {30, 25, 21};

    // https://regex101.com/r/xO4qTh/1
#define REGEX_PATTERN "^\\d-0:\\d+\\.\\d\\.\\d\\*255\\(\\d+\\.\\d+\\*"
#define OBIS_STR_LENGTH 15
const char obis_list[][OBIS_STR_LENGTH] = {
    "1-0:1.8.0/255",
    "1-0:2.8.0/255",
    "1-0:16.7.0/255",
    "1-0:36.7.0/255",
    "1-0:56.7.0/255",
    "1-0:76.7.0/255",
    "1-0:32.7.0/255",
    "1-0:52.7.0/255",
    "1-0:72.7.0/255"
};
const char obis_ID[] = {
    "1-0:96.1.0/255"
};

void parse_ascii(char *input, char *obis, char *value, int max_len)
{
    // input: 1-0:2.8.0*255(000001.18200000*kWh)
    // output: obis:"1-0:2.8.0/255\0" value:"000001.18200000\0"
    const char cf = '(';
    uint8_t i, j = 0;

    for (i = 0; i < strlen(input); i++)
    {
        if (i >= max_len)
            return;
        if (input[i] == cf)
            break;

        obis[i] = input[i];
        obis[i+1] = '\0';
    }

    i++;    // jump over '('
    for (; i < strlen(input); i++)
    {
        if (i >= max_len || j >= max_len)
            return;

        value[j++] = input[i];
        value[j] = '\0';
    }
    value[strlen(value) - 1] = '\0';    // delete ')'

    for (i = 0; i < strlen(obis); i++)
    {
        if (obis[i] == '*')
        {
            obis[i] = '/'; // change '*' to '/'
            break;
        }
    }

    for (i = 0; i < strlen(value); i++)
    {
        if (value[i] == '*')
        {
            value[i] = '\0'; // change '*' to '\0'
            break;
        }
    }
}

void process_message(uint8_t *buffer, size_t len, Sensor *sensor)
{
    lastMessageTime = millis64();

    if (sensor->config->type == ASCII)
    {
        const int MAX_LEN = 50;
        char obis[MAX_LEN];
        char value[MAX_LEN];
        char delim[3] = {ASCII_CR, ASCII_LF};

        buffer[len-3] = '\0'; // Change the '!' to null-terminator
        char *token = strtok((char*)buffer, delim);

        // publish the first line
        publisher.publish(sensor, "ID", token + 1); // jump over '/'
        token = strtok(NULL, delim);

        bool id_found = false;

        while (token)
        {
            yield();
            
            int match, length;
            bool length_ok = false;

            match = re_matchp(pattern, token, &length);
            //DEBUG("match: m:%d l:%d t:%s", match, length, token);

            if (match != 0)
            {
                if (!id_found)
                {
                    // check if ID of meter
                    parse_ascii(token, obis, value, MAX_LEN);
                    if (strcmp(obis, obis_ID) == 0)
                    {
                        id_found = true;
                        publisher.publish(sensor, obis, value);
                    }
                }
            }
            else
            {
                for (uint8_t i = 0; i < PATTERN_LENGTHS; i++)
                {
                    if (length == pattern_length[i])
                        length_ok = true;
                }

                if (length_ok)
                {
                    parse_ascii(token, obis, value, MAX_LEN);

                    for (uint8_t i = 0; i < (sizeof(obis_list) / OBIS_STR_LENGTH); i++)
                    {
                        if (strcmp(obis_list[i], obis) == 0)
                        {
                            publisher.publish(sensor, obis, value);
                            break;
                        }
                    }
                }
            }

            token = strtok(NULL, delim);
        }
    }
    else
    {
        sml_file *file = sml_file_parse(buffer + 8, len - 16);
        DEBUG_SML_FILE(file);
        publisher.publish(sensor, file);

        // free the malloc'd memory
        sml_file_free(file);
    }
}

#ifdef MODBUS
void process_modbus_message(uint8_t index, uint8_t slave_index)
{
    publisher.publish(index, &(modbusSlaveConfigs[slave_index]));
}
#endif

void setup()
{
    // Setup debugging stuff
    SERIAL_DEBUG_SETUP(115200);

#ifdef SERIAL_DEBUG
    // Delay for getting a serial console attached in time
    delay(2000);
#endif

#ifdef MODBUS 
    Serial.setDebugOutput(false);
    Serial1.setDebugOutput(true);
#endif

    webConf = new WebConf(&wifiConnected, &status);

    webConf->loadWebconf(mqttConfig, sensorConfigs, numOfSensors,
#ifdef MODBUS
                        modbusConfig, modbusSlaveConfigs, numOfModbusSensors,
#endif
                        deepSleepInterval);

    // Setup MQTT publisher
    publisher.setup(mqttConfig);

    DEBUG("Setting up %d configured sensors...", numOfSensors);
    const SensorConfig *config  = sensorConfigs;
    for (uint8_t i = 0; i < numOfSensors; i++, config++)
    {
        Sensor *sensor = new Sensor(config, process_message);
        sensors[i] = sensor;
    }
    DEBUG("Sensor setup done.");

#ifdef MODBUS
    DEBUG("Setting up %d configured modbus devices...", numOfModbusSensors);
    modbusConfig.numSlaves = numOfModbusSensors;
    const ModbusConfig *mconfig = &modbusConfig;
    //const ModbusSlaveConfig *mslaveconfig  = modbusSlaveConfigs;
    modbus = new Modbus(mconfig, modbusSlaveConfigs, process_modbus_message);
    DEBUG("Modbus setup done.");
#endif

    pattern = re_compile(REGEX_PATTERN);

   DEBUG("Setup done.");
}

void loop()
{	
    if (webConf->needReset)
    {
        // Doing a chip reset caused by config changes
        DEBUG("Rebooting after 1 second.");
        delay(1000);
        ESP.restart();
    }

    bool allSensorsProcessedMessage=true;
    // Execute sensor state machines
    for (uint8_t i = 0; i < numOfSensors; i++)
    {
        sensors[i]->loop();
        allSensorsProcessedMessage&=sensors[i]->hasProcessedMessage();
    }

#ifdef MODBUS
    modbus->loop();
#endif

    webConf->doLoop();
    
    if(connected && deepSleepInterval > 0 && allSensorsProcessedMessage)
    {
        if(publisher.isConnected())
        {
            DEBUG("Disconnecting MQTT before deep sleep.");
            publisher.disconnect();
        }
        else
        {
            DEBUG("Going to deep sleep for %d seconds.", deepSleepInterval);
            ESP.deepSleep(deepSleepInterval * 1000000);
        }
    }
    yield();
    //delay(1);
}

void status(WebServer* server)
{
    char buffer[1024], *b = buffer;
    b+=sprintf(b, "{\n");
    b+=sprintf(b, "  \"chipId\":\"%08X\",\n", ESP.getChipId());
    b+=sprintf(b, "  \"uptime64\":%llu,\n", millis64());
    b+=sprintf(b, "  \"uptime\":%lu,\n", millis());
    b+=sprintf(b, "  \"lastMessageTime\":%llu,\n", lastMessageTime);
    b+=sprintf(b, "  \"mqttConnected\":%u,\n", publisher.isConnected());
    b+=sprintf(b, "  \"version\":\"%s\"\n", VERSION);
    b+=sprintf(b, "}");
    server->send(200, "application/json", buffer);
}

void wifiConnected()
{
    DEBUG("WiFi connection established.");
    connected = true;
    publisher.connect();
}