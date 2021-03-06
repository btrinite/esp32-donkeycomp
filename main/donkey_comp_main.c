/* Hello World Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_attr.h"
#include "soc/rtc.h"
#include "driver/ledc.h"
#include "driver/mcpwm.h"
#include "driver/uart.h"
#include "driver/rmt.h"
#include <driver/adc.h>
#include "esp_adc_cal.h"

#include "soc/mcpwm_reg.h"
#include "soc/mcpwm_struct.h"
#include "esp_system.h"
#include "math.h"
#include "esp_spi_flash.h"
#include "esp_timer.h"

#include "string.h"

#define MAX_GPIO 40
#define STACK_SIZE 200

// PIN used to drive NeoPixel LEDs
#include "ws2812_control.h"

#define GPIO_OUTPUT_LED    16 /*Keep it consistent with ws2812_control.h !!*/
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_OUTPUT_LED))// How many NeoPixels are attached to the Arduino?

#define TIMESTEPS      8 

// Statuses
#define INT_DISCONNECTED  0
#define INT_RXERROR       1
#define INT_CALIBRATE     2
#define HOST_INIT         3
#define HOST_MODE_USER    4
#define HOST_MODE_LOCAL   5
#define HOST_MODE_DISARMED   6
 

//Each 50ms, check and output value to serial link
#define OUTPUTLOOP 30
#define INTPUTLOOP 10
#define PWM_FREQ 125
// Global var used to capture Rx signal
unsigned int pwm_steering_value = 0;
unsigned int pwm_throttle_value = 0;
unsigned int pwm_ch5_value = 0;
unsigned int pwm_ch6_value = 0;
unsigned int pwm_speedometer_value = 0;
unsigned int freq_value = 0;
unsigned int prev_steering_time = 0;
unsigned int prev_throttle_time = 0;
unsigned int prev_ch5_time = 0;
unsigned int prev_ch6_time = 0;
unsigned int prev_speedometer_time = 0;
unsigned int prev_freq_time = 0;

// Gloval var used to detect signal activity
int steering_toggle = 0;
int throttle_toggle = 0;
int ch5_toggle = 0;
int ch6_toggle = 0;
int speedometer_toggle = 0;

int cmd_throttle = 1500;
int cmd_steering = 1500;
// GLobal buffer for serial output
char buff [50] = {};

/* Sensor */
#define DISTANCE_FRONT_LEFT ADC1_CHANNEL_6
#define DISTANCE_FRONT_RIGHT ADC1_CHANNEL_7
unsigned int dst_sensor_left = 0;
unsigned int dst_sensor_right = 0;

/*PWM based on */

#define PWM_RC_THROTTLE_OUTUT_PIN 32   //Set GPIO 15 as PWM0A
#define PWM_RC_STEERING_OUTUT_PIN 33   //Set GPIO 15 as PWM1A

#define RMT_CLK_DIV      100    /*!< RMT counter clock divider */

// PIN used to connect Rx receiver
#define PWM_RC_THROTTLE_INPUT_PIN   25   
#define PWM_RC_STEERING_INPUT_PIN   26   
#define PWM_RC_CH5_INPUT_PIN        27
#define PWM_RC_CH6_INPUT_PIN        14
#define PWM_SPEEDOMETER_INPUT_PIN   17

#define GPIO_INPUT_PIN_SEL  ((1ULL<<PWM_RC_STEERING_INPUT_PIN) | (1ULL<<PWM_RC_THROTTLE_INPUT_PIN) | (1ULL<<PWM_RC_CH5_INPUT_PIN) | (1ULL<<PWM_RC_CH6_INPUT_PIN) | (1ULL<<PWM_SPEEDOMETER_INPUT_PIN))

//
// PWM Output
//

static void mcpwm_gpio_initialize()
{
    printf("initializing mcpwm gpio...\n");
    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, PWM_RC_THROTTLE_OUTUT_PIN);
    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM1A, PWM_RC_STEERING_OUTUT_PIN);
  }

/**
 * @brief motor moves in forward direction, with duty cycle = duty %
 */
static void mcpwm_set_throttle_pwm(int pwm_width_in_us)
{
  mcpwm_set_duty_in_us(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, pwm_width_in_us);
}

/**
 * @brief motor moves in backward direction, with duty cycle = duty %
 */
static void mcpwm_set_steering_pwm(int pwm_width_in_us)
{
  mcpwm_set_duty_in_us(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, pwm_width_in_us);
}

/**
 * @brief Configure MCPWM module for brushed dc motor
 */
