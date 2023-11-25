#ifndef MQTT_PUBLISHER_H
#define MQTT_PUBLISHER_H

#include "config.h"
#include "debug.h"
#include <Ticker.h>

#include <AsyncMqttClient.h>
#include <string.h>
#include <sml/sml_file.h>

#ifdef MODBUS
#include "modbus.h"
#endif

#define MQTT_RECONNECT_DELAY 5
#define MQTT_LWT_TOPIC "LWT"
#define MQTT_LWT_RETAIN true
#define MQTT_LWT_QOS 2
#define MQTT_LWT_PAYLOAD_ONLINE "Online"
#define MQTT_LWT_PAYLOAD_OFFLINE "Offline"

using namespace std;

struct MqttConfig
{
  char server[128] = "mosquitto";
  char port[8] = "1883";
  char username[128] = "";
  char password[128] = "";
  char topic[128] = "iot/smartmeter/";
  char jsonPayload[9] = "";
};

class MqttPublisher
{
public:
  void setup(MqttConfig _config)
  {
    config = _config;
    uint8_t lastCharOfTopic = strlen(config.topic) - 1;
    baseTopic = String(config.topic) + (lastCharOfTopic >= 0 && config.topic[lastCharOfTopic] == '/' ? "" : "/");
    lastWillTopic = String(baseTopic + MQTT_LWT_TOPIC);

    DEBUG(F("MQTT: Setting up..."));
    DEBUG(F("MQTT: Server: %s"), config.server);
    DEBUG(F("MQTT: Port: %d"), atoi(config.port));
    DEBUG(F("MQTT: Username: %s"), config.username);
    DEBUG(F("MQTT: Password: <hidden>"));
    DEBUG(F("MQTT: Topic: %s"), baseTopic.c_str());

    client.setServer(const_cast<const char *>(config.server), atoi(config.port));
    if (strlen(config.username) > 0 || strlen(config.password) > 0)
    {
      client.setCredentials(config.username, config.password);
    }
    client.setCleanSession(true);
    if (config.jsonPayload[0] == 's')
    {
      if (lastWillJsonPayload != 0)
      {
        delete lastWillJsonPayload;
      }
      lastWillJsonPayload = new_json_wrap(lastWillTopic.c_str(), MQTT_LWT_PAYLOAD_OFFLINE);
      client.setWill(lastWillTopic.c_str(), MQTT_LWT_QOS, MQTT_LWT_RETAIN, lastWillJsonPayload);
    }
    else
    {
      client.setWill(lastWillTopic.c_str(), MQTT_LWT_QOS, MQTT_LWT_RETAIN, MQTT_LWT_PAYLOAD_OFFLINE);
    }
    client.setKeepAlive(MQTT_RECONNECT_DELAY * 3);
    this->registerHandlers();
  }

  void debug(const char *message)
  {
    publish(baseTopic + "debug", message);
  }

  void info(const char *message)
  {
    publish(baseTopic + "info", message);
  }

  void publish(Sensor *sensor, sml_file *file)
  {
    if (sensor->config->status_led_pin != NOT_A_PIN)
    {
      if (file->messages_len != 0)
        sensor->status_led->Blink(40, 40).Repeat(2).Update();
    }

    for (int i = 0; i < file->messages_len; i++)
    {
      sml_message *message = file->messages[i];
      if (*message->message_body->tag == SML_MESSAGE_GET_LIST_RESPONSE)
      {
        sml_list *entry;
        sml_get_list_response *body;
        body = (sml_get_list_response *)message->message_body->data;
        for (entry = body->val_list; entry != NULL; entry = entry->next)
        {
          if (!entry->value)
          { // do not crash on null value
            continue;
          }

          char obisIdentifier[32];
          char buffer[255];

          sprintf(obisIdentifier, "%d-%d:%d.%d.%d/%d",
                  entry->obj_name->str[0], entry->obj_name->str[1],
                  entry->obj_name->str[2], entry->obj_name->str[3],
                  entry->obj_name->str[4], entry->obj_name->str[5]);

          String entryTopic = baseTopic + "sensor/" + (sensor->config->name) + "/obis/" + obisIdentifier + "/";

          if (((entry->value->type & SML_TYPE_FIELD) == SML_TYPE_INTEGER) ||
              ((entry->value->type & SML_TYPE_FIELD) == SML_TYPE_UNSIGNED))
          {
            double value = sml_value_to_double(entry->value);
            int scaler = (entry->scaler) ? *entry->scaler : 0;
            int prec = -scaler;
            if (prec < 0)
              prec = 0;
            value = value * pow(10, scaler);
            sprintf(buffer, "%.*f", prec, value);
            publish(entryTopic + "value", buffer);
          }
          else if (!sensor->config->numeric_only)
          {
            if (entry->value->type == SML_TYPE_OCTET_STRING)
            {
              char *value;
              sml_value_to_strhex(entry->value, &value, true);
              publish(entryTopic + "value", value);
              free(value);
            }
            else if (entry->value->type == SML_TYPE_BOOLEAN)
            {
              publish(entryTopic + "value", entry->value->data.boolean ? "true" : "false");
            }
          }
        }
      }
    }
  }

#ifdef MODBUS
  void publish(uint8_t index, ModbusSlaveConfig *slave)
  {
    char buffer[80];
    String entryTopic = baseTopic + "modbus/" + slave->name + "/";

    if (index == MODBUS_PUBLISH_ERROR)
    {
      snprintf(buffer, 80, "{\"success\":%d,\"fail\":%d}", slave->cntsuccess, slave->cnterrors);
      publish(entryTopic + "error", buffer);

      switch (slave->lasterror)
      {
      case SDM_ERR_NO_ERROR:
        strcpy(buffer, "none");
        break;
      case SDM_ERR_ILLEGAL_FUNCTION:
        strcpy(buffer, "illegal function");
        break;
      case SDM_ERR_ILLEGAL_DATA_ADDRESS:
        strcpy(buffer, "illegal data address");
        break;
      case SDM_ERR_ILLEGAL_DATA_VALUE:
        strcpy(buffer, "illegal data value");
        break;
      case SDM_ERR_SLAVE_DEVICE_FAILURE:
        strcpy(buffer, "slave device failure");
        break;
      case SDM_ERR_CRC_ERROR:
        strcpy(buffer, "crc error");
        break;
      case SDM_ERR_WRONG_BYTES:
        strcpy(buffer, "wrong bytes");
        break;
      case SDM_ERR_NOT_ENOUGHT_BYTES:
        strcpy(buffer, "not enough bytes");
        break;
      case SDM_ERR_TIMEOUT:
        strcpy(buffer, "timeout");
        break;
      case SDM_ERR_EXCEPTION:
        strcpy(buffer, "exception");
        break;
      default:
        sprintf(buffer, "unknown: %d", slave->lasterror);
      };
      publish(entryTopic + "last_error", buffer);
    }
    else if (index == MODBUS_PUBLISH_ID_SERIAL)
    {
      sprintf(buffer, "%d", slave->id);
      publish(entryTopic + "id", buffer, 0, true);

      if (slave->serial != 0)
      {
        sprintf(buffer, "%u", slave->serial);
        publish(entryTopic + "serial", buffer, 0, true);
      }
    }
    else
    {
      sprintf(buffer, "%.*f", sdmarr[index].prec, sdmarr[index].regvalarr);
      publish(entryTopic + (char *)(sdmarr[index].name), buffer);
      sdmarr[index].regvalarr = NAN;
    }
  }
#endif

