/*
  O2Land RHT Dakin Control, Version 3
  RHT-Daikin-Ctrl Arduino Module is needed

  Remote Trigger Command
  message to o2daikin
  command:
    daikin-auto                 enable auto control
    daikin-off                  turn OFF (but auto control is still there)
    rht-disable                 disable auto control

  Author: Samson Chen, Qblinks Inc.
*/

#include "elapsedMillis.h"

// Adjustment Logic:
//
// Starting from 26 degrees
// Check every REMAIN minutes
// heat index > HI_HIGH --> down 1 degree
// heat index < HI_LOW --> up 1 degree
// otherwise stay
//
// degree range 24 - 26
//
// boost mode, use lower temp and higher fan without further control
//

// init log information
#define INIT_STR                "RhT Control System Initialized V5.2.2, 2017-09-29"

// ambient environmental parameters during normal hours
#define HI_HIGH                 26.90
#define HI_LOW                  26.00

// ambient environmental parameters during H hours
#define HI_HIGH_H               27.50
#define HI_LOW_H                26.50

// avoid frequent ON-OFF mode switch
//   when the mode is switched, no more action is allowed before this timer is reached
#define REMAIN_MODE_TIME       10          // in minutes

// higher temperature time period (on hour), use TEMP_SET_H during this time period
#define HTEMP_BEGIN_HOUR        3           // 3:00 AM
#define HTEMP_END_HOUR          6           // 6:00 AM

// other control parameters
#define TEMP_BOOST_CMD          "att23"
#define FAN_SPEED_NIGHT         "atf6"
#define FAN_SPEED_BOOST         "atf5"

// AC setting
#define AC_START_TEMP           26
#define AC_SETTING_HIGH         26
#define AC_SETTING_LOW          24

// -----------------------------------------------------------------------------------------------------------------
// status and control parameters
#define MODE_OFF                0
#define MODE_COOLING            1
#define MODE_DEHUMIDIFIER       2
#define MODE_FAN                3

#define IR_REPEAT               3
#define IR_REPEAT_DELAY         2000

int currentMode = MODE_OFF;
elapsedMillis elapsed_system_up_timer;
elapsedMillis elapsed_remain_mode_timer;

// system up timer, value will be calculated in loop
unsigned int lastCommandSentInMinutes;
unsigned int systemUpTimerInMinutes;

String recvLine = "";
elapsedMillis timeElapsed;
elapsedMillis timeElapsedSyncTime;
elapsedMillis timeElapsedResetSht;

float currentTemp = 0;
float currentRh = 0;
float currentHI = 0;
unsigned int currentReadCount = 0;

bool daikin_boost = false;

bool rht_control_on = false;

int autoOffTimerHour = 25;  // auto off control
int autoOnTimerHour = 25;   // auto on control

bool current_H_hours = false; // track the H hours

unsigned int currentACsetting = AC_START_TEMP;

