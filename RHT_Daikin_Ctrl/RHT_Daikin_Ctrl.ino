/*
  Humidity and Temperature Reader with Daikin Air Conditioning Control

  For Sensirion SHT3x Arduino library, please refer to https://github.com/winkj/arduino-sht
  For the information of AltSoftSerial, please refer to https://www.pjrc.com/teensy/td_libs_AltSoftSerial.html
  For the information of Daikin IR Control, please refer to https://github.com/danny-source/Arduino_IRremote_Daikin

  Commands          response                              description
  at                >OK
  atrt              >R=rr,T=tt.tt,H=hh.hh,C=nn            Reading temperature/humidity, R=Rh;T=Temperature(C);H=Heat Index(C);C=Data Count(Number of data fetching from SHT3X)
  atc1              >OK                                   Cooling ON
  atm1              >OK                                   Dehumidifier ON
  atfN              >OK or >Err                           Set fan speed, N=1 ~ 6, default = 6
  attN              >OK or >Err                           Set air conditioning temperature, N=18 ~ 29, default = 24
  atd0              >OK                                   Air conditioning all OFF
  
  Author: Samson Chen, Qblinks Inc.
  Date: Apr 10th, 2016
*/

#include <AltSoftSerial.h>
#include <Wire.h>
#include <elapsedMillis.h>
#include <IRdaikin.h>
#include "sht3x.h"

// Using Sensirion SHT3x for the accurate measurements
SHT3X sht3x;

// for AltSoftSerial, TX/RX pins are hard coded: RX = digital pin 8, TX = digital pin 9 on Arduino Uno
AltSoftSerial portOne;
String recvLine = "";

// Daikin Default
#define TEMP_SETTING          24          // default cooling temperature
#define FAN_SETTING           6           // default fan speed (NIGHT)

// Daikin Control
IRdaikin irdaikin;
int daikin_temp = TEMP_SETTING;
int daikin_fan = FAN_SETTING;

// SHT3X reading tracking
float currentTemp = 0;
float currentRh = 0;
unsigned int sht_read_count = 0;  // tracking the number of data fetching from SHT3X

// tracking the sample interval
elapsedMillis timeElapsed;
long lastSampleTime;
elapsedMillis shtReading;


/**
 *  Setup
 */
void setup() {
  // Setup SHT3x
  sht3x.setAddress(SHT3X::I2C_ADDRESS_44);
  sht3x.setAccuracy(SHT3X::ACCURACY_MEDIUM);
  Wire.begin();

  // Start the software serial port
  portOne.begin(38400);

  // let serial console settle
  delay(3000);

  // initialize IR module
  irdaikin.begin();

  // default Daikin setting
  daikin_temp = TEMP_SETTING;
  daikin_fan = FAN_SETTING;

  // reset reading
  currentTemp = 0;
  currentRh = 0;
  sht_read_count = 0;

  // reset sample tracking
  timeElapsed = (long) 10000;   // this initial value to force the first SHT3X reading
  lastSampleTime = 0;
  shtReading = 0;
  
  // notify the processor
  String qmoteCmd = "RhT-D READY\r\n";
  portOne.write(qmoteCmd.c_str());
}


/**
 *  Switch to cooling mode
 */
void daikin_cooling_on()
{
  irdaikin.daikin_on();
  irdaikin.daikin_setSwing_off();
  irdaikin.daikin_setMode(1);  // cooling
  irdaikin.daikin_setFan(daikin_fan);
  irdaikin.daikin_setTemp(daikin_temp);
  irdaikin.daikin_sendCommand();
}


/**
 *  Switch to dehumidifier mode
 */
void daikin_dehumidifier_on()
{
  irdaikin.daikin_on();
  irdaikin.daikin_setSwing_off();
  irdaikin.daikin_setMode(2);  // dehumidifier
  irdaikin.daikin_setFan(daikin_fan);
  irdaikin.daikin_setTemp(daikin_temp);  
  irdaikin.daikin_sendCommand();
}

/**
 *  Air conditioning OFF
 */
void daikin_all_off()
{
  irdaikin.daikin_off();
  irdaikin.daikin_setSwing_off();
  irdaikin.daikin_setMode(2);  // dehumidifier
  irdaikin.daikin_setFan(daikin_fan);
  irdaikin.daikin_setTemp(daikin_temp);  
  irdaikin.daikin_sendCommand();
}


/**
 * Heat Index Calculator
 * Code based on Robtillaart's post on http://forum.arduino.cc/index.php?topic=107569.0
 */
float heatIndex(double tempC, double humidity)
{
 double c1 = -42.38, c2 = 2.049, c3 = 10.14, c4 = -0.2248, c5= -6.838e-3, c6=-5.482e-2, c7=1.228e-3, c8=8.528e-4, c9=-1.99e-6;
 double T = (tempC * ((double) 9 / (double) 5)) + (double) 32;
 double R = humidity;

 double A = (( c5 * T) + c2) * T + c1;
 double B = ((c7 * T) + c4) * T + c3;
 double C = ((c9 * T) + c8) * T + c6;

 double rv = (C * R + B) * R + A;
 double rvC = (rv - (double) 32) * ((double) 5 / (double) 9);
 
 return ((float) rvC);
}


