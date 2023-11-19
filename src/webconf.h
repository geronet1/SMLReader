#ifndef WEBCONF_H
#define WEBCONF_H

#include <ESP8266WiFi.h>
#include <ESP8266HTTPUpdateServer.h>
#include <IotWebConf.h>
#include "debug.h"
#include "MqttPublisher.h"

const uint8_t MAX_SENSORS = 4;
#define IOTWEBCONF_STATUS_LED D4

struct GeneralWebConfig
{
	char numberOfSensors[2] = "1";
    char deepSleepInterval[5] = "";
};

#ifdef WITH_MODBUS
const uint8_t MAX_MODBUS = 3;

struct ModbusWebConfig
{
	char numberOfSensors[2] = "1";
    char baud[10] = "9600";
    char mode[4] = "8N1";
    char direction_pin[2] = { 1+'A','\0' };
    char swapuart[9] = "selected";
    char msTurnaround[6] = "300";
    char msTimeout[6] = "20";
};
#endif

struct SensorWebConfig
{
    char pin[2] = { D1+'A','\0' };
    char name[32] = "sensor0";
    char numeric_only[9] = "selected";
    char status_led_inverted[9] = "selected";
    char status_led_pin[2] = { D0+'A','\0' };
    char interval[5] = "0";
};

#ifdef WITH_MODBUS
struct ModbusSensorWebConfig
{
    char name[32] = "modbus0";
    char slave_id[4] = "0";
    char type[2] = { SDM630+'A','\0' };
    char status_led_pin[2] = { D6+'A','\0' };
    char status_led_inverted[9] = "selected";
    char interval[5] = "0";
};
#endif

struct WebConfGroups
{
    iotwebconf::ParameterGroup *generalGroup;
    iotwebconf::ParameterGroup *mqttGroup;
    iotwebconf::ParameterGroup *sensorGroups[MAX_SENSORS];
#ifdef WITH_MODBUS
    iotwebconf::ParameterGroup *modbusGroup;
    iotwebconf::ParameterGroup *modbusGroups[MAX_MODBUS];
#endif
};

struct SensorStrings{
	char grpid[8] = "sensor0";
	char grpname[9] = "Sensor 0";
	char pin[6] = "s0pin";
	char name[7] = "s0name";
	char numOnly[10] = "s0numOnly";
	char ledInverted[10] = "s0ledI";
	char ledPin[10] = "s0ledP";
	char interval[9] = "s0int";
};

SensorStrings sensorStrings[MAX_SENSORS];
const uint8_t NUMBER_OF_PINS=10;
const char pinOptions[] = { NOT_A_PIN+'A', '\0', D0+'A', '\0', D1+'A', '\0', D2+'A', '\0', D3+'A', '\0', D4+'A', '\0', D5+'A', '\0', D6+'A', '\0', D7+'A', '\0', D8+'A', '\0'};
const uint8_t PIN_LABEL_LENGTH = 3;
const char *pinNames[] = {"--", "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8"};

#ifdef WITH_MODBUS
struct ModbusStrings{
	char grpid[8] = "modbus0";
	char grpname[9] = "Modbus 0";
	char name[7] = "m0name";
	char slave_id[8] = "m0slave";
	char type[8] = "m0type";
	char ledPin[10] = "m0ledP";
	char ledInverted[10] = "m0ledI";
	char interval[9] = "m0int";
};

ModbusStrings modbusStrings[MAX_MODBUS];

#define BAUD_LABEL_LENGTH 10
const char baudOptions[][BAUD_LABEL_LENGTH] = { "2400","4800","9600","19200","38400" };

#define MODE_LABEL_LENGTH 4
const char modeOptions[] = { SERIAL_8N1, SERIAL_8E1, SERIAL_8O1, SERIAL_8N2 };
const char modeNames[][MODE_LABEL_LENGTH] = { "8N1", "8E1", "8O1", "8N2"};

