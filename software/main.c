//
// main.c : AVR uC code for flukso sensor board
//
// Copyright (c) 2008-2009 jokamajo.org
//               2010      flukso.net
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
// $Id$

#include <string.h>
#include <stdlib.h>

#include "wiring/wiring_private.h"

#include "main.h"

#include <avr/io.h>
// pin/register/ISR definitions
#include <avr/interrupt.h>

// eeprom library
#include <avr/eeprom.h>

// watchdog timer library
#include <avr/wdt.h>

// variable declarations
volatile struct state aux[4] = {{false, false, false, START, 0}, {false, false, false, START, 0}, {false, false, false, START, 0}, {false, false, false, START, 0}};

volatile struct sensor EEMEM EEPROM_measurements[4] = {{SENSOR0, START}, {SENSOR1, START}, {SENSOR2, START}, {SENSOR3, START}};
volatile struct sensor measurements[4];

volatile struct time_struct time = {false, 0};

volatile uint8_t muxn = 0;
volatile uint16_t timer = 0;

// interrupt service routine for INT0
ISR(INT0_vect) {
  pulse_add(&measurements[2], &aux[2], PULSE_CONST_2, PULSE_HALF_2);
}

// interrupt service routine for INT1
ISR(INT1_vect) {
  pulse_add(&measurements[3], &aux[3], PULSE_CONST_3, PULSE_HALF_3);
}

// interrupt service routine for PCI2 (PCINT20)
/**
ISR(PCINT2_vect) {
  if (aux[4].toggle == false) {
    aux[4].toggle = true;
  }
  else {
    pulse_add(&measurements[4], &aux[4], PULSE_CONST_4, PULSE_HALF_4);
  }
}
**/

void pulse_add(volatile struct sensor *measurement, volatile struct state *aux, uint32_t pulse_const, uint32_t pulse_half) {
  measurement->value += pulse_const;

  if (aux->half == true) {
    measurement->value += 1;
  }

  if (pulse_half) {
    aux->half = !aux->half;
  }

  aux->pulse = true;
  aux->time  = time.ms;
}

// interrupt service routine for ADC
ISR(TIMER2_COMPA_vect) {
#if DBG > 0
  PORTD |= (1<<PD4);
#endif
  // read ADC result
  // add to nano(Wh) counter
#if PHASE == 2
  MacU16X16to32(aux[0].nano, METERCONST, ADC);
#else
  MacU16X16to32(aux[muxn].nano, METERCONST, ADC);
#endif
  if (aux[muxn].nano > WATT) {
     measurements[muxn].value++;
     aux[muxn].pulse = true;
     aux[muxn].nano -= WATT;
     aux[muxn].pulse_count++;
  }

  if (timer == SECOND) {
    aux[muxn].nano_start = aux[muxn].nano_end;
    aux[muxn].nano_end = aux[muxn].nano;
    aux[muxn].pulse_count_final = aux[muxn].pulse_count;
    aux[muxn].pulse_count = 0;
    aux[muxn].power = true;
  }

  // cycle through the available ADC input channels (0 and 1)
  muxn++;
  if (!(muxn &= 0x1)) timer++;
  if (timer > SECOND) timer = 0;

  // We have timer interrupts occcuring at a frequency of 1250Hz.
  // In order to map this to 1000Hz (=ms) we have to skip every fifth interrupt.
  if (!time.skip) time.ms++;
  time.skip = (((time.ms & 0x3) == 3) && !time.skip) ? true : false;

  ADMUX &= 0xF8;
  ADMUX |= muxn;
  // start a new ADC conversion
  ADCSRA |= (1<<ADSC);

#if DBG > 0
  PORTD &= ~(1<<PD4);
#endif

#if DBG > 1
  aux[muxn].nano = WATT+1;
  timer = SECOND;
#endif
}
 
