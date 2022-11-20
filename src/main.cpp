#include <Arduino.h>
#include "Config.h"
#include "Storage.h"
#include "Signature.h"
#include "AzureDpsClient.h"
#include "CliMode.h"
#include "DHT.h"
#include "Bitmap.h"
#include "Cert.h"
#include "Multichannel_Gas_GMXXX.h"
#include <TFT_eSPI.h>
#include <Wire.h>
#include <LIS3DHTR.h>
#include <rpcWiFiClientSecure.h>
#include <PubSubClient.h>
#include <WiFiUdp.h>
#include <NTP.h>
#include <az_json.h>
#include <az_result.h>
#include <az_span.h>
#include <az_iot_hub_client.h>

#define MQTT_PACKET_SIZE 1024

#define DHTPIN 0
#define DHTTYPE DHT11

LIS3DHTR<TwoWire> AccelSensor;
GAS_GMXXX<TwoWire> gas;
DHT dht(DHTPIN, DHTTYPE);
TFT_eSPI tft;
TFT_eSprite spr = TFT_eSprite(&tft);  //sprite

WiFiClientSecure wifi_client;
PubSubClient mqtt_client(wifi_client);
WiFiUDP wifi_udp;
NTP ntp(wifi_udp);

std::string HubHost;
std::string DeviceId;

#define AZ_RETURN_IF_FAILED(exp) \
  do \
  { \
    az_result const _result = (exp); \
    if (az_result_failed(_result)) \
    { \
      return _result; \
    } \
  } while (0)

////////////////////////////////////////////////////////////////////////////////
// 

#define DLM "\r\n"

static String StringVFormat(const char* format, va_list arg)
{
    const int len = vsnprintf(nullptr, 0, format, arg);
    char str[len + 1];
    vsnprintf(str, sizeof(str), format, arg);

    return String{ str };
}

static void Abort(const char* format, ...)
{
    va_list arg;
    va_start(arg, format);
    String str{ StringVFormat(format, arg) };
    va_end(arg);

    Serial.print(String::format("ABORT: %s" DLM, str.c_str()));

    while (true) {}
}

static void Log(const char* format, ...)
{
    va_list arg;
    va_start(arg, format);
    String str{ StringVFormat(format, arg) };
    va_end(arg);

    Serial.print(str);
}

////////////////////////////////////////////////////////////////////////////////
// Display

static void DisplayPrintf(const char* format, ...)
{
    va_list arg;
    va_start(arg, format);
    String str{ StringVFormat(format, arg) };
    va_end(arg);

    Log("%s\n", str.c_str());
    //tft.printf("%s\n", str.c_str());
}

////////////////////////////////////////////////////////////////////////////////
// Button

#include <AceButton.h>
using namespace ace_button;

enum class ButtonId
{
    RIGHT = 0,
    CENTER,
    LEFT,
};
static const int ButtonNumber = 3;
static AceButton Buttons[ButtonNumber];
static bool ButtonsClicked[ButtonNumber];

static void ButtonEventHandler(AceButton* button, uint8_t eventType, uint8_t buttonState)
{
    const uint8_t id = button->getId();
    if (ButtonNumber <= id) return;

    switch (eventType)
    {
    case AceButton::kEventClicked:
        switch (static_cast<ButtonId>(id))
        {
        case ButtonId::RIGHT:
            DisplayPrintf("Right button was clicked");
            break;
        case ButtonId::CENTER:
            digitalWrite(LCD_BACKLIGHT, HIGH);
            DisplayPrintf("Center button was clicked");
            break;
        case ButtonId::LEFT:
            digitalWrite(LCD_BACKLIGHT, LOW);
            DisplayPrintf("Left button was clicked");
            break;
        }
        ButtonsClicked[id] = true;
        break;
    }
}

static void ButtonInit()
{
    Buttons[static_cast<int>(ButtonId::RIGHT)].init(WIO_KEY_A, HIGH, static_cast<uint8_t>(ButtonId::RIGHT));
    Buttons[static_cast<int>(ButtonId::CENTER)].init(WIO_KEY_B, HIGH, static_cast<uint8_t>(ButtonId::CENTER));
    Buttons[static_cast<int>(ButtonId::LEFT)].init(WIO_KEY_C, HIGH, static_cast<uint8_t>(ButtonId::LEFT));

    ButtonConfig* buttonConfig = ButtonConfig::getSystemButtonConfig();
    buttonConfig->setEventHandler(ButtonEventHandler);
    buttonConfig->setFeature(ButtonConfig::kFeatureClick);

    for (int i = 0; i < ButtonNumber; ++i) ButtonsClicked[i] = false;
}

