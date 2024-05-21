#include <list>
#include <sml/sml_file.h>
#include <IotWebConf.h>
#include "debug.h"
#include "MqttPublisher.h"
#include "config.h"
#include "webconf.h"
#include "Sensor.h"

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

void process_message(byte *buffer, size_t len, Sensor *sensor)
{
    lastMessageTime = millis64();
    // Parse
    if (sensor->config->type == ASCII)
    {
        char obis[50];
        char value[50];
        char delim[3] = {ASCII_CR, ASCII_LF};

        buffer[len-3] = '\0'; // Change the '!' to null-terminator
        char *token = strtok((char*)buffer, delim);

        publisher.publish(sensor, "ID", token + 1); // jump over '/'
        token = strtok(NULL, delim);

        while (token)
        {
            uint8_t i, j = 0;
            char cf = '(';
            for (i = 0; i < strlen(token); i++)
            {
                if (token[i] == cf)
                    break;
                                 
                obis[i] = token[i];
                obis[i+1] = '\0';
            }

            i++;    // jump over '('
            for (; i < strlen(token); i++)
            {
                value[j++] = token[i];
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

            if (strlen(obis) >= 13 && strlen(value) >= 2)
                publisher.publish(sensor, obis, value);

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