// interrupt service routine for analog comparator
ISR(ANALOG_COMP_vect) {
  uint8_t i;

  //debugging:
  //measurements[3].value = END3;
  //measurements[2].value = END2;
  //measurements[1].value = END1;
  //measurements[0].value = END0;

  //disable uC sections to consume less power while writing to EEPROM

  //disable UART Tx and Rx:
  UCSR0B &= ~((1<<RXEN0) | (1<<TXEN0));
  //disable ADC:
  ADCSRA &= ~(1<<ADEN);

  for (i=0; i<4; i++)
    eeprom_write_block((const void*)&measurements[i].value, (void*)&EEPROM_measurements[i].value, 4);

  //indicate writing to EEPROM has finished by lighting up the green LED
  PORTB |= (1<<PB7);//DONE

  //enable UART Tx and Rx:
  UCSR0B |= (1<<RXEN0) | (1<<TXEN0);
  // enable ADC and start a first ADC conversion
  ADCSRA |= (1<<ADEN) | (1<<ADSC);

  printString("msg BROWN-OUT\n");
}

// interrupt service routine for watchdog timeout
ISR(WDT_vect) {
  uint8_t i;

  for (i=0; i<4; i++)
    eeprom_write_block((const void*)&measurements[i].value, (void*)&EEPROM_measurements[i].value, 4);

  printString("msg WDT\n");
}

// disable WDT
void WDT_off(void) {
  cli();
  wdt_reset();
  // clear the WDT reset flag in the status register
  MCUSR &= ~(1<<WDRF);
  // timed sequence to be able to change the WDT settings afterwards
  WDTCSR |= (1<<WDCE) | (1<<WDE);
  // disable WDT
  WDTCSR = 0x00;
}

// enable WDT
void WDT_on(void) {
  // enable the watchdog timer (2s)
  wdt_enable(WDTO_2S);
  // set watchdog interrupt enable flag
  WDTCSR |= (1<<WDIE);
}

void setup()
{
  // WDT_off(); -> moved the call to this function to start of the main loop, before init

  // clock settings: divide by 8 to get a 1Mhz clock, allows us to set the BOD level to 1.8V (DS p.37)
//  CLKPR = (1<<CLKPCE);
//  CLKPR = (1<<CLKPS1) | (1<<CLKPS0);

  // load meterid's and metervalues from EEPROM
  eeprom_read_block((void*)&measurements, (const void*)&EEPROM_measurements, sizeof(measurements));

  // init serial port
  beginSerial(4800);
  _delay_ms(100);

  //LEDPIN=PB5/SCK configured as output pin
  DDRB |= (1<<PB7);//DONE

  // PD2=INT0 and PD3=INT1 configuration
  // set as input pin with 20k pull-up enabled
  PORTD |= (1<<PD2) | (1<<PD3);
  // INT0 and INT1 to trigger an interrupt on a falling edge
  EICRA = (1<<ISC01) | (1<<ISC11);
  // enable INT0 and INT1 interrupts
  EIMSK = (1<<INT0) | (1<<INT1);

#if DBG > 0
  // re-use PD4 pin for tracing interrupt times
  DDRD |= (1<<DDD4);
#else
  // PD4=PCINT20 configuration
  // set as input pin with 20k pull-up enabled
  PORTD |= (1<<PD4);
  //enable pin change interrupt on PCINT20
  PCMSK2 |= (1<<PCINT20);
  //pin change interrupt enable 2
  PCICR |= (1<<PCIE2);
#endif

  // analog comparator setup for brown-out detection
  // PD7=AIN1 configured by default as input to obtain high impedance
 
  // disable digital input cicuitry on AIN0 and AIN1 pins to reduce leakage current
  DIDR1 |= (1<<AIN1D) | (1<<AIN0D);  
 
  // comparing AIN1 (Vcc/4.4) to bandgap reference (1.1V)
  // bandgap select | AC interrupt enable | AC interrupt on rising edge (DS p.243)
  ACSR |= (1<<ACBG) | (1<<ACIE) | (1<<ACIS1) | (1<<ACIS0);

  // Timer2 set to CTC mode (DS p.146, 154, 157)
  TCCR2A |= 1<<WGM21;
#if DBG > 0
  // Toggle pin OC2A=PB3 on compare match
  TCCR2A |= 1<<COM2A0;
#endif
  // Set PB3 as output pin
  DDRB |= (1<<DDB7);//DONE
  // Timer2 clock prescaler set to 9 => fTOV2 = 1000kHz / 256 / 8 = 488.28Hz (DS p.158)
  TCCR2B |= (1<<CS22) | (1<<CS20); //DONE Prescaler 128
  // Enable output compare match interrupt for timer2 (DS p.159)
  TIMSK2 |= (1<<OCIE2A);
  // Increase sampling frequency to 1250Hz (= 625Hz per channel)
  OCR2A = 0x63;

  // disable digital input cicuitry on ADCx pins to reduce leakage current
  DIDR0 |= (1<<ADC5D) | (1<<ADC4D) | (1<<ADC3D) | (1<<ADC2D) | (1<<ADC1D) | (1<<ADC0D);

  // select VBG as reference for ADC
  ADMUX |= (1<<REFS1) | (1<<REFS0);
  // ADC prescaler set to 8 => 1000kHz / 8 = 125kHz (DS p.258)
  ADCSRA |= (1<<ADPS1) | (1<<ADPS0) | (1<<ADPS2); //DONE, since frequency is 16Mhz, Prescaler needs to be 128 to achieve 125Khz
  // enable ADC and start a first ADC conversion
  ADCSRA |= (1<<ADEN) | (1<<ADSC);

  //set global interrupt enable in SREG to 1 (DS p.12)
  sei();
}