float last10minTemps[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// -----------------------------------------------------------------------------------
void setup() {
    Serial1.begin(38400);

    // Blue LED
    pinMode(D7, OUTPUT);

    // Turn RGB LED off
    rgb_led_off();

    // Subscribe Network Event
    Particle.subscribe("o2daikin", myHandler);

    // sync timer and set time zone
    Particle.syncTime();
    Time.zone(+8);

    // reset elapsed timer
    timeElapsed = 0;
    timeElapsedSyncTime = 0;
    timeElapsedResetSht = 0;

    // zero the remain_mode_timer to avoid any sudden action
    elapsed_remain_mode_timer = 0;

    // other default state settings
    daikin_boost = false;
    rht_control_on = false;
    autoOffTimerHour = 25;  // there is no clock hour 25
    autoOnTimerHour = 25;  // there is no clock hour 25
    current_H_hours = false; // assume the RhT control is enabled outside the H hours

    // set the air conditioning to the default modes
    Serial1.println(FAN_SPEED_NIGHT);

    // send the first reading request
    Serial1.println("atrt");

    // log the event
    Particle.publish("o2sensor", INIT_STR);
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


void fan_on()
{
    // control through IFTTT
    delay(10000);  // to avoid event override
    Particle.publish("o2fan", "ON");
    delay(10000);

    // log the event
    Particle.publish("o2sensor", "Turn external fan ON");
}


void fan_off()
{
    // control through IFTTT
    delay(10000);  // to avoid event override
    Particle.publish("o2fan", "OFF");
    delay(10000);

    // log the event
    Particle.publish("o2sensor", "Turn external fan OFF");
}


void daikin_fan_on()
{
    unsigned int repeat;

    // send to log
    Particle.publish("o2sensor", "switch to fan only");

    // repeatly sending commands
    for(repeat = 0; repeat < IR_REPEAT; repeat++)
    {
        // send ON ac command
        Serial1.println("ate1");

        // delay per repeated command
        delay(IR_REPEAT_DELAY);
    }

    // set mode
    currentMode = MODE_FAN;

    // reset the remain_mode_timer
    elapsed_remain_mode_timer = 0;
}


void daikin_ac_on()
{
    unsigned int repeat;

    // send to log
    Particle.publish("o2sensor", "switch to cooling");

    // repeatly sending commands
    for(repeat = 0; repeat < IR_REPEAT; repeat++)
    {
        // send ON ac command
        Serial1.println("atc1");

        // delay per repeated command
        delay(IR_REPEAT_DELAY);
    }

    // set mode
    currentMode = MODE_COOLING;

    // reset the remain_mode_timer
    elapsed_remain_mode_timer = 0;
}


void daikin_dehumidifier_on()
{
    unsigned int repeat;

    // send to log
    Particle.publish("o2sensor", "switch to dehumidifier");

    // repeatly sending commands
    for(repeat = 0; repeat < IR_REPEAT; repeat++)
    {
        // send ON dehumidifier command
        Serial1.println("atm1");

        // delay per repeated command
        delay(IR_REPEAT_DELAY);
    }

    // set mode
    currentMode = MODE_DEHUMIDIFIER;

    // reset the remain_mode_timer
    elapsed_remain_mode_timer = 0;
}


void daikin_off()
{
    unsigned int repeat;

    // send to log
    Particle.publish("o2sensor", "turn off");

    // repeatly sending commands
    for(repeat = 0; repeat < IR_REPEAT; repeat++)
    {
        // send OFF command
        Serial1.println("atd0");

        // delay per repeated command
        delay(IR_REPEAT_DELAY);
    }

    // set mode
    currentMode = MODE_OFF;

    // reset the remain_mode_timer
    elapsed_remain_mode_timer = 0;
}


void rgb_led_off()
{
  // Turn RGB LED off
  RGB.control(true);
  RGB.color(0, 0, 0);
  delay(1000);
  RGB.brightness(0);
  delay(1000);
}


void daikin_ac_on_set_temp(unsigned int setTemp)
{
  if(setTemp >= 22 && setTemp <= 26)
  {
    Particle.publish("o2sensor", "set AC temperature to " + String(setTemp));

    switch(setTemp)
    {
      case 22:
        Serial1.println("att22");
        break;

      case 23:
        Serial1.println("att23");
        break;

      case 24:
        Serial1.println("att24");
        break;

      case 25:
        Serial1.println("att25");
        break;

      case 26:
        Serial1.println("att26");
        break;
    }

    // set fan speed
    Serial1.println(FAN_SPEED_NIGHT);

    // turn on AC
    daikin_ac_on();
  }
  else
  {
    Particle.publish("o2sensor", "Invalid AC temperature setting " + String(setTemp));
  }
}


/**
 * decrease 1 degree in the range
 */
void ac_setting_down(void)
{
  if(currentACsetting > AC_SETTING_LOW)
  {
    currentACsetting--;

    // send the command
    daikin_ac_on_set_temp(currentACsetting);    
  }
}


/**
 * increase 1 degree in the range
 */
 void ac_setting_up(void)
{
  if(currentACsetting < AC_SETTING_HIGH)
  {
    currentACsetting++;

    // send the command
    daikin_ac_on_set_temp(currentACsetting);    
  }
}


/**
 * Insert current temperature into the 10-minute tracking array
 * Init the array if this is the first one
 * The latest one is always last10minTemps[0]
 */
void insert_temperature(float t)
{
  unsigned int i;

  // initialize the array if it has not happened
  if(last10minTemps[0] == 0)
  {
    for(i=0; i < 10; i++)
    {
      last10minTemps[i] = t;
    }
  }
  else
  {
    // remove the latest one, and shift every one in the memory
    for(i=0; i < 9; i++)
    {
      last10minTemps[i] = last10minTemps[i + 1];
    }

    // insert the new one
    last10minTemps[9] = t;
  }
}


// =========================================================================================================
// =========================================================================================================
// =========================================================================================================

void loop()
{
    // ---------------------------------------------------------------------------------------------------------------------------------
    // Convert stat timers
    // ---------------------------------------------------------------------------------------------------------------------------------

    // convert the time of the last command sent to minutes
    lastCommandSentInMinutes = elapsed_remain_mode_timer / (long) 60000;

    // convert the overall running time of auto control to minutes
    systemUpTimerInMinutes = elapsed_system_up_timer / (long) 60000;

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Determine remain_mode timer based on the current mode
    // ---------------------------------------------------------------------------------------------------------------------------------
    unsigned int useRemainModeTime = REMAIN_MODE_TIME;

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Determine current time
    // ---------------------------------------------------------------------------------------------------------------------------------

    bool hTempTime = (Time.hour() >= HTEMP_BEGIN_HOUR && Time.hour() < HTEMP_END_HOUR) ? true : false;

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Track the H hours and switch the temperature setting if enters to or exits from the H hours
    // ---------------------------------------------------------------------------------------------------------------------------------

    if(rht_control_on)
    {
      if((!current_H_hours) && hTempTime)
      {
        // enters to H hours
        Particle.publish("o2sensor", "H hours begins");
      }
      else if(current_H_hours && (!hTempTime))
      {
        // exits from H hours
        Particle.publish("o2sensor", "H hours ends");
      }
    }

    // track it
    current_H_hours = hTempTime;

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Check auto on timer (Check ON first, then OFF, in case OFF/ON at the same time)
    // ---------------------------------------------------------------------------------------------------------------------------------

    if(Time.hour() == autoOnTimerHour)
    {
      // process the enable procedure regardless what the current control status is
      // **************************************************************************
      // reset system control variables
      elapsed_remain_mode_timer = 60 * (long) 60000;  // maximize the remain_mode_timer to allow the next contorl
      elapsed_system_up_timer = 0;  // this is a fresh start, reset must-off timer to enable the auto control
      currentMode = MODE_OFF;  // assume the current mode off
      daikin_boost = false;  // don't boost for the next run

      // enable RHT control
      rht_control_on = true;

      // external FAN off if AC is turned on by timer
      fan_off();

      // restore the defaults
      currentACsetting = AC_START_TEMP;
      daikin_ac_on_set_temp(currentACsetting);

      // send the log
      Particle.publish("o2sensor", "RhT On by the Auto On Timer");

      // clear timer
      autoOnTimerHour = 25;

      // Turn RGB LED off
      rgb_led_off();
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Check auto off timer
    // ---------------------------------------------------------------------------------------------------------------------------------

    if(Time.hour() == autoOffTimerHour)
    {
      // disable RHT control
      rht_control_on = false;

      // don't boost for the next run
      daikin_boost = false;

      // turn AC off
      daikin_off();

      // external FAN on if AC is turned off by timer
      fan_on();

      // send the log
      Particle.publish("o2sensor", "RhT Off by the Auto Off Timer");

      // clear timer
      autoOffTimerHour = 25;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Processing periodical commands
    // ---------------------------------------------------------------------------------------------------------------------------------

    long readingInterval = (currentTemp > 0 && currentRh > 0) ? ((long) 60000) : ((long) 8000); // shorter if ambient info was not obtained
    if (timeElapsed > readingInterval)
    {
        Serial1.println("atrt");        // send command of getting environmental information
        timeElapsed = 0;                // reset the counter to 0 so the counting starts over...

        // debugging area
        //Particle.publish("o2sensor", String(Time.hour()) + ":" + String(Time.minute()));
    }

    // daily time sync
    if(timeElapsedSyncTime > 43200000)
    {
        Particle.syncTime();
        timeElapsedSyncTime = 0;
    }

    // reset SHT31-D every hour plus 7 seconds
    // it seems SHT31-D some time freeze after constantly running for a few days
    if(timeElapsedResetSht > (long) 3607000)
    {
        Serial1.println("atrs");        // send command to reset SHT31-D
        timeElapsedResetSht = 0;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Processing incoming UART commands
    // ---------------------------------------------------------------------------------------------------------------------------------

    // check if Qmote has incoming notifications or commands
    while (Serial1.available() > 0)
    {
        char c = Serial1.read();

        if(c == 0x0D)
        {
            // envrionmental data response
            if(recvLine.substring(0, 3)  == ">R=")
            {
                // proceed extraction

                int searchIdx;
                int commaIdx;
                float xRh = 0;
                float xTemp = 0;
                float xHI = 0;
                unsigned int xReadCount = 0;
                bool xtracted = false;

                // extract R
                searchIdx = recvLine.indexOf("R=");
                if(searchIdx >= 0)
                {
                    commaIdx = recvLine.substring(searchIdx).indexOf(",");
                    if(commaIdx >= 0)
                    {
                        xRh=recvLine.substring(searchIdx + 2, commaIdx + searchIdx).toFloat();

                        // extract T
                        searchIdx = recvLine.indexOf("T=");
                        if(searchIdx >= 0)
                        {
                            commaIdx = recvLine.substring(searchIdx).indexOf(",");
                            if(commaIdx >= 0)
                            {
                                xTemp=recvLine.substring(searchIdx + 2, commaIdx + searchIdx).toFloat();

                                // extract H
                                searchIdx = recvLine.indexOf("H=");
                                if(searchIdx >= 0)
                                {
                                    commaIdx = recvLine.substring(searchIdx).indexOf(",");
                                    if(commaIdx >= 0)
                                    {
                                        xHI = recvLine.substring(searchIdx + 2, commaIdx + searchIdx).toFloat();

                                        // extract C
                                        searchIdx = recvLine.indexOf("C=");
                                        if(searchIdx >= 0)
                                        {
                                            xReadCount = recvLine.substring(searchIdx + 2).toInt();

                                            // information successfully extracted
                                            xtracted = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // apply the information if extracted
                if(xtracted)
                {
                    currentRh = xRh;
                    currentTemp = xTemp;
                    currentHI = xHI;
                    currentReadCount = xReadCount & 0xFFFF;     // keep only the low 16-bit
                    String modeStr;

                    // output current mode
                    switch(currentMode)
                    {
                        case MODE_OFF:
                            modeStr = "OFF";
                            break;

                        case MODE_COOLING:
                            modeStr = "AC" + String(currentACsetting);
                            break;

                        case MODE_DEHUMIDIFIER:
                            modeStr = "DH";
                            break;

                        case MODE_FAN:
                            modeStr = "FAN";
                            break;

                        default:
                            modeStr = "Unknown";
                            break;
                    }

                    // publish to thingspeak
                    // Particle.publish("thingspeak", "field1=" + String(xRh, 2) + "&field2=" + String(xTemp, 2) + "&field3=" + String(xHI,2));

                    // publish to the event log
                    String txtOutput = "Rh" + String(xRh, 2) + ", T" + String(xTemp, 2) + ", HI" + String(xHI,2);

                    // added the conditional stat events
                    if(!rht_control_on)
                    {
                        txtOutput += ", NO_Ctrl";
                    }
                    else
                    {
                      // current mode
                      txtOutput += ", " + modeStr;

                      // current control status
                      if(daikin_boost)
                      {
                          txtOutput += ", Boost";
                      }
                      else if(lastCommandSentInMinutes < useRemainModeTime)
                      {
                          txtOutput += ", Remain";
                      }
                      else if(hTempTime == true)
                      {
                          txtOutput += ", HTemp";
                      }
                    }

                    // publish it
                    Particle.publish("o2sensor", txtOutput);

                    // tracking temperature history
                    if(currentTemp > 0)
                    {
                      insert_temperature(currentTemp);
                    }
                } // end if(xtracted)
            } // end of extraction

          // clear the line
          recvLine.remove(0);
          recvLine = "";
        }
        else if(c != 0x0A)
        {
          recvLine += c;
        }
        // ignore 0x0A
    } // end while

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Environmental Control
    // ---------------------------------------------------------------------------------------------------------------------------------

    // determine temperature setting to use
    float hiHigh = hTempTime ? HI_HIGH_H : HI_HIGH;
    float hiLow = hTempTime ? HI_LOW_H : HI_LOW;
    
    // performing daikin control only when RHT Control is enabled and not in boost mode and all ambient parameters were obtained
    if(rht_control_on && !daikin_boost && currentHI > 0 && currentTemp > 0)
    {
      // avoid frequent mode switching
      if(lastCommandSentInMinutes >= useRemainModeTime)
      {

        // .....................................................
        // if currentHI > hiHigh --> down 1 degree
        if(currentHI > hiHigh)
        {
          // do something only when the temperature stops decreasing
          if(currentTemp >= last10minTemps[0])
          {
            // down 1 degree
            ac_setting_down();          
          }          
        }
        else if(currentHI < hiLow)
        {
          // do somwthing only when the temperature stops increasing
          if(currentTemp <= last10minTemps[0])
          {
            // up 1 degree
            ac_setting_up();
          }
        }

        // otherwise remain the same setting

        // .....................................................

      } // end REMAIN_MODE_TIME

      //*******************************************************

    } // end rht_control_on

  // ------------------------------------------------------------------------------------------

} // end main loop


// Event handler triggered from IFTTT
void myHandler(const char *event, const char *data)
{
    String ingredient = data;

    if(ingredient.indexOf("daikin-auto") > -1)
    {
        // maximize the remain_mode_timer to allow the next contorl
        elapsed_remain_mode_timer = 60 * (long) 60000;

        // this is a fresh start
        // reset must-off timer to enable the auto control
        elapsed_system_up_timer = 0;

        // assume the current mode off
        currentMode = MODE_OFF;

        // switch between normal mode and boost mode
        if(rht_control_on)
        {
          if(daikin_boost)
          {
            // log the event
            Particle.publish("o2sensor", "Rht switched to normal mode");

            daikin_boost = false;

            // restore the defaults
            currentACsetting = AC_START_TEMP;
            daikin_ac_on_set_temp(currentACsetting);

            // turn off external fan
            fan_off();
          }
          else
          {
            // log the event
            Particle.publish("o2sensor", "Rht switched to boost mode");

            daikin_boost = true;

            // use the boost settings
            Serial1.println(TEMP_BOOST_CMD);
            Serial1.println(FAN_SPEED_BOOST);

            // turn on everything
            daikin_ac_on();
            fan_on();
          }
        }
        else
        {
          // log the event
          Particle.publish("o2sensor", "RhT-Auto Enabled");

          // restore the defaults
          currentACsetting = AC_START_TEMP;
          daikin_ac_on_set_temp(currentACsetting);

          // enable RHT Control
          rht_control_on = true;
        }
    }
    else if(ingredient.indexOf("daikin-off") > -1)
    {
      // disable RHT control
      rht_control_on = false;

      // don't boost for the next run
      daikin_boost = false;

      // turn both AC and FAN off
      daikin_off();
      fan_off();

      // rht off also disable auto-on
      autoOnTimerHour = 25;

      // Turn RGB LED off
      rgb_led_off();
    }
    else if(ingredient.indexOf("auto-off-") > -1)
    {
      // auto off timer is a one time valid setting
      // it can be set independently from rht on/off control
      // auto-off-1 means off at 01:00
      // auto-off-23 means off at 23:00
      // auto-off-25 means no auto off (because there is no 25:00)

      if(ingredient.length() >= 10)
      {
        int searchIdx = ingredient.indexOf("auto-off-");
        autoOffTimerHour = ingredient.substring(searchIdx + 9).toInt();

        // log the config
        if(autoOffTimerHour >= 0 && autoOffTimerHour <= 24)
        {
          Particle.publish("o2sensor", "RhT Auto Off at " + String(autoOffTimerHour) + ":00");
        }
        else
        {
          //set to 25 if it is not 00 through 24
          autoOffTimerHour = 25;

          Particle.publish("o2sensor", "RhT Auto Off Timer is disabled");
        }
      }
    }
    else if(ingredient.indexOf("auto-on-") > -1)
    {
      // auto on timer is a one time valid setting
      // it can be set independently from rht on/off control
      // auto-on-1 means on at 01:00
      // auto-on-23 means on at 23:00
      // auto-on-25 means no auto on (because there is no 25:00)

      if(ingredient.length() >= 9)
      {
        int searchIdx = ingredient.indexOf("auto-on-");
        autoOnTimerHour = ingredient.substring(searchIdx + 8).toInt();

        // log the config
        if(autoOnTimerHour >= 0 && autoOnTimerHour <= 24)
        {
          Particle.publish("o2sensor", "RhT Auto On at " + String(autoOnTimerHour) + ":00");

          // restore the defaults
          currentACsetting = AC_START_TEMP;

          // Auto-ON mode is important, so lid the RGB LED
          RGB.control(true);
          RGB.color(0, 100, 0);
          delay(1000);
          RGB.brightness(20);
          delay(1000);
        }
        else
        {
          // set to 25 if it is not 00 through 24
          autoOnTimerHour = 25;

          Particle.publish("o2sensor", "RhT Auto On Timer is disabled");

          // Turn RGB LED off
          rgb_led_off();
        }
      }
    }
    else if(ingredient.indexOf("cool-on-") > -1)
    {
      // Turn on AC Cool supports only
      // cool-on-22
      // cool-on-23
      // cool-on-24
      // cool-on-25
      // cool-on-26

      if(ingredient.length() == 10)
      {
        int searchIdx = ingredient.indexOf("cool-on-");
        int coolTemp = ingredient.substring(searchIdx + 8).toInt();

        // log the config
        if(coolTemp >= 22 && coolTemp <= 26)
        {
          Particle.publish("o2sensor", "Manually turn on AC and set temperature to " + String(coolTemp));

          switch(coolTemp)
          {
            case 22:
              Serial1.println("att22");
              break;

            case 23:
              Serial1.println("att23");
              break;

            case 24:
              Serial1.println("att24");
              break;

            case 25:
              Serial1.println("att25");
              break;

            case 26:
              Serial1.println("att26");
              break;
          }

          // set fan speed
          Serial1.println(FAN_SPEED_NIGHT);

          // turn on AC
          daikin_ac_on();
        }
        else
        {
          Particle.publish("o2sensor", "Invalid AC temperature command " + String(coolTemp));
        }
      }
    }

    // toggle LED as a progress indicator
    digitalWrite(D7, HIGH);
    delay(1000);
    digitalWrite(D7, LOW);
}
