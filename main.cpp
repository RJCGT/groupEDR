/*
 * Copyright (c) 2020, CATIE
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "mbed.h"
#include <nsapi_dns.h>
#include <MQTTClientMbedOs.h>
#include "bme280.h"

using namespace sixtron;

namespace
{
#define GROUP_NUMBER "groupEDR"
#define MQTT_TOPIC_PUBLISH_PRES "RGJCT/feeds/groupedr.press"
#define MQTT_TOPIC_PUBLISH_TEMP "RGJCT/feeds/groupedr.temp"
#define MQTT_TOPIC_PUBLISH_HUM "RGJCT/feeds/groupedr.hum"
#define MQTT_TOPIC_SUBSCRIBE_ECL "RGJCT/feeds/groupedr.eclairage"
#define MQTT_TOPIC_SUBSCRIBE_BTN "RGJCT/feeds/groupedr.pressbtn"
#define MQTT_TOPIC_SUBSCRIBE_VIT "RGJCT/feeds/groupedr.vitled"
#define SYNC_INTERVAL 1
#define MQTT_CLIENT_ID "aio_lAfI78cgx5Fh0fBml6XGPsYUpuEa"
}

// Peripherals
static DigitalOut led(LED1);
static InterruptIn button(BUTTON1);
I2C bus(I2C1_SDA, I2C1_SCL);
BME280 bme = BME280(&bus, BME280::I2CAddress::Address1);
// Network
NetworkInterface *network;
MQTTClient *client;

// MQTT
// const char* hostname = "fd9f:590a:b158::1";
const char *hostname = "io.adafruit.com";
int port = 1883;
bool btn = false;
// Error code
nsapi_size_or_error_t rc = 0;

// Event queue
static int id_yield;
static EventQueue main_queue(32 * EVENTS_EVENT_SIZE);

/*!
 *  \brief Called when a message is received
 *
 *  Print messages received on mqtt topic
 */
void messageArrived(MQTT::MessageData &md)
{
    MQTT::Message &message = md.message;
    printf("Message arrived: qos %d, retained %d, dup %d, packetid %d\r\n", message.qos, message.retained, message.dup, message.id);
    printf("Payload %.*s\r\n", message.payloadlen, (char *)message.payload);

    // Get the payload string
    char *char_payload = (char *)malloc((message.payloadlen + 1) * sizeof(char)); // allocate the necessary size for our buffer
    char_payload = (char *)message.payload;                                       // get the arrived payload in our buffer
    char_payload[message.payloadlen] = '\0';                                      // String must be null terminated

    // Compare our payload with known command strings
    if (strcmp(char_payload, "ON") == 0)
    {
        led = 1;
    }
    else if (strcmp(char_payload, "OFF") == 0)
    {
        led = 0;
    }
    else if (strcmp(char_payload, "RESET") == 0)
    {
        printf("RESETTING ...\n");
        system_reset();
    }
}

/*!
 *  \brief Yield to the MQTT client
 *
 *  On error, stop publishing and yielding
 */
static void yield()
{
    // printf("Yield\n");

    rc = client->yield(100);

    if (rc != 0)
    {
        printf("Yield error: %d\n", rc);
        main_queue.cancel(id_yield);
        main_queue.break_dispatch();
        system_reset();
    }
}

/*!
 *  \brief Publish data over the corresponding adafruit MQTT topic
 *
 */
static int8_t pressure()
{
    float pres = bme.pressure();
    printf("Pressure : %.2f Pa \n",pres);
    char *presPayload = "101542.00";
    
    MQTT::Message messagepres;
    messagepres.qos = MQTT::QOS1;
    messagepres.retained = false;
    messagepres.dup = false;
    messagepres.payload = (void *)presPayload ;
    messagepres.payloadlen = strlen(presPayload );

    printf("Send: %s to MQTT Broker: %s\n", presPayload , hostname);
    rc = client->publish(MQTT_TOPIC_PUBLISH_PRES, messagepres);
    if (rc != 0)
    {
        printf("Failed to publish: %d\n", rc);
        return rc;
    }

    return 0;
}