void send(uint8_t msg_type, const struct sensor *measurement, const struct state *aux)
{
  uint8_t i;
  uint32_t value = 0;
  uint32_t ms = 0;
  int32_t rest;
  uint8_t pulse_count;

  char message[60];

  switch (msg_type) {
  case PULSE:
    // blink the green LED
    PORTB |= (1<<PB7);//DONE
    _delay_ms(20);
    PORTB &= ~(1<<PB7);//DONE

    cli();
    value = measurement->value;
    ms = aux->time;
    sei();
 
    strcpy(message, "pls ");
    break;

  case POWER:
    cli();
    rest = aux->nano_end - aux->nano_start;
    pulse_count = aux->pulse_count_final;
    sei();

    // Since the AVR has no dedicated floating-point hardware, we need 
    // to resort to fixed-point calculations for converting nWh/s to W.
    // 1W = 10^6/3.6 nWh/s
    // value[watt] = 3.6/10^6 * rest[nWh/s]
    // value[watt] = 3.6/10^6 * 65536 * (rest[nWh/s] / 65536)
    // value[watt] = 3.6/10^6 * 65536 * 262144 / 262144 * (rest[nWh/s] / 65536)
    // value[watt] = 61847.53 / 262144 * (rest[nWh/s] / 65536)
    // We round the constant down to 61847 to prevent 'underflow' in the
    // consecutive else statement.
    // The error introduced in the fixed-point rounding equals 8.6*10^-6.
    MacU16X16to32(value, (uint16_t)(labs(rest)/65536), 61847);
    value /= 262144;

    if (rest >= 0)
      value += pulse_count*3600;
    else
      value = pulse_count*3600 - value;

    strcpy(message, "pwr ");
    break;
  }

  strcpy(&message[4], measurement->id);
  strcpy(&message[36], ":0000000000\n");

  i = 46;
  do {                                // generate digits in reverse order
    message[i--] = '0' + value % 10;  // get next digit
  } while ((value /= 10) > 0);        // delete it

  if ((msg_type == PULSE) && ms) {
    strcpy(&message[47], ":0000000000\n");
    i = 57;
    do {                                // generate digits in reverse order
      message[i--] = '0' + ms % 10;     // get next digit
    } while ((ms /= 10) > 0);           // delete it
  }

  printString(message);
  printString("\r");
}

void loop()
{
  uint8_t i;
 
  // check whether we have to send out a pls or pwr to the deamon
  for (i=0; i<4; i++) {
    if (aux[i].pulse == true) {
      send(PULSE, (const struct sensor *)&measurements[i], (const struct state *)&aux[i]);
      aux[i].pulse = false;
    }

    if (aux[i].power == true) {
      send(POWER, (const struct sensor *)&measurements[i], (const struct state *)&aux[i]);
      aux[i].power = false;
    }  
  }
  wdt_reset();
}

int main(void)
{
  uint8_t i;

  WDT_off();
  setup();

  PORTB |= (1<<PB7);//DONE
  _delay_ms(1000);
  PORTB &= ~(1<<PB7);//DONE
  // insert a startup delay of 20s to prevent interference with redboot
  // interrupts are already enabled at this stage
  // so the pulses are counted but not sent to the deamon
//  for (i=0; i<4; i++) _delay_ms(5000);

  serialFlush();
  printString("\n");

  WDT_on();

  for (;;) loop();
  return 0;
}