static void mcpwm_init_control()
{
    //1. mcpwm gpio initialization
    mcpwm_gpio_initialize();

    //2. initial mcpwm configuration
    printf("Configuring Initial Parameters of mcpwm...\n");
    mcpwm_config_t pwm_config;
    pwm_config.frequency = PWM_FREQ;    //frequency = 500Hz,
    pwm_config.cmpr_a = (float)((1500.0*100.0)/(1000000/PWM_FREQ));    //duty cycle of PWMxA = 0
    pwm_config.cmpr_b = 0;    //duty cycle of PWMxb = 0
    pwm_config.counter_mode = MCPWM_UP_COUNTER;
    pwm_config.duty_mode = MCPWM_DUTY_MODE_0;
    mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_config);    //Configure PWM0A & PWM0B with above settings
    mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_1, &pwm_config);    //Configure PWM1A & PWM1B with above settings
    mcpwm_set_duty_type(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
    mcpwm_set_duty_type(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, MCPWM_DUTY_MODE_0);
}

#define ESP_INTR_FLAG_DEFAULT 0

typedef struct {
  uint32_t t0;
  uint32_t t1;
} SIGNAL_TIMING;

SIGNAL_TIMING pwm_timing[MAX_GPIO];
uint32_t pwm_length[MAX_GPIO];

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{    
    portENTER_CRITICAL_ISR(&mux);
    uint32_t gpio_num = (uint32_t) arg;
    uint32_t t = esp_timer_get_time();
    if (gpio_get_level(gpio_num) == 1) {
      // rising edge
      pwm_timing[gpio_num].t0 = t;
    } else {
      //falling edge
      pwm_timing[gpio_num].t1 = t;
      if (t >= pwm_timing[gpio_num].t0)
      pwm_length[gpio_num] = pwm_timing[gpio_num].t1 - pwm_timing[gpio_num].t0;
    }
    portEXIT_CRITICAL_ISR(&mux);
}

//
// PWM INPUT
//

void init_rx_gpio (void)
{
     gpio_config_t io_conf;
     //interrupt of rising edge
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode    
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(PWM_RC_THROTTLE_INPUT_PIN, gpio_isr_handler, (void*) PWM_RC_THROTTLE_INPUT_PIN);
    gpio_isr_handler_add(PWM_RC_STEERING_INPUT_PIN, gpio_isr_handler, (void*) PWM_RC_STEERING_INPUT_PIN);
    gpio_isr_handler_add(PWM_RC_CH5_INPUT_PIN, gpio_isr_handler, (void*) PWM_RC_CH5_INPUT_PIN);
    gpio_isr_handler_add(PWM_RC_CH6_INPUT_PIN, gpio_isr_handler, (void*) PWM_RC_CH6_INPUT_PIN);
    gpio_isr_handler_add(PWM_SPEEDOMETER_INPUT_PIN, gpio_isr_handler, (void*) PWM_SPEEDOMETER_INPUT_PIN);
}

void init_led_gpio(void)
{
    gpio_config_t io_conf;
    //disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_LED;
    //disable pull-down mode
    io_conf.pull_down_en = 1;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);
}
// --------------------------------------
// LED Part
// --------------------------------------

#define COMPCOLOR(r, g, b) (g<<16)|(r<<8)|(b)

struct Led {
  unsigned char r;
  unsigned char g;
  unsigned char b;
  unsigned char timing;
} ;

struct Led leds[NUM_LEDS];
struct led_state led_new_state;

void switchOffLed() {
  for(int i=0;i<NUM_LEDS;i++){
    led_new_state.leds[0]=COMPCOLOR(0,0,0);
    ws2812_write_leds(led_new_state);
  }    
}

void updateLed(void * pvParameters ) {
  static int seq = 0;

  while(1) {
    for(int i=0;i<NUM_LEDS;i++) {
      if (leds[i].timing>>seq & 0x01) {
        led_new_state.leds[0]=COMPCOLOR(leds[i].r,leds[i].g,leds[i].b);
      } else {
        led_new_state.leds[0]=COMPCOLOR(0,0,0);
      }
      ws2812_write_leds(led_new_state);
    }  
    seq=(seq+1)%TIMESTEPS;
    vTaskDelay((1000/TIMESTEPS) / portTICK_PERIOD_MS);
  }
  vTaskDelete( NULL );
}

void setLed (unsigned char lednum, unsigned char r, unsigned char g, unsigned char b, unsigned char timing) {
  if (lednum < NUM_LEDS) {
    leds[lednum].r=r;
    leds[lednum].g=g;
    leds[lednum].b=b;
    leds[lednum].timing = timing;
  }
}