  void connect()
  {
    if (this->connected)
    {
      DEBUG(F("MQTT: Already connected. Aborting connection request."));
      return;
    }
    DEBUG(F("MQTT: Connecting to broker..."));
    client.connect();
  }

  void disconnect()
  {
    if (!this->connected)
    {
      DEBUG(F("MQTT: Not connected. Aborting disconnect request."));
      return;
    }
    DEBUG(F("MQTT: Disconnecting from broker..."));
    client.disconnect();
  }

  bool isConnected()
  {
    return connected;
  }

private:
  bool connected = false;
  MqttConfig config;
  AsyncMqttClient client;
  Ticker reconnectTimer;
  String baseTopic;
  String lastWillTopic;
  const char *lastWillJsonPayload = 0;

  void publish(const String &topic, const String &payload, uint8_t qos = 0, bool retain = false)
  {
    publish(topic.c_str(), payload.c_str(), qos, retain);
  }
  void publish(String &topic, const char *payload, uint8_t qos = 0, bool retain = false)
  {
    publish(topic.c_str(), payload, qos, retain);
  }
  void publish(const char *topic, const String &payload, uint8_t qos = 0, bool retain = false)
  {
    publish(topic, payload.c_str(), qos, retain);
  }

  void publish(const char *topic, const char *payload, uint8_t qos = 0, bool retain = false)
  {
    if (this->connected)
    {
      DEBUG(F("MQTT: Publishing to %s:"), topic);
      if (config.jsonPayload[0] == 's')
      {
        const char *buf = new_json_wrap(topic, payload);
        DEBUG(F("%s\n"), buf);
        client.publish(topic, qos, retain, buf, strlen(buf));
        delete buf;
      }
      else
      {
        DEBUG(F("%s"), payload);
//        digitalWrite(D6, LOW);
        client.publish(topic, qos, retain, payload, strlen(payload));
//        digitalWrite(D6, HIGH);
      }
    }
  }

  const char *new_json_wrap(const char *topic, const char *payload)
  {
    const char *subtopic = topic + baseTopic.length();
    size_t bufsize = strlen(payload) + strlen(subtopic) + 8;
    char *buf = new char[bufsize];
    bool payloadIsNumber = true;
    for (const char *i = payload; *i != '\0'; i++)
    {
      if (!isdigit(*i))
      {
        payloadIsNumber = false;
        break;
      }
    }
    if (payloadIsNumber)
    {
      sniprintf(buf, bufsize, "{\"%s\":%s}", subtopic, payload);
    }
    else
    {
      sniprintf(buf, bufsize, "{\"%s\":\"%s\"}", subtopic, payload);
    }
    return buf;
  }

  void registerHandlers()
  {
    client.onConnect([this](bool sessionPresent)
                     {
      this->connected = true;
      this->reconnectTimer.detach();
      DEBUG(F("MQTT: Connection established."));
      char message[64];
      snprintf(message, 64, "Hello from %08X, running SMLReader version %s.", ESP.getChipId(), VERSION);
      info(message);
      publish(baseTopic + MQTT_LWT_TOPIC, MQTT_LWT_PAYLOAD_ONLINE, MQTT_LWT_QOS, MQTT_LWT_RETAIN); });
    client.onDisconnect([this](AsyncMqttClientDisconnectReason reason)
                        {
      this->connected = false;
      DEBUG(F("MQTT: Disconnected. Reason: %d."), reason);
      reconnectTimer.attach(MQTT_RECONNECT_DELAY, [this]() {
        if (WiFi.isConnected()) {
          this->connect();
        }
      }); });
  }
};

#endif