static int8_t temphum()
{
    float temp = bme.temperature();
    printf("Temperature : %.2f °C \n",temp);
    float hum = bme.humidity();
    printf("Humidity : %.2f % \n", hum);

    char *tempPayload = "10.43";

    char *humPayload = "40.32";
    
    MQTT::Message messagetemp;
    messagetemp.qos = MQTT::QOS1;
    messagetemp.retained = false;
    messagetemp.dup = false;
    messagetemp.payload = (void *)tempPayload;
    messagetemp.payloadlen = strlen(tempPayload);

    MQTT::Message messagehum;
    messagehum.qos = MQTT::QOS1;
    messagehum.retained = false;
    messagehum.dup = false;
    messagehum.payload = (void *)humPayload;
    messagehum.payloadlen = strlen(humPayload);

    printf("Send: %s to MQTT Broker: %s\n", tempPayload, hostname);
    rc = client->publish(MQTT_TOPIC_PUBLISH_TEMP, messagetemp);
    if (rc != 0)
    {
        printf("Failed to publish: %d\n", rc);
        return rc;
    }
    ThisThread::sleep_for(500ms);
    printf("Send: %s to MQTT Broker: %s\n", humPayload, hostname);
    rc = client->publish(MQTT_TOPIC_PUBLISH_HUM, messagehum);
    if (rc != 0)
    {
        printf("Failed to publish: %d\n", rc);
        return rc;
    }

    return 0;
}


// main() runs in its own thread in the OS
// (note the calls to ThisThread::sleep_for below for delays)

int main()
{
    bme.initialize();
    bme.set_sampling(BME280::SensorMode::NORMAL, BME280::SensorSampling::OVERSAMPLING_X1, BME280::SensorSampling::OVERSAMPLING_X1, BME280::SensorSampling::OVERSAMPLING_X1, BME280::SensorFilter::OFF, BME280::StandbyDuration::MS_0_5);

    printf("Connecting to border router...\n");

    /* Get Network configuration */
    network = NetworkInterface::get_default_instance();

    if (!network)
    {
        printf("Error! No network interface found.\n");
        return 0;
    }

    /* Add DNS */
    nsapi_addr_t new_dns = {
        NSAPI_IPv6,
        {0xfd, 0x9f, 0x59, 0x0a, 0xb1, 0x58, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x01}};
    nsapi_dns_add_server(new_dns, "LOWPAN");

    /* Border Router connection */
    rc = network->connect();
    if (rc != 0)
    {
        printf("Error! net->connect() returned: %d\n", rc);
        return rc;
    }

    /* Print IP address */
    SocketAddress a;
    network->get_ip_address(&a);
    printf("IP address: %s\n", a.get_ip_address() ? a.get_ip_address() : "None");

    /* Open TCP Socket */
    TCPSocket socket;
    SocketAddress address;
    network->gethostbyname(hostname, &address);
    address.set_port(port);

    /* MQTT Connection */
    client = new MQTTClient(&socket);
    socket.open(network);
    rc = socket.connect(address);
    if (rc != 0)
    {
        printf("Connection to Adafruit Failed\n");
        return rc;
    }

    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    data.MQTTVersion = 4;
    data.keepAliveInterval = 25;
    data.clientID.cstring = "6TRON";
    data.username.cstring = (char *)"RGJCT";
    data.password.cstring = (char *)"aio_lAfI78cgx5Fh0fBml6XGPsYUpuEa";
    if (client->connect(data) != 0)
    {
        printf("Connection to Adafruit Failed\n");
    }

    printf("Connected to Adafruit\n");

    /* MQTT Subscribe */
    if ((rc = client->subscribe(MQTT_TOPIC_SUBSCRIBE_BTN, MQTT::QOS0, messageArrived)) != 0)
    {
        printf("rc from MQTT subscribe is %d\r\n", rc);
    }
    printf("Subscribed to Topic: %s\n", MQTT_TOPIC_SUBSCRIBE_BTN);

    if ((rc = client->subscribe(MQTT_TOPIC_SUBSCRIBE_ECL, MQTT::QOS0, messageArrived)) != 0)
    {
        printf("rc from MQTT subscribe is %d\r\n", rc);
    }
    printf("Subscribed to Topic: %s\n", MQTT_TOPIC_SUBSCRIBE_ECL);

    if ((rc = client->subscribe(MQTT_TOPIC_SUBSCRIBE_VIT, MQTT::QOS0, messageArrived)) != 0)
    {
        printf("rc from MQTT subscribe is %d\r\n", rc);
    }
    printf("Subscribed to Topic: %s\n", MQTT_TOPIC_SUBSCRIBE_VIT);

    yield();

    // Yield every 1 second
    id_yield = main_queue.call_every(SYNC_INTERVAL * 1000, yield);

    // Publish
    
    main_queue.call_every(SYNC_INTERVAL*5000,temphum);
    button.fall(main_queue.event(pressure));
    main_queue.dispatch_forever();
}