#define TYPE_LABEL_LENGTH 10
const uint8_t NUMBER_OF_TYPES=2;
const char typeOptions[] = { SDM630 + 'A', '\0', SDM_EXAMPLE + 'A', '\0' };
const char typeNames[][TYPE_LABEL_LENGTH] = { "SDM630", "example"};

#endif

class WebConf
{
private:
    GeneralWebConfig general;
    MqttConfig mqtt;
    SensorWebConfig sensors[MAX_SENSORS];
#ifdef WITH_MODBUS
    ModbusWebConfig modbus;
    ModbusSensorWebConfig modbus_sensors[MAX_MODBUS];
#endif
    WebConfGroups groups;
    IotWebConf *iotWebConf;
    WebServer *webServer;
    DNSServer *dnsServer;
    ESP8266HTTPUpdateServer *httpUpdater;
    std::function<void(WebServer*)> status;

public:
    bool needReset = false;

    WebConf(std::function<void()> wifiConnected, std::function<void(WebServer*)> status){
        webServer = new WebServer();
        dnsServer = new DNSServer();
        httpUpdater = new ESP8266HTTPUpdateServer();
        iotWebConf = new IotWebConf(WIFI_AP_SSID, dnsServer, webServer, WIFI_AP_DEFAULT_PASSWORD, CONFIG_VERSION);

        this->status = status;

        DEBUG("Setting up WiFi and config stuff.");

        this->setupWebconf();

        iotWebConf->setConfigSavedCallback([this] { this->configSaved(); } );
        iotWebConf->setWifiConnectionCallback(wifiConnected);
        iotWebConf->setupUpdateServer(
            [this](const char* updatePath) { this->httpUpdater->setup(this->webServer, updatePath); },
            [this](const char* userName, char* password) { this->httpUpdater->updateCredentials(userName, password); }
        );

        webServer->on("/", [this] { this->iotWebConf->handleConfig(); });
        webServer->on("/status", [this] { this->status(this->webServer); } );
        webServer->on("/reset", [this] { needReset = true; });
        webServer->onNotFound([this] { this->iotWebConf->handleNotFound(); });
    }

    void doLoop(){
	    iotWebConf->doLoop();
    }

