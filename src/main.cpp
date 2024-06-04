#include <list>
#include <sml/sml_file.h>
#include <IotWebConf.h>
#include "SC16IS752.h"
#include "MqttPublisher.h"
#include "config.h"
#include "webconf.h"
#include "Sensor.h"

#ifdef MODBUS
#include "modbus.h"
#endif

void wifiConnected();
void status(WebServer *);
void configSaved();

WebConf *webConf;

MqttConfig mqttConfig;
MqttPublisher publisher;

SC16IS752 *spiuart;
#define SC16IS752_CS_PIN D8 // GPIO15
#define SC16IS752_IRQ_PIN D1 // GPIO05
#define SC16IS752_BAUDRATE_A 9600
#define SC16IS752_BAUDRATE_B 9600

/*
ESP     SC16    OSZI    Typ
D5      13      D1      CLK
D6      12      D3      SO
D7      11      D2      SI
D8      10      D0       CS
D1      15      D4      IRQ

D0                      LED
*/

uint8_t numOfSensors;
SensorConfig sensorConfigs[MAX_SENSORS];
Sensor *sensors[MAX_SENSORS];

#ifdef MODBUS
uint8_t numOfModbusSensors;
ModbusConfig modbusConfig;
ModbusSlaveConfig modbusSlaveConfigs[MAX_MODBUS];
Modbus *modbus;
#endif

volatile bool SC16IS752_IRQ = false;
uint16_t deepSleepInterval;
uint64_t lastMessageTime = 0;

bool connected = false;

void process_message(uint8_t *buffer, size_t len, Sensor *sensor)
{
    lastMessageTime = millis64();
    // Parse
    sml_file *file = sml_file_parse(buffer + 8, len - 16);

    //DEBUG_SML_FILE(file);

    publisher.publish(sensor, file);

    // free the malloc'd memory
    sml_file_free(file);
}

#ifdef MODBUS
void process_modbus_message(uint8_t index, uint8_t slave_index)
{
    publisher.publish(index, &(modbusSlaveConfigs[slave_index]));
}
#endif

void IRAM_ATTR SC16IS752_IRQ_HANDLE()
{
    SC16IS752_IRQ = true;
}

void readSC16IS752()
{
    for (uint8_t ch = 0; ch < 2; ch++)
    {
        uint8_t irq = spiuart->InterruptEventTest(ch);
        //DEBUG("INT ch%d: 0x%02X", ch, irq);

        if (irq == SC16IS750_RECEIVE_LINE_STATUS_ERROR)
        {
            uint8_t state = spiuart->linestate(ch);
            Serial.printf("LSR ERROR: 0x%02X\n", state);
            if (state & SC16IS750_LSR_FIFO_ERROR)
            {
                Serial.printf("FIFO ERROR\n");
            }
            if (state & SC16IS750_LSR_FRAMING_ERROR)
            {
                Serial.printf("FRAMING ERROR\n");
            }
            if (state & SC16IS750_LSR_PARITY_ERROR)
            {
                Serial.printf("PARITY ERROR\n");
            }
            if (state & SC16IS750_LSR_OVERRUN_ERROR)
            {
                Serial.printf("OVERRUN ERROR\n");
            }

            // clear fifo
            spiuart->ResetFifo(ch, FIFO_RX_RESET);
        }

        if (irq == SC16IS750_RECEIVE_TIMEOUT_INTERRUPT || irq == SC16IS750_RHR_INTERRUPT)
        {
            uint8_t len, data[64];
            len = spiuart->readFIFO(ch, data);
            DEBUG("DATA CH%d: %d", ch, len);

            if (sensors[ch] != NULL)
                sensors[ch]->new_data(data, len);
        }
        if (irq == SC16IS750_RECEIVE_TIMEOUT_INTERRUPT)
        {
            if (sensors[ch] != NULL)
                sensors[ch]->finish_data();
        }

        yield();
    }
}

uint8_t HexToInt(char* hexValue)
{
  uint8_t tens = (hexValue[0] <= '9') ? hexValue[0] - '0' : hexValue[0] - '7';
  uint8_t ones = (hexValue[1] <= '9') ? hexValue[1] - '0' : hexValue[1] - '7';
  return (16 * tens) + ones;
}