static void ButtonDoWork()
{
    for (int i = 0; static_cast<size_t>(i) < std::extent<decltype(Buttons)>::value; ++i)
    {
        Buttons[i].check();
    }
}

////////////////////////////////////////////////////////////////////////////////
// Azure IoT DPS

static AzureDpsClient DpsClient;
static unsigned long DpsPublishTimeOfQueryStatus = 0;

static void MqttSubscribeCallbackDPS(char* topic, byte* payload, unsigned int length);

static int RegisterDeviceToDPS(const std::string& endpoint, const std::string& idScope, const std::string& registrationId, const std::string& symmetricKey, const uint64_t& expirationEpochTime, std::string* hubHost, std::string* deviceId)
{
    std::string endpointAndPort{ endpoint };
    endpointAndPort += ":";
    endpointAndPort += std::to_string(8883);

    if (DpsClient.Init(endpointAndPort, idScope, registrationId) != 0) return -1;

    const std::string mqttClientId = DpsClient.GetMqttClientId();
    const std::string mqttUsername = DpsClient.GetMqttUsername();

    const std::vector<uint8_t> signature = DpsClient.GetSignature(expirationEpochTime);
    const std::string encryptedSignature = GenerateEncryptedSignature(symmetricKey, signature);
    const std::string mqttPassword = DpsClient.GetMqttPassword(encryptedSignature, expirationEpochTime);

    const std::string registerPublishTopic = DpsClient.GetRegisterPublishTopic();
    const std::string registerSubscribeTopic = DpsClient.GetRegisterSubscribeTopic();

    Log("DPS:" DLM);
    Log(" Endpoint = %s" DLM, endpoint.c_str());
    Log(" Id scope = %s" DLM, idScope.c_str());
    Log(" Registration id = %s" DLM, registrationId.c_str());
    Log(" MQTT client id = %s" DLM, mqttClientId.c_str());
    Log(" MQTT username = %s" DLM, mqttUsername.c_str());
    //Log(" MQTT password = %s" DLM, mqttPassword.c_str());

    wifi_client.setCACert(ROOT_CA_BALTIMORE);
    mqtt_client.setBufferSize(MQTT_PACKET_SIZE);
    mqtt_client.setServer(endpoint.c_str(), 8883);
    mqtt_client.setCallback(MqttSubscribeCallbackDPS);
    DisplayPrintf("Connecting to Azure IoT Hub DPS...");
    if (!mqtt_client.connect(mqttClientId.c_str(), mqttUsername.c_str(), mqttPassword.c_str())) return -2;

    mqtt_client.subscribe(registerSubscribeTopic.c_str());
    mqtt_client.publish(registerPublishTopic.c_str(), "{payload:{\"modelId\":\"" IOT_CONFIG_MODEL_ID "\"}}");

    while (!DpsClient.IsRegisterOperationCompleted())
    {
        mqtt_client.loop();
        if (DpsPublishTimeOfQueryStatus > 0 && millis() >= DpsPublishTimeOfQueryStatus)
        {
            mqtt_client.publish(DpsClient.GetQueryStatusPublishTopic().c_str(), "");
            Log("Client sent operation query message" DLM);
            DpsPublishTimeOfQueryStatus = 0;
        }
    }

    if (!DpsClient.IsAssigned()) return -3;

    mqtt_client.disconnect();

    *hubHost = DpsClient.GetHubHost();
    *deviceId = DpsClient.GetDeviceId();

    Log("Device provisioned:" DLM);
    Log(" Hub host = %s" DLM, hubHost->c_str());
    Log(" Device id = %s" DLM, deviceId->c_str());

    return 0;
}

