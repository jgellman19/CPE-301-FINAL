/*
CPE 301 Final Project - Swamp Cooler
Jacob Gellman, Blake Smith, Erica Nichols
Spring 2024
*/

#include <DHT.h>
#include <DHT_U.h>

// *** MACROS ***
#define RDA 0x80
#define TBE 0x20
// for pin manipulation!
#define WRITE_HIGH(address, pin_num)  address |= (0x01 << pin_num);
#define WRITE_LOW(address, pin_num)  address &= ~(0x01 << pin_num);
#define PIN_READ(address, pin_num) (address & (1 << pin_num)) != 0;

// *** INCLUDES ***
// LCD DISPLAY
#include <LiquidCrystal.h>
const int RS = 22, EN = 24, D4 = 3, D5 = 4, D6 = 5, D7 = 6;
LiquidCrystal lcd(RS, EN, D4, D5, D6, D7);
bool displayData;

// STEPPER
#include <Stepper.h>
const int stepsPerRevolution = 2038;
Stepper myStepper = Stepper(stepsPerRevolution, 39, 41, 43, 45); // 1N1 = 39, 1N4 = 45

// RTC
#include <Wire.h>
#include <RTClib.h>
RTC_DS1307 rtc;
DateTime now;

String date_time_to_str(DateTime obj){
  String output = String(obj.year()) + "-" + 
                  String(obj.month()) + "-" + 
                  String(obj.day()) + " " + 
                  String(obj.hour()) + ":" + 
                  String(obj.minute()) + ":" + 
                  String(obj.second());
  return output;
}

// *** REGISTERS ***
// SERIAL TRANSMISSION 
volatile unsigned char *myUCSR0A = (unsigned char *)0x00C0;
volatile unsigned char *myUCSR0B = (unsigned char *)0x00C1;
volatile unsigned char *myUCSR0C = (unsigned char *)0x00C2;
volatile unsigned int  *myUBRR0  = (unsigned int *) 0x00C4;
volatile unsigned char *myUDR0   = (unsigned char *)0x00C6;
 
// ADC
volatile unsigned char* my_ADMUX = (unsigned char*) 0x7C;
volatile unsigned char* my_ADCSRB = (unsigned char*) 0x7B;
volatile unsigned char* my_ADCSRA = (unsigned char*) 0x7A;
volatile unsigned int* my_ADC_DATA = (unsigned int*) 0x78;

// Timer Pointers
volatile unsigned char *myTCCR1A  = 0x80;
volatile unsigned char *myTCCR1B  = 0x81;
volatile unsigned char *myTCCR1C  = 0x82;
volatile unsigned char *myTIMSK1  = 0x6F;
volatile unsigned char *myTIFR1   = 0x36;
volatile unsigned int  *myTCNT1   = 0x84;

// GPIO
// LEDS - 7: PH4 (RED), 8: PH5 (YELLOW), 9: PH6 (GREEN), 10: PB4 (BLUE)
// BUTTONS - 12: PB6 (STOP), 13: PB7 (RESET)
// FAN CONTROL - 31: PC6 (IN4), 33: PC4 (IN3), 35: PC2 (ON/OFF/SPEED)
// for port Hs
volatile unsigned char* port_h = (unsigned char*) 0x102;
volatile unsigned char* ddr_h = (unsigned char*) 0x101;
volatile unsigned char* pin_h = (unsigned char*) 0x100;
// for port Bs
volatile unsigned char* port_b = (unsigned char*) 0x25;
volatile unsigned char* ddr_b = (unsigned char*) 0x24;
volatile unsigned char* pin_b = (unsigned char*) 0x23;
// for port Cs
volatile unsigned char* port_c = (unsigned char*) 0x28;
volatile unsigned char* ddr_c = (unsigned char*) 0x27;
volatile unsigned char* pin_c = (unsigned char*) 0x26;

// Button flags
bool startButton;
bool resetButton;
bool stopButton;

// ISR
const int startButtonPin = 2;
const int interruptNumber = digitalPinToInterrupt(startButtonPin);

volatile unsigned long overflowCounter = 0; // Counter for the overflows
volatile unsigned long delayCounter = 0; // Counter for main loop delay
volatile unsigned long buttonCounter = 0;
const unsigned long overflowsPerMinute = 60000 / 4.1; // Calculate overflows in one minute
const unsigned long overflowsPer500ms = 500 / 4.1;
bool readData = true;
bool readButtons = true;
ISR(TIMER1_OVF_vect){
  overflowCounter++;
  delayCounter++;
  buttonCounter++;
  if (overflowCounter >= overflowsPerMinute){
    readData = true;
    overflowCounter = 0;
  }
  if (buttonCounter >= overflowsPer500ms){
    readButtons = true;
    buttonCounter = true;
  }
}