    void setupWebconf(){
        GeneralWebConfig &generalConfig = this->general;
        MqttConfig &mqttConfig = this->mqtt;
#ifdef WITH_MODBUS
        ModbusWebConfig &modbusConfig = this->modbus;
#endif
        using namespace iotwebconf;
        iotWebConf->getApTimeoutParameter()->visible = true;

        ParameterGroup* &generalGroup = this->groups.generalGroup = new ParameterGroup("general","General");    
        static char numOfSensorsValidator[] = "min='0' max='0'";
        numOfSensorsValidator[13] = MAX_SENSORS + '0';
        generalGroup->addItem(new NumberParameter("Number of sensors", "numOfSensors", generalConfig.numberOfSensors, sizeof(generalConfig.numberOfSensors), generalConfig.numberOfSensors, nullptr, numOfSensorsValidator));    
        generalGroup->addItem(new NumberParameter("Deep sleep interval (s)", "deepSleep", generalConfig.deepSleepInterval, sizeof(generalConfig.deepSleepInterval), generalConfig.deepSleepInterval, nullptr, "min='0' max='3600'"));
        iotWebConf->addParameterGroup(generalGroup);

        ParameterGroup* &mqttGroup = this->groups.mqttGroup = new ParameterGroup("mqtt","MQTT");
        mqttGroup->addItem(new TextParameter("MQTT server", "mqttServer", mqttConfig.server, sizeof(mqttConfig.server), mqttConfig.server));
        mqttGroup->addItem(new TextParameter("MQTT port", "mqttPort", mqttConfig.port, sizeof(mqttConfig.port), mqttConfig.port));
        mqttGroup->addItem(new TextParameter("MQTT username", "mqttUsername", mqttConfig.username, sizeof(mqttConfig.username), mqttConfig.username));
        mqttGroup->addItem(new PasswordParameter("MQTT password", "mqttPassword", mqttConfig.password, sizeof(mqttConfig.password), mqttConfig.password));
        mqttGroup->addItem(new TextParameter("MQTT topic", "mqttTopic", mqttConfig.topic, sizeof(mqttConfig.topic), mqttConfig.topic));
        mqttGroup->addItem(new CheckboxParameter("MQTT JSON Payload", "mqttJsonPayload", mqttConfig.jsonPayload, sizeof(mqttConfig.jsonPayload), mqttConfig.jsonPayload));
        iotWebConf->addParameterGroup(mqttGroup);

#ifdef WITH_MODBUS
        ParameterGroup* &modbusGroup = this->groups.modbusGroup = new ParameterGroup("modbus","Modbus");
        static char numOfModbusValidator[] = "min='0' max='0'";
        numOfModbusValidator[13] = MAX_MODBUS + '0';
        modbusGroup->addItem(new NumberParameter("Number of sensors", "numOfModbusSensors", modbusConfig.numberOfSensors, sizeof(modbusConfig.numberOfSensors), modbusConfig.numberOfSensors, nullptr, numOfModbusValidator));    
        modbusGroup->addItem(new SelectParameter("Baud rate", "baudrate", modbusConfig.baud, sizeof(modbusConfig.baud), (char*)baudOptions, (char*)baudOptions, sizeof(baudOptions) / BAUD_LABEL_LENGTH, BAUD_LABEL_LENGTH, modbusConfig.baud));
        modbusGroup->addItem(new SelectParameter("Mode", "mode", modbusConfig.mode, sizeof(modbusConfig.mode), modeOptions, *modeNames, sizeof(modeNames) / MODE_LABEL_LENGTH, MODE_LABEL_LENGTH, modbusConfig.mode));
        modbusGroup->addItem(new SelectParameter("Direction pin", "directionpin", modbusConfig.direction_pin, sizeof(modbusConfig.direction_pin), pinOptions, *pinNames, NUMBER_OF_PINS, PIN_LABEL_LENGTH, modbusConfig.direction_pin));
        modbusGroup->addItem(new CheckboxParameter("Swap UART", "swapuart", modbusConfig.swapuart, sizeof(modbusConfig.swapuart), modbusConfig.swapuart));

        static char numOfTimeValidator[20];
        snprintf(numOfTimeValidator, 20, "min='%d' max='%d'", SDM_MIN_DELAY, SDM_MAX_DELAY);
        modbusGroup->addItem(new NumberParameter("Turnaround delay (ms)", "msturnaround", modbusConfig.msTurnaround, sizeof(modbusConfig.msTurnaround), modbusConfig.msTurnaround, nullptr, numOfTimeValidator));
        modbusGroup->addItem(new NumberParameter("Response timeout (ms)", "mstimeout", modbusConfig.msTimeout, sizeof(modbusConfig.msTimeout), modbusConfig.msTimeout, nullptr, numOfTimeValidator));
        iotWebConf->addParameterGroup(modbusGroup);
#endif

        for(byte i=0; i<MAX_SENSORS; i++){
            char sensorIdChar = i + '0';
            SensorStrings &strs = sensorStrings[i];
            strs.grpid[6] = sensorIdChar;
            strs.grpname[7] = sensorIdChar;
            strs.pin[1] = sensorIdChar;
            strs.name[1] = sensorIdChar;
            strs.numOnly[1] = sensorIdChar;
            strs.ledInverted[1] = sensorIdChar;
            strs.ledPin[1] = sensorIdChar;
            strs.interval[1] = sensorIdChar;
            SensorWebConfig &cfg = this->sensors[i];
            
            ParameterGroup* &sensorGroup = this->groups.sensorGroups[i] = new ParameterGroup(strs.grpid, strs.grpname);
            sensorGroup->visible = false;
            sensorGroup->addItem(new SelectParameter("Pin", strs.pin, cfg.pin, sizeof(cfg.pin), pinOptions, *pinNames, NUMBER_OF_PINS, PIN_LABEL_LENGTH, cfg.pin));
            sensorGroup->addItem(new TextParameter("Name", strs.name, cfg.name, sizeof(cfg.name), cfg.name));
            sensorGroup->addItem(new CheckboxParameter("Numeric Values Only", strs.numOnly, cfg.numeric_only, sizeof(cfg.numeric_only), cfg.numeric_only));
            sensorGroup->addItem(new SelectParameter("Led Pin", strs.ledPin, cfg.status_led_pin, sizeof(cfg.status_led_pin), pinOptions, *pinNames, NUMBER_OF_PINS, PIN_LABEL_LENGTH, cfg.status_led_pin));
            sensorGroup->addItem(new CheckboxParameter("Led inverted", strs.ledInverted, cfg.status_led_inverted, sizeof(cfg.status_led_inverted), cfg.status_led_inverted));
            sensorGroup->addItem(new NumberParameter("Standby interval (s)", strs.interval, cfg.interval, sizeof(cfg.interval), cfg.interval));
            iotWebConf->addParameterGroup(sensorGroup);
        }

#ifdef WITH_MODBUS
        for(byte i=0; i<MAX_MODBUS; i++){
            char modbusIdChar = i + '0';
            ModbusStrings &mbstrs = modbusStrings[i];
            mbstrs.grpid[6] = modbusIdChar;
            mbstrs.grpname[7] = modbusIdChar;
            mbstrs.name[1] = modbusIdChar;
            mbstrs.slave_id[1] = modbusIdChar;
            mbstrs.type[1] = modbusIdChar;
            mbstrs.interval[1] = modbusIdChar;
            mbstrs.ledPin[1] = modbusIdChar;
            mbstrs.ledInverted[1] = modbusIdChar;
            ModbusSensorWebConfig &cfg = this->modbus_sensors[i];
            
            ParameterGroup* &modbusGroup = this->groups.modbusGroups[i] = new ParameterGroup(mbstrs.grpid, mbstrs.grpname);
            modbusGroup->visible = false;
            modbusGroup->addItem(new TextParameter("Name", mbstrs.name, cfg.name, sizeof(cfg.name), cfg.name));
            modbusGroup->addItem(new NumberParameter("Slave ID", mbstrs.slave_id, cfg.slave_id, sizeof(cfg.slave_id), cfg.slave_id));
            modbusGroup->addItem(new SelectParameter("Type", mbstrs.type, cfg.type, sizeof(cfg.type), typeOptions, *typeNames, NUMBER_OF_TYPES, TYPE_LABEL_LENGTH, cfg.type));
            modbusGroup->addItem(new SelectParameter("Led Pin", mbstrs.ledPin, cfg.status_led_pin, sizeof(cfg.status_led_pin), pinOptions, *pinNames, NUMBER_OF_PINS, PIN_LABEL_LENGTH, cfg.status_led_pin));
            modbusGroup->addItem(new CheckboxParameter("Led inverted", mbstrs.ledInverted, cfg.status_led_inverted, sizeof(cfg.status_led_inverted), cfg.status_led_inverted));
            modbusGroup->addItem(new NumberParameter("Request interval (s)", mbstrs.interval, cfg.interval, sizeof(cfg.interval), cfg.interval));
            iotWebConf->addParameterGroup(modbusGroup);
        }
#endif
    }