static void MqttSubscribeCallbackDPS(char* topic, byte* payload, unsigned int length)
{
    Log("Subscribe:" DLM " %s" DLM " %.*s" DLM, topic, length, (const char*)payload);

    if (DpsClient.RegisterSubscribeWork(topic, std::vector<uint8_t>(payload, payload + length)) != 0)
    {
        Log("Failed to parse topic and/or payload" DLM);
        return;
    }

    if (!DpsClient.IsRegisterOperationCompleted())
    {
        const int waitSeconds = DpsClient.GetWaitBeforeQueryStatusSeconds();
        Log("Querying after %u  seconds..." DLM, waitSeconds);

        DpsPublishTimeOfQueryStatus = millis() + waitSeconds * 1000;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Azure IoT Hub

static az_iot_hub_client HubClient;

static int SendCommandResponse(az_iot_hub_client_method_request* request, uint16_t status, az_span response);
static void MqttSubscribeCallbackHub(char* topic, byte* payload, unsigned int length);

static int ConnectToHub(az_iot_hub_client* iot_hub_client, const std::string& host, const std::string& deviceId, const std::string& symmetricKey, const uint64_t& expirationEpochTime)
{
    static std::string deviceIdCache;
    deviceIdCache = deviceId;

    const az_span hostSpan{ az_span_create((uint8_t*)&host[0], host.size()) };
    const az_span deviceIdSpan{ az_span_create((uint8_t*)&deviceIdCache[0], deviceIdCache.size()) };
    az_iot_hub_client_options options = az_iot_hub_client_options_default();
    options.model_id = AZ_SPAN_LITERAL_FROM_STR(IOT_CONFIG_MODEL_ID);
    if (az_result_failed(az_iot_hub_client_init(iot_hub_client, hostSpan, deviceIdSpan, &options))) return -1;

    char mqttClientId[128];
    size_t client_id_length;
    if (az_result_failed(az_iot_hub_client_get_client_id(iot_hub_client, mqttClientId, sizeof(mqttClientId), &client_id_length))) return -4;

    char mqttUsername[256];
    if (az_result_failed(az_iot_hub_client_get_user_name(iot_hub_client, mqttUsername, sizeof(mqttUsername), NULL))) return -5;

    char mqttPassword[300];
    uint8_t signatureBuf[256];
    az_span signatureSpan = az_span_create(signatureBuf, sizeof(signatureBuf));
    az_span signatureValidSpan;
    if (az_result_failed(az_iot_hub_client_sas_get_signature(iot_hub_client, expirationEpochTime, signatureSpan, &signatureValidSpan))) return -2;
    const std::vector<uint8_t> signature(az_span_ptr(signatureValidSpan), az_span_ptr(signatureValidSpan) + az_span_size(signatureValidSpan));
    const std::string encryptedSignature = GenerateEncryptedSignature(symmetricKey, signature);
    az_span encryptedSignatureSpan = az_span_create((uint8_t*)&encryptedSignature[0], encryptedSignature.size());
    if (az_result_failed(az_iot_hub_client_sas_get_password(iot_hub_client, expirationEpochTime, encryptedSignatureSpan, AZ_SPAN_EMPTY, mqttPassword, sizeof(mqttPassword), NULL))) return -3;

    Log("Hub:" DLM);
    Log(" Host = %s" DLM, host.c_str());
    Log(" Device id = %s" DLM, deviceIdCache.c_str());
    Log(" MQTT client id = %s" DLM, mqttClientId);
    Log(" MQTT username = %s" DLM, mqttUsername);
    //Log(" MQTT password = %s" DLM, mqttPassword);

    wifi_client.setCACert(ROOT_CA_BALTIMORE);
    mqtt_client.setBufferSize(MQTT_PACKET_SIZE);
    mqtt_client.setServer(host.c_str(), 8883);
    mqtt_client.setCallback(MqttSubscribeCallbackHub);

    if (!mqtt_client.connect(mqttClientId, mqttUsername, mqttPassword)) return -6;

    mqtt_client.subscribe(AZ_IOT_HUB_CLIENT_METHODS_SUBSCRIBE_TOPIC);
    mqtt_client.subscribe(AZ_IOT_HUB_CLIENT_C2D_SUBSCRIBE_TOPIC);

    return 0;
}

static void DisplayTelemetry(float voc, float co, float no2, float c2h5ch, float t, float h) {

    //digitalWrite(LCD_BACKLIGHT, LOW);

    // display voc
    spr.createSprite(40, 30);
    spr.fillSprite(TFT_BLACK);
    spr.setFreeFont(&FreeSansBoldOblique12pt7b);
    spr.setTextColor(TFT_WHITE);
    spr.drawFloat(voc, 0, 0, 1);
    spr.pushSprite(15, 100);
    spr.deleteSprite();

    // display co
    spr.createSprite(40, 30);
    spr.setFreeFont(&FreeSansBoldOblique12pt7b);
    spr.setTextColor(TFT_WHITE);
    spr.drawFloat(co, 0, 0, 1);
    spr.setTextColor(TFT_GREEN);
    spr.pushSprite(15, 185);
    spr.deleteSprite();

    // display temperature
    spr.createSprite(30, 30);
    spr.setFreeFont(&FreeSansBoldOblique12pt7b);
    spr.setTextColor(TFT_WHITE);
    spr.drawFloat(t, 0, 0, 1);
    spr.setTextColor(TFT_GREEN);
    spr.pushSprite((tft.width() / 2) - 1, 100);
    spr.deleteSprite();

    // display no2
    spr.createSprite(45, 30);
    spr.setFreeFont(&FreeSansBoldOblique12pt7b);
    spr.setTextColor(TFT_WHITE);
    spr.drawFloat(no2, 0, 0, 1);
    spr.pushSprite(((tft.width() / 2) + (tft.width() / 2) / 2), 97);
    spr.deleteSprite();

    // display humidity
    spr.createSprite(30, 30);
    spr.setFreeFont(&FreeSansBoldOblique12pt7b);
    spr.setTextColor(TFT_WHITE);
    spr.drawNumber(h, 0, 0, 1);
    spr.pushSprite((tft.width() / 2) - 1, (tft.height() / 2) + 67);
    spr.deleteSprite();

    // display c2h5ch
    spr.createSprite(45, 30);
    spr.setFreeFont(&FreeSansBoldOblique12pt7b);
    spr.setTextColor(TFT_WHITE);
    spr.drawFloat(c2h5ch, 0 , 0, 1);
    spr.pushSprite(((tft.width() / 2) + (tft.width() / 2) / 2), (tft.height() / 2) + 67);
    spr.deleteSprite();

}

static az_result SendTelemetry()
{
    float accelX, accelY, accelZ;
    AccelSensor.getAcceleration(&accelX, &accelY, &accelZ);
    float humidity;
    uint32_t val = 0;
    float no2, c2h5ch, voc, co;
    int light;
    float temperature;
    
    light = analogRead(WIO_LIGHT) * 100 / 1023;

    // get multichannel gas sensor data

    // VOC
    val = gas.getGM502B();
    if (val > 999) val = 999;
    voc = gas.calcVol(val);    

    // CO
    val = gas.getGM702B();
    if (val > 999) val = 999;
    co = gas.calcVol(val);

    // Temperature
    temperature = dht.readTemperature();

    // NO2
    val = gas.getGM102B();
    if (val > 999) val = 999;
    no2 = gas.calcVol(val);

    // Humidity
    humidity = dht.readHumidity();
    if (humidity > 99.9) humidity = 99.9;

    // Ethyl
    val = gas.getGM302B();
    if (val > 999) val = 999;
    c2h5ch = gas.calcVol(val);


    char creationTime[20 + 1];  // yyyy-mm-ddThh:mm:ss.sssZ
    {
        const time_t t = ntp.epoch();
        struct tm tm;
        gmtime_r(&t, &tm);
        const int len = snprintf(creationTime, sizeof(creationTime), "%d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (len != sizeof(creationTime) - 1)
        {
            Log("Failed snprintf" DLM);
            return AZ_ERROR_NOT_SUPPORTED;
        }
    }

    az_iot_message_properties props;
    uint8_t propsBuffer[128];
    if (az_result_failed(az_iot_message_properties_init(&props, az_span_create(propsBuffer, sizeof(propsBuffer)), 0)))
    {
        Log("Failed az_iot_message_properties_init" DLM);
        return AZ_ERROR_NOT_SUPPORTED;
    }
    if (az_result_failed(az_iot_message_properties_append(&props, AZ_SPAN_FROM_STR("iothub-creation-time-utc"), az_span_create(reinterpret_cast<uint8_t*>(creationTime), strlen(creationTime)))))
    {
        Log("Failed az_iot_message_properties_append" DLM);
        return AZ_ERROR_NOT_SUPPORTED;
    }

    char telemetry_topic[128];
    if (az_result_failed(az_iot_hub_client_telemetry_get_publish_topic(&HubClient, &props, telemetry_topic, sizeof(telemetry_topic), NULL)))
    {
        Log("Failed az_iot_hub_client_telemetry_get_publish_topic" DLM);
        return AZ_ERROR_NOT_SUPPORTED;
    }

    az_json_writer json_builder;
    char telemetry_payload[200];
    AZ_RETURN_IF_FAILED(az_json_writer_init(&json_builder, AZ_SPAN_FROM_BUFFER(telemetry_payload), NULL));
    AZ_RETURN_IF_FAILED(az_json_writer_append_begin_object(&json_builder));
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_ACCEL_X)));
    AZ_RETURN_IF_FAILED(az_json_writer_append_double(&json_builder, accelX, 3));
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_ACCEL_Y)));
    AZ_RETURN_IF_FAILED(az_json_writer_append_double(&json_builder, accelY, 3));
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_ACCEL_Z)));
    AZ_RETURN_IF_FAILED(az_json_writer_append_double(&json_builder, accelZ, 3));
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_LIGHT)));
    AZ_RETURN_IF_FAILED(az_json_writer_append_int32(&json_builder, light));
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_TEMP)));
    AZ_RETURN_IF_FAILED(az_json_writer_append_double(&json_builder, temperature, 2));
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_HUMID)));
    AZ_RETURN_IF_FAILED(az_json_writer_append_double(&json_builder, humidity, 2));
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_CO)));
    AZ_RETURN_IF_FAILED(az_json_writer_append_double(&json_builder, co, 2));
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_VOC)));
    AZ_RETURN_IF_FAILED(az_json_writer_append_double(&json_builder, voc, 2));
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_NO2)));
    AZ_RETURN_IF_FAILED(az_json_writer_append_double(&json_builder, no2, 2));
    AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_C2H5CH)));
    AZ_RETURN_IF_FAILED(az_json_writer_append_double(&json_builder, c2h5ch, 2));
    AZ_RETURN_IF_FAILED(az_json_writer_append_end_object(&json_builder));
    const az_span out_payload{ az_json_writer_get_bytes_used_in_destination(&json_builder) };

    static int sendCount = 0;
    if (!mqtt_client.publish(telemetry_topic, az_span_ptr(out_payload), az_span_size(out_payload), false))
    {
        DisplayPrintf("ERROR: Send telemetry %d", sendCount);
    }
    else
    {
        ++sendCount;
        DisplayPrintf("Sent telemetry %d", sendCount);
    }

    DisplayTelemetry(voc, co, no2, c2h5ch, temperature, humidity); // display values
    return AZ_OK;
}