void serial_cmd(char* cmd)
{
    // Syntax: "w0 01 02\r\n" or "r0 01\r\n"
    // w= r or w
    // 0: channel
    // 01: register address
    // 02: new value

    bool write = (cmd[0] == 'w');
    if ((write && strlen(cmd) != 10) || (!write && strlen(cmd) != 7))
    {
        Serial.print("wrong command\n");
        return;
    }

    char ch[2] = {cmd[1], '\0'};
    char reg[3] = {cmd[3], cmd[4], '\0'};
    char val[3] = {cmd[6], cmd[7], '\0'};
    
    uint8_t channel = atoi(ch);
    uint8_t address = HexToInt(reg);
    uint8_t value = HexToInt(val);

    if (write)
        spiuart->WriteRegister(channel, address, value);

    uint8_t result = spiuart->ReadRegister(channel, address);

    Serial.printf("[REG] ch:%02X adr:%02X val:%02X\n", channel, address, result);
}

void setup()
{
    // Setup debugging stuff
    //SERIAL_DEBUG_SETUP(115200);
    Serial.begin(115200);
    Serial.setTimeout(5);

#ifdef SERIAL_DEBUG
    // Delay for getting a serial console attached in time
    delay(2000);
#endif

#ifdef MODBUS
    Serial.setDebugOutput(false);
    Serial1.setDebugOutput(true);
#endif

    webConf = new WebConf(&wifiConnected, &status);

    webConf->loadWebconf(mqttConfig, sensorConfigs, &numOfSensors,
#ifdef MODBUS
                         modbusConfig, modbusSlaveConfigs, &numOfModbusSensors,
#endif
                         deepSleepInterval);

    // Setup MQTT publisher
    publisher.setup(mqttConfig);

    DEBUG("Setting up external UART...");
    spiuart = new SC16IS752(SC16IS750_PROTOCOL_SPI, SC16IS752_CS_PIN);
    if (!spiuart->begin(SC16IS752_BAUDRATE_A, numOfSensors > 1 ? SC16IS752_BAUDRATE_B : 0)) 
    {
        DEBUG("ERROR: SC16IS752 not found!");
    }
    else
    {
        pinMode(SC16IS752_IRQ_PIN, INPUT);
        attachInterrupt(digitalPinToInterrupt(SC16IS752_IRQ_PIN), SC16IS752_IRQ_HANDLE, FALLING);
    }

    DEBUG("Setting up %d configured sensors...", numOfSensors);

    for (uint8_t i = 0; i < MAX_SENSORS; i++)
    {
        sensors[i] = NULL;
    }

    const SensorConfig *config = sensorConfigs;
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
    // const ModbusSlaveConfig *mslaveconfig  = modbusSlaveConfigs;
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

    if (SC16IS752_IRQ)
    {
        SC16IS752_IRQ = false;
        readSC16IS752();
    }

    bool allSensorsProcessedMessage = true;
    // Execute sensor state machines
    for (uint8_t i = 0; i < numOfSensors; i++)
    {
        sensors[i]->loop();
        allSensorsProcessedMessage &= sensors[i]->hasProcessedMessage();
    }

#ifdef MODBUS
    modbus->loop();
#endif

    webConf->doLoop();

    if (connected && deepSleepInterval > 0 && allSensorsProcessedMessage)
    {
        if (publisher.isConnected())
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

    if (Serial.available())
    {
        static char cmd[50];
        static uint8_t pos = 0;
        char c = Serial.read();
        if (pos < 50)
            cmd[pos++] = c;

        if (c == '\n')
        {
            cmd[pos] = '\0';
            serial_cmd(cmd);
            pos = 0;
        }
    }

    yield();
}

void status(WebServer *server)
{
    char buffer[1024], *b = buffer;
    b += sprintf(b, "{\n");
    b += sprintf(b, "  \"chipId\":\"%08X\",\n", ESP.getChipId());
    b += sprintf(b, "  \"uptime64\":%llu,\n", millis64());
    b += sprintf(b, "  \"uptime\":%lu,\n", millis());
    b += sprintf(b, "  \"lastMessageTime\":%llu,\n", lastMessageTime);
    b += sprintf(b, "  \"mqttConnected\":%u,\n", publisher.isConnected());
    b += sprintf(b, "  \"version\":\"%s\"\n", VERSION);
    b += sprintf(b, "}");
    server->send(200, "application/json", buffer);
}

void wifiConnected()
{
    DEBUG("WiFi connection established.");
    connected = true;
    publisher.connect();
}