    void loadWebconf(MqttConfig &mqttConfig, SensorConfig sensorConfigs[MAX_SENSORS], uint8_t &numOfSensors,
#ifdef WITH_MODBUS
     ModbusConfig &modbusConfig, ModbusSlaveConfig modbusConfigs[MAX_MODBUS], uint8_t &numOfModbusSensors, 
#endif
     uint16_t &deepSleepInterval)
     {
        iotWebConf->setStatusPin(IOTWEBCONF_STATUS_LED);
        boolean validConfig = iotWebConf->init();
        if (!validConfig)
        {
            DEBUG("Missing or invalid config. MQTT publisher disabled.");
            MqttConfig defaults;
            // Resetting to default values
            strcpy(mqttConfig.server, defaults.server);
            strcpy(mqttConfig.port, defaults.port);
            strcpy(mqttConfig.username, defaults.username);
            strcpy(mqttConfig.password, defaults.password);
            strcpy(mqttConfig.topic, defaults.topic);
            strcpy(mqttConfig.jsonPayload, defaults.jsonPayload);

            numOfSensors = 1;
            deepSleepInterval = 0;

            for (uint8_t i = 0; i < MAX_SENSORS; i++)
            {
                this->groups.sensorGroups[i]->visible = false;
            }
#ifdef WITH_MODBUS
            numOfModbusSensors = 0;
            for (uint8_t i = 0; i < MAX_MODBUS; i++)
            {
                this->groups.modbusGroups[i]->visible = false;
            }
#endif
        }
        else
        {
            strcpy(mqttConfig.jsonPayload, this->mqtt.jsonPayload);
            strcpy(mqttConfig.password, this->mqtt.password);
            strcpy(mqttConfig.port, this->mqtt.port);
            strcpy(mqttConfig.server, this->mqtt.server);
            strcpy(mqttConfig.topic, this->mqtt.topic);
            strcpy(mqttConfig.username, this->mqtt.username);

            numOfSensors = this->general.numberOfSensors[0] - '0';
            numOfSensors = numOfSensors < MAX_SENSORS ? numOfSensors : MAX_SENSORS;
            for (uint8_t i = 0; i < numOfSensors; i++)
            {
                this->groups.sensorGroups[i]->visible = i < numOfSensors;
                sensorConfigs[i].interval = atoi(this->sensors[i].interval);
                sensorConfigs[i].name = this->sensors[i].name;
                sensorConfigs[i].numeric_only = this->sensors[i].numeric_only[0] == 's';
                sensorConfigs[i].pin = this->sensors[i].pin[0] - 'A';
                sensorConfigs[i].status_led_inverted = this->sensors[i].status_led_inverted[0] == 's';
                sensorConfigs[i].status_led_pin = this->sensors[i].status_led_pin[0] - 'A';

            }

#ifdef WITH_MODBUS
            numOfModbusSensors = this->modbus.numberOfSensors[0] - '0';
            numOfModbusSensors = numOfModbusSensors < MAX_MODBUS ? numOfModbusSensors : MAX_MODBUS;
            modbusConfig.baud = atoi(this->modbus.baud);
            modbusConfig.mode = this->modbus.mode[0];
            modbusConfig.direction_pin = this->modbus.direction_pin[0] - 'A';
            if (modbusConfig.direction_pin == 1)
                modbusConfig.direction_pin = NOT_A_PIN;

            modbusConfig.swapuart = this->modbus.swapuart[0] == 's';
            modbusConfig.msTurnaround = atoi(this->modbus.msTurnaround);
            modbusConfig.msTimeout = atoi(this->modbus.msTimeout);

            numOfModbusSensors = numOfModbusSensors < MAX_MODBUS ? numOfModbusSensors : MAX_MODBUS;

            for (uint8_t i = 0; i < numOfModbusSensors; i++)
            {
                this->groups.modbusGroups[i]->visible = i < numOfModbusSensors;
                modbusConfigs[i].name = this->modbus_sensors[i].name;
                modbusConfigs[i].slave_id = atoi(this->modbus_sensors[i].slave_id);
                modbusConfigs[i].type = this->modbus_sensors[i].type[0] - 'A';
                modbusConfigs[i].status_led_pin = this->modbus_sensors[i].status_led_pin[0] - 'A';
                modbusConfigs[i].status_led_inverted = this->modbus_sensors[i].status_led_inverted[0] == 's';
                modbusConfigs[i].interval = atoi(this->modbus_sensors[i].interval);
            }
#endif
            deepSleepInterval = atoi(this->general.deepSleepInterval);
        }
    }
    
    void configSaved()
    {
        DEBUG("Configuration was updated.");
        needReset = true;
    }
};

#endif