static az_result SendButtonTelemetry(ButtonId id)
{
    char creationTime[20 + 1];  // yyyy-mm-ddThh:mm:ss.sssZ
    {
        const time_t t = ntp.epoch();
        struct tm tm;
        gmtime_r(&t, &tm);
        const int len = snprintf(creationTime, sizeof(creationTime), "%d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (len != sizeof(creationTime) - 1)
        {
            Log("Failed snprintf" DLM);
            return AZ_ERROR_NOT_SUPPORTED;
        }
    }

    az_iot_message_properties props;
    uint8_t propsBuffer[128];
    if (az_result_failed(az_iot_message_properties_init(&props, az_span_create(propsBuffer, sizeof(propsBuffer)), 0)))
    {
        Log("Failed az_iot_message_properties_init" DLM);
        return AZ_ERROR_NOT_SUPPORTED;
    }
    if (az_result_failed(az_iot_message_properties_append(&props, AZ_SPAN_FROM_STR("iothub-creation-time-utc"), az_span_create(reinterpret_cast<uint8_t*>(creationTime), strlen(creationTime)))))
    {
        Log("Failed az_iot_message_properties_append" DLM);
        return AZ_ERROR_NOT_SUPPORTED;
    }

    char telemetry_topic[128];
    if (az_result_failed(az_iot_hub_client_telemetry_get_publish_topic(&HubClient, &props, telemetry_topic, sizeof(telemetry_topic), NULL)))
    {
        Log("Failed az_iot_hub_client_telemetry_get_publish_topic" DLM);
        return AZ_ERROR_NOT_SUPPORTED;
    }

    az_json_writer json_builder;
    char telemetry_payload[200];
    AZ_RETURN_IF_FAILED(az_json_writer_init(&json_builder, AZ_SPAN_FROM_BUFFER(telemetry_payload), NULL));
    AZ_RETURN_IF_FAILED(az_json_writer_append_begin_object(&json_builder));
    switch (id)
    {
    case ButtonId::RIGHT:
        AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_RIGHT_BUTTON)));
        AZ_RETURN_IF_FAILED(az_json_writer_append_string(&json_builder, AZ_SPAN_LITERAL_FROM_STR("click")));
        break;
    case ButtonId::CENTER:
        AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_CENTER_BUTTON)));
        AZ_RETURN_IF_FAILED(az_json_writer_append_string(&json_builder, AZ_SPAN_LITERAL_FROM_STR("click")));
        break;
    case ButtonId::LEFT:
        AZ_RETURN_IF_FAILED(az_json_writer_append_property_name(&json_builder, AZ_SPAN_LITERAL_FROM_STR(TELEMETRY_LEFT_BUTTON)));
        AZ_RETURN_IF_FAILED(az_json_writer_append_string(&json_builder, AZ_SPAN_LITERAL_FROM_STR("click")));
        break;
    default:
        return AZ_ERROR_ARG;
    }
    AZ_RETURN_IF_FAILED(az_json_writer_append_end_object(&json_builder));
    const az_span out_payload{ az_json_writer_get_bytes_used_in_destination(&json_builder) };

    if (!mqtt_client.publish(telemetry_topic, az_span_ptr(out_payload), az_span_size(out_payload), false))
    {
        DisplayPrintf("ERROR: Send button telemetry");
    }
    else
    {
        DisplayPrintf("Sent button telemetry");
    }

    return AZ_OK;
}