/**
 *  Main loop
 */
void loop()
{
  float readingTemp;
  float readingRh;
  
  // ---------------------------------------------------------------------------------------------------------------------------------
  // Processing SHT3x sampling and reading
  // ---------------------------------------------------------------------------------------------------------------------------------
  
  // issue SHT3x sampling every 8 seconds
  if((timeElapsed - lastSampleTime) > (long) 8000)
  {
    sht3x.readSample();
    lastSampleTime = timeElapsed;
  }

  // read SHT3x every 2 seonds
  if(shtReading >= (long) 2000)
  {
    shtReading = 0;
  
    // read SHT3x
    readingTemp = sht3x.getTemperature();
    readingRh = sht3x.getHumidity();
  
    // filter out incorrect SHT3x reading
    if(readingTemp > -100 && readingRh >= 0)
    {
      currentTemp = readingTemp;
      currentRh = readingRh;
      sht_read_count++;
    }     
  }

  // ---------------------------------------------------------------------------------------------------------------------------------
  // Processing UART ewceiver
  // ---------------------------------------------------------------------------------------------------------------------------------

  // check if UART has incoming data
  while (portOne.available() > 0)
  {
    char c = portOne.read();
    
    if(c == 0x0D)
    {
      // processing the line
      portOne.println(recvLine);

      if(recvLine == "at")
      {
        portOne.println(">OK");
      }
      else if(recvLine == "atrt")
      {
        String rsp;

        rsp = ">R=";
        rsp += currentRh;
        rsp += ",T=";
        rsp += currentTemp;
        rsp += ",H=";
        rsp += heatIndex(currentTemp, currentRh);
        rsp += ",C=";
        rsp += sht_read_count;

        portOne.println(rsp);
      }
      else if(recvLine == "atc1")
      {
        daikin_cooling_on();
        portOne.println(">OK");
      }
      else if(recvLine == "atm1")
      {
        daikin_dehumidifier_on();
        portOne.println(">OK");
      }
      else if(recvLine == "atd0")
      {
        daikin_all_off();
        portOne.println(">OK");
      }
      else if(recvLine == "atf1")
      {
        daikin_fan = 1;
        portOne.println(">OK set fan speed to 1 (max)");
      }
      else if(recvLine == "atf2")
      {
        daikin_fan = 2;
        portOne.println(">OK set fan speed to 2");
      }
      else if(recvLine == "atf3")
      {
        daikin_fan = 3;
        portOne.println(">OK set fan speed to 3");
      }
      else if(recvLine == "atf4")
      {
        daikin_fan = 4;
        portOne.println(">OK set fan speed to 4");
      }
      else if(recvLine == "atf5")
      {
        daikin_fan = 5;
        portOne.println(">OK set fan speed to 5");
      }
      else if(recvLine == "atf6")
      {
        daikin_fan = 6;
        portOne.println(">OK set fan speed to 6 (night)");
      }
      else if(recvLine == "att18")
      {
        daikin_temp = 18;
        portOne.println(">OK set cooling temperature to 18 (min)");
      }
      else if(recvLine == "att19")
      {
        daikin_temp = 19;
        portOne.println(">OK set cooling temperature to 19");
      }
      else if(recvLine == "att20")
      {
        daikin_temp = 20;
        portOne.println(">OK set cooling temperature to 20");
      }
      else if(recvLine == "att21")
      {
        daikin_temp = 21;
        portOne.println(">OK set cooling temperature to 21");
      }
      else if(recvLine == "att22")
      {
        daikin_temp = 22;
        portOne.println(">OK set cooling temperature to 22");
      }
      else if(recvLine == "att23")
      {
        daikin_temp = 23;
        portOne.println(">OK set cooling temperature to 23");
      }
      else if(recvLine == "att24")
      {
        daikin_temp = 24;
        portOne.println(">OK set cooling temperature to 24");
      }
      else if(recvLine == "att25")
      {
        daikin_temp = 25;
        portOne.println(">OK set cooling temperature to 25");
      }
      else if(recvLine == "att26")
      {
        daikin_temp = 26;
        portOne.println(">OK set cooling temperature to 26");
      }
      else if(recvLine == "att27")
      {
        daikin_temp = 27;
        portOne.println(">OK set cooling temperature to 27");
      }
      else if(recvLine == "att28")
      {
        daikin_temp = 28;
        portOne.println(">OK set cooling temperature to 28");
      }
      else if(recvLine == "att29")
      {
        daikin_temp = 29;
        portOne.println(">OK set cooling temperature to 29 (max)");
      }
      else
      {
        portOne.println(">Err");
      }
      
      // clear the line
      recvLine = "";
    }
    else if(c != 0x0A)
    {
      recvLine += c;
    }
    // ignore 0x0A
  }
}