void displayStatusOnLED (int status)
{
  static int _last_status = -1;
  if (status != _last_status) {
    _last_status = status;
#ifdef DEBUG
    Serial.print("New status : ");
    Serial.println(status);
#endif
    if (status==INT_CALIBRATE) {
      // fast white blink
      setLed (0,0xff,0xff,0xff,0x55);
    }
    if (status==HOST_INIT) {
      // Slow red blink
      setLed (0,0xff,0x00,0x0,0x18);
    }
    if (status==HOST_MODE_USER) {
      // Slow green blink
      setLed (0,0x00,0xff,0x00,0x18);
    }
    if (status==HOST_MODE_LOCAL) {
      // Slow blue blink
      setLed (0,0x00,0x00,0xFF,0x18);
    }
    if (status==INT_DISCONNECTED) {
      // fast red blink
      setLed (0,0xff,0x00,0x00,0x55);
    }
    if (status==INT_RXERROR) {
      // slow red blink
      setLed (0,0xff,0x00,0x00,0x33);
    }
    if (status==HOST_MODE_DISARMED) {
      // fast green blink
      setLed (0,0x00,0xff,0x00,0x55);
    }
  }
}
void processStatusFromHost (const char *status) {

  if (strcmp(status, "init")==0) {
    displayStatusOnLED(HOST_INIT);
  }
  if (strcmp(status, "disarmed")==0) {
    displayStatusOnLED(HOST_MODE_DISARMED);
  }
  if (strcmp(status, "user")==0) {
    displayStatusOnLED(HOST_MODE_USER);
  }
  if (strcmp(status, "local")==0) {
    displayStatusOnLED(HOST_MODE_LOCAL);
  }
}

void timedCheckOutput()
{
  uint32_t t = (esp_timer_get_time()/1000)%50000;
  if (pwm_length[PWM_RC_THROTTLE_INPUT_PIN] == 0) {
    sprintf(buff, "%d,-1,-1,-1,-1,-1,-1,-1\n", t);
     displayStatusOnLED(INT_RXERROR);   
  } else {
    sprintf(buff, "%d,%d,%d,%d,%d,%d,%d,%d\n", t, 
    pwm_length[PWM_RC_THROTTLE_INPUT_PIN], 
    pwm_length[PWM_RC_STEERING_INPUT_PIN], 
    pwm_length[PWM_RC_CH5_INPUT_PIN], 
    pwm_length[PWM_RC_CH6_INPUT_PIN],
    pwm_length[PWM_SPEEDOMETER_INPUT_PIN],
    dst_sensor_left,
    dst_sensor_right);
  }
  printf(buff);    
  memset(pwm_length, 0, sizeof(pwm_length[0])*MAX_GPIO);   
}

#define UART_RX_BUFF_SIZE 100
static char cmd[UART_RX_BUFF_SIZE];
char  * linep = cmd;
int len=UART_RX_BUFF_SIZE;
int getline() {
    int c;

    for(;;) {
        c = fgetc(stdin);
        if(c == EOF)
            return 0;

        if((*linep++ = c) == '\n')
            break;

        if(--len <= 1) {
          break;        
        }

    }
    *linep = '\0';
    linep = cmd;
    len=UART_RX_BUFF_SIZE;
    return len;
}

void parseCommand (void) {
  int a = 1500;
  int b = 1500;
  char c[200];
  if (sscanf (cmd, "%d,%d,%s", &a, &b, c) == 3) {
    cmd_throttle = a;
    cmd_steering = b;
    processStatusFromHost(c);
  }
}

void get_sensor();

void readCommand(void * pvParameters ) {
  static int seq = 0;
  while(1) {
    get_sensor();
    if (getline(cmd, sizeof(cmd)) > 0 ) {
      parseCommand();
      mcpwm_set_throttle_pwm(cmd_throttle);
      mcpwm_set_steering_pwm(cmd_steering);
    }
    vTaskDelay(INTPUTLOOP / portTICK_PERIOD_MS);
  }
  vTaskDelete( NULL );
}
//
// MAIN
//

void sensor_init()
{
  adc1_config_width(ADC_WIDTH_BIT_9);
  adc1_config_channel_atten(DISTANCE_FRONT_LEFT,ADC_ATTEN_DB_11);
  adc1_config_channel_atten(DISTANCE_FRONT_RIGHT,ADC_ATTEN_DB_11);
}

void get_sensor()
{
  dst_sensor_left =  adc1_get_raw(DISTANCE_FRONT_LEFT);
  dst_sensor_right =  adc1_get_raw(DISTANCE_FRONT_RIGHT);
}

void app_main()
{
  int tick=0;
  struct led_state new_state;
  static uint8_t ucParameterToPass;
  TaskHandle_t xHandle = NULL;
  
  uart_set_baudrate(UART_NUM_0, 2000000); 
  const int uart_buffer_size = (1024 * 2);

  memset (leds, 0, sizeof(leds));
  mcpwm_init_control();
  init_rx_gpio();
  init_led_gpio();
  ws2812_control_init();
  sensor_init();

  switchOffLed();
  displayStatusOnLED(INT_DISCONNECTED);   
  xTaskCreate(&updateLed, "led_task", 2048, NULL, 5, NULL);
  xTaskCreate(&readCommand, "rxuart_task", 2048, NULL, 5, NULL);


  mcpwm_set_throttle_pwm(1500);
  mcpwm_set_steering_pwm(1500);
  // Set configuration of timer0 for high speed channels
  //
  while(1) {
    vTaskDelay(OUTPUTLOOP / portTICK_PERIOD_MS);
    timedCheckOutput();
  };
}