static void HandleCommandMessage(az_span payload, az_iot_hub_client_method_request* command_request)
{
    int command_res_code = 200;
    az_result rc = AZ_OK;

    if (az_span_is_content_equal(AZ_SPAN_LITERAL_FROM_STR(COMMAND_RING_BUZZER), command_request->name))
    {
        // Parse the command payload (it contains a 'duration' field)
        Log("Processing command 'ringBuzzer'" DLM);
        char buffer[32];
        az_span_to_str(buffer, 32, payload);
        Log("Raw command payload: %s" DLM, buffer);

        az_json_reader json_reader;
        uint32_t duration;
        if (az_json_reader_init(&json_reader, payload, NULL) == AZ_OK)
        {
            if (az_json_reader_next_token(&json_reader) == AZ_OK)
            {
                if (az_result_failed(rc = az_json_token_get_uint32(&json_reader.token, &duration)))
                {
                    Log("Couldn't parse JSON token res=%d" DLM, rc);
                }
                else
                {
                    Log("Duration: %dms" DLM, duration);
                }
            }

            // Invoke command
            analogWrite(WIO_BUZZER, 128);
            delay(duration);
            analogWrite(WIO_BUZZER, 0);

            int rc;
            if (az_result_failed(rc = SendCommandResponse(command_request, command_res_code, AZ_SPAN_LITERAL_FROM_STR("{}"))))
            {
                Log("Unable to send %d response, status 0x%08x" DLM, command_res_code, rc);
            }
        }
    }
    else
    {
        // Unsupported command
        Log("Unsupported command received: %.*s." DLM, az_span_size(command_request->name), az_span_ptr(command_request->name));

        int rc;
        if (az_result_failed(rc = SendCommandResponse(command_request, 404, AZ_SPAN_LITERAL_FROM_STR("{}"))))
        {
            printf("Unable to send %d response, status 0x%08x\n", 404, rc);
        }
    }
}

