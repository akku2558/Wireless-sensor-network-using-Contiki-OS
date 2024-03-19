#include "arch/dev/sensor/sht11/sht11-sensor.h"
#include "contiki.h"
#include "sys/etimer.h"
#include "sys/pt.h"
#include <stdio.h>
#include <math.h>

#include "net/routing/routing.h"
#include "random.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include <stdint.h>
#include <inttypes.h>

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define WITH_SERVER_REPLY  1
#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define SEND_INTERVAL     (5 * CLOCK_SECOND)

static struct simple_udp_connection udp_conn;
static uint32_t rx_count = 0;
static int display_temp;
static int display_humidity;

/*---------------------------------------------------------------------------*/
PROCESS(transmitting_sensor_data, "Reading and Transmitting temperature and humidity data From Udp client to server");

AUTOSTART_PROCESSES(&transmitting_sensor_data);
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  /* convert the received data to an integer */
  int received_data = 0;
  for(int i = 0; i <datalen; i++){
    received_data |= data[i] << (8 * i);
  }
  LOG_INFO("Received response '%d' from ", received_data);
  LOG_INFO_6ADDR(sender_addr);
  
  #if LLSEC802154_CONF_ENABLED
  LOG_INFO_(" LLSEC LV:%d", uipbuf_get_attr(UIPBUF_ATTR_LLSEC_LEVEL));
  #endif
  LOG_INFO_("\n");
  rx_count++;
}
/*---------------------------------------------------------------------------*/

//For Temperature sensor
static struct etimer et_temp_timer;
static struct pt pt_temp_thread;

//For Humidity sensor
static struct etimer et_humid_timer;
static struct pt pt_humidity_thread;

/*---------------------------------------------------------------------------*/

//protothread for reading temperature
PT_THREAD(read_temp(struct pt *pt)) {
    PT_BEGIN(pt);

    while(1) {

        // Activate the SHT11 sensor
        SENSORS_ACTIVATE(sht11_sensor);
        // Wait for the sensor to be ready
        PT_WAIT_UNTIL(pt, sht11_sensor.status(SENSORS_READY));
        // Read the raw temperature value
        int raw_temp = sht11_sensor.value(SHT11_SENSOR_TEMP);
        float calculated_temp = -39.60 + (raw_temp * 0.01);
        // this is from datasheet, double check the 14 bit reading
        display_temp = (int)calculated_temp;
        printf("Temperature: %d C\r\n", display_temp);
        // Deactivate the sensor to save power
        SENSORS_DEACTIVATE(sht11_sensor);
        // Set a timer to read the temperature every 5 seconds
        etimer_set(&et_temp_timer, CLOCK_SECOND * 5);
        // Wait until the timer expires
        PT_WAIT_UNTIL(pt, etimer_expired(&et_temp_timer));
    }
    PT_END(pt);
}

/*---------------------------------------------------------------------------*/

//protothread for reading humitidy
PT_THREAD(read_humidity(struct pt *pt))
{
  PT_BEGIN(pt);
  
  while(1) {
  
        // Activate the SHT11 sensor
        SENSORS_ACTIVATE(sht11_sensor);
        // Wait for the sensor to be ready
        PT_WAIT_UNTIL(pt, sht11_sensor.status(SENSORS_READY));
        // Read the raw humidity value
        int raw_humidity = sht11_sensor.value(SHT11_SENSOR_HUMIDITY);
        float calculated_humidity = -2.0468 + 0.0367 * raw_humidity + (-1.5955 * pow(10, -6)) * raw_humidity * raw_humidity;
        display_humidity = (int)calculated_humidity;
        printf("Humidity: %d %% \r\n", display_humidity);
        // Deactivate the sensor to save power
        SENSORS_DEACTIVATE(sht11_sensor);
        // Set a timer to read the humidity every 5 seconds
        etimer_set(&et_humid_timer, CLOCK_SECOND * 5);
        // Wait until the timer expires
        PT_WAIT_UNTIL(pt, etimer_expired(&et_humid_timer));
    }
    PT_END(pt);
}

/*---------------------------------------------------------------------------*/

//main function
PROCESS_THREAD(transmitting_sensor_data, ev, data)
{

    static struct etimer periodic_timer;
    static char str[64];
    uip_ipaddr_t dest_ipaddr;
    static uint32_t tx_count;
    static uint32_t missed_tx_count;
    PROCESS_BEGIN();
    
    //Initializing the sensor reading processes
    PT_INIT(&pt_temp_thread);
    PT_INIT(&pt_humidity_thread);
    
    /* Initialize UDP connection */
  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);

  etimer_set(&periodic_timer, random_rand() % SEND_INTERVAL);
  LOG_INFO("Client - senses the temperature and humidity and transmits /n");
    
    while(1) {
    
        // Execute the temperature reading protothread
        read_temp(&pt_temp_thread);
        
        //Execute the humidity reading protothread
        read_humidity(&pt_humidity_thread);
    
        PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer));

        if(NETSTACK_ROUTING.node_is_reachable() &&
          NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

          /* Print statistics every 10th TX */
          if(tx_count % 10 == 0) {
            LOG_INFO("Tx/Rx/MissedTx: %" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
                 tx_count, rx_count, missed_tx_count);
          }
          /* Send to DAG root */
          LOG_INFO("Sending Data to: ");
          LOG_INFO_6ADDR(&dest_ipaddr);
          LOG_INFO_("\n");

          /* Send the temperature and humidity data in a single variable to the DAG root */
          int data_to_send = display_temp * 100 + display_humidity;
          simple_udp_sendto(&udp_conn, &data_to_send, sizeof(data_to_send), &dest_ipaddr);
    
          tx_count++;
        } else {
          LOG_INFO("Not reachable yet\n");
          if(tx_count > 0) {
            missed_tx_count++;
          }
        }
        
  
      /* Add some delay */
      etimer_set(&periodic_timer, SEND_INTERVAL - CLOCK_SECOND + (random_rand() % (2 * CLOCK_SECOND)));
  
      // Pause this process to allow others to run
      PROCESS_PAUSE();
    }
    PROCESS_END();
}

/*---------------------------------------------------------------------------*/