void my_delay(unsigned long duration){
  unsigned long delayThreshold = duration / 4.1;
  delayCounter = 0;
  while (delayCounter < delayThreshold) {} // wait until delay finishes
}
void gpio_init()
{
  // LEDs are OUTPUT
  // 7: PH4 (RED), 8: PH5 (YELLOW), 9: PH6 (GREEN), 10: PB4 (BLUE)
  WRITE_HIGH(*ddr_h, 4); // PH4 ddr HIGH (output)
  WRITE_HIGH(*ddr_h, 5); // PH5 ddr HIGH (output)
  WRITE_HIGH(*ddr_h, 6); // PH6 ddr HIGH (output)
  WRITE_HIGH(*ddr_b, 4); // PB4 ddr HIGH (output)

  // INIT TO OFF
  WRITE_LOW(*port_h, 4); // RED
  WRITE_LOW(*port_h, 5); // YELLOW
  WRITE_LOW(*port_h, 6); // GREEN
  WRITE_LOW(*port_b, 4); // BLUE

  // BUTTONS are INPUT (pull-up resistor)
  // BUTTONS - 11: PB5 (), 12: PB6 (), 13: PB7 ()
  // WRITE_LOW(*ddr_b, 5); // START
  WRITE_LOW(*ddr_b, 6); // STOP
  WRITE_LOW(*ddr_b, 7); // RESET

  // INIT PULL UP RESISTOR
  // WRITE_HIGH(*port_b, 5); // START
  WRITE_HIGH(*port_b, 6); // STOP
  WRITE_HIGH(*port_b, 7); // RESET

  // FAN IS OUTPUT
  // FAN CONTROL - 31: PC6 (IN4), 33: PC4 (IN3), 35: PC2 (ON/OFF/SPEED)
  WRITE_HIGH(*ddr_c, 6);
  WRITE_HIGH(*ddr_c, 4);
  WRITE_HIGH(*ddr_c, 2);
}

// ISR setup function
void isr_setup(){
  pinMode(startButtonPin, INPUT_PULLUP);
  attachInterrupt(interruptNumber, handleStartPress, FALLING);
}

// handle start button press
void handleStartPress(){
  startButton = true;
}

// Timer setup function
void setup_timer_regs()
{
  // disable global interrupts
  cli();
  // setup the timer control registers
  *myTCCR1A= 0x00;
  *myTCCR1B= 0X00;
  *myTCCR1C= 0x00;
  
  // reset the TOV flag
  *myTIFR1 |= 0x01;
  
  // enable the TOV interrupt
  *myTIMSK1 |= 0x01;

  // start timer
  *myTCCR1B |= 0x01;

  // enable global interrupts
  sei();
}

void rtc_init()
{
  Wire.begin();
  rtc.begin();
  if (!rtc.isrunning()) {
    Serial.println("RTC is NOT running!");
    // Set the RTC to the date and time this sketch was compiled
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
}

void stepper_init()
{
  myStepper.setSpeed(15);
  desiredPos = currentPos; // dont move on startup!
}

void control_fan(bool on)
{
  // FAN CONTROL - 31: PC6 (IN4), 33: PC4 (IN3), 35: PC2 (ENB)
  // IN3 HIGH, IN4 LOW -> Forward
  // IN3 LOW, IN4 HIGH -> BACKWARD
  // ENB HIGH -> ON, LOW -> OFF
  if (on){
    WRITE_LOW(*port_c, 6);
    WRITE_HIGH(*port_c, 4);
    WRITE_HIGH(*port_c, 2);
  }
  else{
    WRITE_LOW(*port_c, 2);
  }
}
void adc_init()
{
  // setup the A register
  *my_ADCSRA |= 0b10000000; // set bit   7 to 1 to enable the ADC
  *my_ADCSRA &= 0b11011111; // clear bit 6 to 0 to disable the ADC trigger mode
  *my_ADCSRA &= 0b11110111; // clear bit 5 to 0 to disable the ADC interrupt
  *my_ADCSRA &= 0b11111000; // clear bit 0-2 to 0 to set prescaler selection to slow reading
  // setup the B register
  *my_ADCSRB &= 0b11110111; // clear bit 3 to 0 to reset the channel and gain bits
  *my_ADCSRB &= 0b11111000; // clear bit 2-0 to 0 to set free running mode
  // setup the MUX Register
  *my_ADMUX  &= 0b01111111; // clear bit 7 to 0 for AVCC analog reference
  *my_ADMUX  |= 0b01000000; // set bit   6 to 1 for AVCC analog reference
  *my_ADMUX  &= 0b11011111; // clear bit 5 to 0 for right adjust result
  *my_ADMUX  &= 0b11100000; // clear bit 4-0 to 0 to reset the channel and gain bits
}

unsigned int adc_read(unsigned char adc_channel_num)
{
  // clear the channel selection bits (MUX 4:0)
  *my_ADMUX  &= 0b11100000;
  // clear the channel selection bits (MUX 5)
  *my_ADCSRB &= 0b11110111;
  // set the channel number
  if(adc_channel_num > 7)
  {
    // set the channel selection bits, but remove the most significant bit (bit 3)
    adc_channel_num -= 8;
    // set MUX bit 5
    *my_ADCSRB |= 0b00001000;
  }
  // set the channel selection bits
  *my_ADMUX  += adc_channel_num;
  // set bit 6 of ADCSRA to 1 to start a conversion
  *my_ADCSRA |= 0x40;
  // wait for the conversion to complete
  while((*my_ADCSRA & 0x40) != 0);
  // return the result in the ADC data register
  return *my_ADC_DATA;
}