static int SendCommandResponse(az_iot_hub_client_method_request* request, uint16_t status, az_span response)
{
    az_result rc = AZ_OK;
    // Get the response topic to publish the command response
    char commands_response_topic[128];
    if (az_result_failed(rc = az_iot_hub_client_methods_response_get_publish_topic(&HubClient, request->request_id, status, commands_response_topic, sizeof(commands_response_topic), NULL)))
    {
        Log("Unable to get method response publish topic" DLM);
        return rc;
    }

    Log("Status: %u\tPayload: '", status);
    char* payload_char = (char*)az_span_ptr(response);
    if (payload_char != NULL)
    {
        for (int32_t i = 0; i < az_span_size(response); i++)
        {
            Log("%c", *(payload_char + i));
        }
    }
    Log("'" DLM);

    // Send the commands response
    if (mqtt_client.publish(commands_response_topic, az_span_ptr(response), az_span_size(response), false))
    {
        Log("Sent response" DLM);
    }

    return rc;
}

static void MqttSubscribeCallbackHub(char* topic, byte* payload, unsigned int length)
{
    az_span topic_span = az_span_create((uint8_t *)topic, strlen(topic));
    az_iot_hub_client_method_request command_request;

    if (az_result_succeeded(az_iot_hub_client_methods_parse_received_topic(&HubClient, topic_span, &command_request)))
    {
        DisplayPrintf("Command arrived!");
        // Determine if the command is supported and take appropriate actions
        HandleCommandMessage(az_span_create(payload, length), &command_request);
    }

    Log(DLM);
}

void setup_display() {

    //Head
    tft.setRotation(3);
    tft.fillScreen(TFT_BLACK);
    tft.setFreeFont(&FreeSansBoldOblique18pt7b);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Air Quality", 70, 10 , 1);

    //Line
    for (int8_t line_index = 0; line_index < 5 ; line_index++)
    {
    tft.drawLine(0, 50 + line_index, tft.width(), 50 + line_index, TFT_GREEN);
    }

    //VCO & CO Rect
    tft.drawRoundRect(5, 60, (tft.width() / 2) - 20 , tft.height() - 65 , 10, TFT_WHITE); // L1

    //VCO Text
    tft.setFreeFont(&FreeSansBoldOblique12pt7b);
    tft.setTextColor(TFT_RED);
    tft.drawString("VOC", 7 , 65 , 1);
    tft.setTextColor(TFT_GREEN);
    tft.drawString("ppm", 55, 108, 1);

    //CO Text
    tft.setFreeFont(&FreeSansBoldOblique12pt7b);
    tft.setTextColor(TFT_RED);
    tft.drawString("CO", 7 , 150 , 1);
    tft.setTextColor(TFT_GREEN);
    tft.drawString("ppm", 55, 193, 1);

    // Temp rect
    tft.drawRoundRect((tft.width() / 2) - 10  , 60, (tft.width() / 2) / 2 , (tft.height() - 65) / 2 , 10, TFT_BLUE); // s1
    tft.setFreeFont(&FreeSansBoldOblique9pt7b);
    tft.setTextColor(TFT_RED) ;
    tft.drawString("Temp", (tft.width() / 2) - 1  , 70 , 1); // Print the test text in the custom font
    tft.setTextColor(TFT_GREEN);
    tft.drawString("o", (tft.width() / 2) + 30, 95, 1);
    tft.drawString("C", (tft.width() / 2) + 40, 100, 1);

    //No2 rect
    tft.drawRoundRect(((tft.width() / 2) + (tft.width() / 2) / 2) - 5  , 60, (tft.width() / 2) / 2 , (tft.height() - 65) / 2 , 10, TFT_BLUE); // s2
    tft.setFreeFont(&FreeSansBoldOblique9pt7b);
    tft.setTextColor(TFT_RED);
    tft.drawString("NO2", ((tft.width() / 2) + (tft.width() / 2) / 2)   , 70 , 1); // Print the test text in the custom font
    tft.setTextColor(TFT_GREEN);
    tft.drawString("ppm", ((tft.width() / 2) + (tft.width() / 2) / 2) + 30 , 120, 1);

    //Humi Rect
    tft.drawRoundRect((tft.width() / 2) - 10 , (tft.height() / 2) + 30, (tft.width() / 2) / 2 , (tft.height() - 65) / 2 , 10, TFT_BLUE); // s3
    tft.setFreeFont(&FreeSansBoldOblique9pt7b);
    tft.setTextColor(TFT_RED) ;
    tft.drawString("Humi", (tft.width() / 2) - 1 , (tft.height() / 2) + 40 , 1); // Print the test text in the custom font
    tft.setTextColor(TFT_GREEN);
    tft.drawString("%", (tft.width() / 2) + 30, (tft.height() / 2) + 70, 1);

    //c2h5ch Rect
    tft.drawRoundRect(((tft.width() / 2) + (tft.width() / 2) / 2) - 5  , (tft.height() / 2) + 30, (tft.width() / 2) / 2 , (tft.height() - 65) / 2 , 10, TFT_BLUE); // s4
    tft.setFreeFont(&FreeSansBoldOblique9pt7b);
    tft.setTextColor(TFT_RED) ;
    tft.drawString("Ethyl", ((tft.width() / 2) + (tft.width() / 2) / 2)   , (tft.height() / 2) + 40 , 1); // Print the test text in the custom font
    tft.setTextColor(TFT_GREEN);
    tft.drawString("ppm", ((tft.width() / 2) + (tft.width() / 2) / 2) + 30 , (tft.height() / 2) + 90, 1);

}

////////////////////////////////////////////////////////////////////////////////
// setup and loop

void setup()
{
    ////////////////////
    // Load storage

    Storage::Load();

    ////////////////////
    // Init I/O

    Serial.begin(115200);

    pinMode(WIO_BUZZER, OUTPUT);

    ////////////////////
    // Display Logo

    tft.begin();
    tft.setRotation(3);
    tft.fillScreen(TFT_WHITE);
    tft.pushImage((tft.width() - SeeedstudioBitmapWidth) / 2, (tft.height() - SeeedstudioBitmapHeight) / 2, SeeedstudioBitmapWidth, SeeedstudioBitmapHeight, SeeedstudioBitmap);
    delay(2000);

    setup_display();

    ////////////////////
    // Enter configuration mode

    pinMode(WIO_KEY_A, INPUT_PULLUP);
    pinMode(WIO_KEY_B, INPUT_PULLUP);
    pinMode(WIO_KEY_C, INPUT_PULLUP);
    delay(100);

    if (digitalRead(WIO_KEY_A) == LOW &&
        digitalRead(WIO_KEY_B) == LOW &&
        digitalRead(WIO_KEY_C) == LOW   )
    {
        DisplayPrintf("In configuration mode");
        CliMode();
    }

    ////////////////////
    // Init sensor

    AccelSensor.begin(Wire1);
    AccelSensor.setOutputDataRate(LIS3DHTR_DATARATE_25HZ);
    AccelSensor.setFullScaleRange(LIS3DHTR_RANGE_2G);

    gas.begin(Wire, 0x08);
    dht.begin();
    
    ButtonInit();

    ////////////////////
    // Connect Wi-Fi

    DisplayPrintf("Connecting to SSID: %s", IOT_CONFIG_WIFI_SSID);
    do
    {
        Log(".");
        WiFi.begin(IOT_CONFIG_WIFI_SSID, IOT_CONFIG_WIFI_PASSWORD);
        delay(500);
    }
    while (WiFi.status() != WL_CONNECTED);
    DisplayPrintf("Connected");

    ////////////////////
    // Sync time server
    
    ntp.begin();

    ////////////////////
    // Provisioning

    #if defined(USE_CLI) || defined(USE_DPS)

        if (RegisterDeviceToDPS(IOT_CONFIG_GLOBAL_DEVICE_ENDPOINT, IOT_CONFIG_ID_SCOPE, IOT_CONFIG_REGISTRATION_ID, IOT_CONFIG_SYMMETRIC_KEY, ntp.epoch() + TOKEN_LIFESPAN, &HubHost, &DeviceId) != 0)
        {
            Abort("RegisterDeviceToDPS()");
        }

    #else

        HubHost = IOT_CONFIG_IOTHUB;
        DeviceId = IOT_CONFIG_DEVICE_ID;

    #endif // USE_CLI || USE_DPS
}

void loop()
{
    ButtonDoWork();

    static uint64_t reconnectTime;
    if (!mqtt_client.connected())
    {
        Log("Connecting to Azure IoT Hub...");
        const uint64_t now = ntp.epoch();
        if (ConnectToHub(&HubClient, HubHost, DeviceId, IOT_CONFIG_SYMMETRIC_KEY, now + TOKEN_LIFESPAN) != 0)
        {
            //DisplayPrintf("> ERROR.");
            Log("> ERROR. Status code =%d. Try again in 5 seconds." DLM, mqtt_client.state());
            delay(5000);
            return;
        }

        Log("> SUCCESS.");
        reconnectTime = now + TOKEN_LIFESPAN * 0.85;
    }
    else
    {
        if ((uint64_t)ntp.epoch() >= reconnectTime)
        {
            Log("Disconnect");
            mqtt_client.disconnect();
            return;
        }

        mqtt_client.loop();

        static unsigned long nextTelemetrySendTime = 0;
        if (millis() > nextTelemetrySendTime)
        {
            Log("Sending Telemetry...");
            SendTelemetry();
            nextTelemetrySendTime = millis() + TELEMETRY_FREQUENCY_MILLISECS;
        }

        for (int i = 0; i < ButtonNumber; ++i)
        {
            if (ButtonsClicked[i])
            {
                SendButtonTelemetry(static_cast<ButtonId>(i));
                ButtonsClicked[i] = false;
            }
        }
    }
}
