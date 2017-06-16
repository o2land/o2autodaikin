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
// if (COLD_HEAT_INDEX <= temp_heat_index) && (temp <= DEH_TEMP), use DEH, otherwise, use AC
// * The trick is if the temperature is low, AC equals to FAN,
//   but if the temperature rises too fast, AC will run as AC.
//   On the other hand, if heat index is high but temperature is low,
//   it implies the humidity is too high. AC will not work but dehumidifier will.
//
// boost mode, use lower temp and higher fan without further control
//

// init log information
#define INIT_STR                "RhT Control System Initialized V3.3.0, 2017-06-16"

// ambient environmental parameters
#define DEH_TEMP                26.00       // This is (TEMP_AC_CMD + 2) because AC may stop running below that point
#define COLD_TEMP_HI            24.20       // use Heat Index
#define COLD_TEMP_HI_H          24.75       // use Heat Index (Higher Tempeature Period)

#define TEMP_AC_CMD             "att24"
#define TEMP_BOOST_CMD          "att23"
#define FAN_SPEED_NIGHT         "atf6"
#define FAN_SPEED_BOOST         "atf5"

// avoid frequent ON-OFF mode switch
//   when the mode is switched, no more action is allowed before this timer is reached
#define REMAIN_MODE_TIME         5          // in minutes
#define REMAIN_MODE_TIME_DH     15          // in minutes

// higher temperature time period (on hour), use COLD_TEMP_H during this time period
#define HTEMP_BEGIN_HOUR        3           // 3:00 AM
#define HTEMP_END_HOUR          6           // 6:00 AM

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

bool current_fan_mode_on = false;
bool daikin_boost = false;

bool rht_control_on = false;

int autoOffTimerHour = 25;  // auto off control
int autoOnTimerHour = 25;   // auto on control

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
    current_fan_mode_on = false;
    daikin_boost = false;
    rht_control_on = false;
    autoOffTimerHour = 25;  // there is no clock hour 25
    autoOnTimerHour = 25;  // there is no clock hour 25

    // set the air conditioning to the default modes
    Serial1.println(TEMP_AC_CMD);
    Serial1.println(FAN_SPEED_NIGHT);

    // log the event
    Particle.publish("o2sensor", INIT_STR);
}


void fan_on()
{
    if(!current_fan_mode_on)
    {
        // control through IFTTT
        Particle.publish("o2fan", "ON");

        // set the flag
        current_fan_mode_on = TRUE;

        // log the event
        Particle.publish("o2sensor", "Turn external fan ON");
    }
}


void fan_off()
{
    if(current_fan_mode_on)
    {
        // control through IFTTT
        Particle.publish("o2fan", "OFF");

        // set the flag
        current_fan_mode_on = FALSE;

        // log the event
        Particle.publish("o2sensor", "Turn external fan OFF");
    }
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

    // Dehumidifer uses a shorter timer since it may pull temperature too low
    if(currentMode == MODE_DEHUMIDIFIER)
    {
      useRemainModeTime = REMAIN_MODE_TIME_DH;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Determine current time
    // ---------------------------------------------------------------------------------------------------------------------------------

    bool hTempTime = (Time.hour() >= HTEMP_BEGIN_HOUR && Time.hour() < HTEMP_END_HOUR) ? true : false;

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Check auto on timer (Check ON first, then OFF, in case OFF/ON at the same time)
    // ---------------------------------------------------------------------------------------------------------------------------------

    if(Time.hour() == autoOnTimerHour)
    {
      // process the enable procedure regardless what the current control status is
      // **************************************************************************
      // reset system control variables
      elapsed_remain_mode_timer = REMAIN_MODE_TIME * (long) 60000;  // maximize the remain_mode_timer to allow the next contorl
      elapsed_system_up_timer = 0;  // this is a fresh start, reset must-off timer to enable the auto control
      currentMode = MODE_OFF;  // assume the current mode off
      daikin_boost = false;  // don't boost for the next run

      // enable RHT control
      rht_control_on = true;

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
      if(rht_control_on)
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
      }

      // clear timer
      autoOffTimerHour = 25;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Processing periodical commands
    // ---------------------------------------------------------------------------------------------------------------------------------

    if (timeElapsed > (long) 60000)
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
                            modeStr = "AC";
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
                    Particle.publish("thingspeak", "field1=" + String(xRh, 2) + "&field2=" + String(xTemp, 2) + "&field3=" + String(xHI,2));

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

    // determine cold temperature criteria based on the current time
    float coldTemp = hTempTime ? COLD_TEMP_HI_H : COLD_TEMP_HI;

    // performing daikin control only when RHT Control is enabled and not in boost mode
    if(rht_control_on && !daikin_boost)
    {
      // avoid frequent mode switching
      if(lastCommandSentInMinutes >= useRemainModeTime)
      {
        // .....................................................

        // if COLD_TEMP <= temp <= DEH_TEMP
        if(((float) currentHI >= (float) coldTemp) &&
           ((float) currentTemp <= (float) DEH_TEMP))
        {
          if(currentMode != MODE_DEHUMIDIFIER)
          {
            daikin_dehumidifier_on();
          }
        }

        // .....................................................

        // outside the DEH window
        else
        {
          if(currentMode != MODE_COOLING)
          {
            daikin_ac_on();
          }
        }

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
        elapsed_remain_mode_timer = REMAIN_MODE_TIME * (long) 60000;

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
            Serial1.println(TEMP_AC_CMD);
            Serial1.println(FAN_SPEED_NIGHT);

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
          Serial1.println(TEMP_AC_CMD);
          Serial1.println(FAN_SPEED_NIGHT);

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
      // auto-off-01 means off at 01:00
      // auto-off-23 means off at 23:00
      // auto-off-25 means no auto off (because there is no 25:00)

      if(ingredient.length() == 11)
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
          Particle.publish("o2sensor", "RhT Auto Off Timer is disabled");
        }
      }
    }
    else if(ingredient.indexOf("auto-on-") > -1)
    {
      // auto on timer is a one time valid setting
      // it can be set independently from rht on/off control
      // auto-on-01 means on at 01:00
      // auto-on-23 means on at 23:00
      // auto-on-25 means no auto on (because there is no 25:00)

      if(ingredient.length() == 10)
      {
        int searchIdx = ingredient.indexOf("auto-on-");
        autoOnTimerHour = ingredient.substring(searchIdx + 8).toInt();

        // log the config
        if(autoOnTimerHour >= 0 && autoOnTimerHour <= 24)
        {
          Particle.publish("o2sensor", "RhT Auto On at " + String(autoOnTimerHour) + ":00");

          // Auto-ON mode is important, so lid the RGB LED
          RGB.control(true);
          RGB.color(0, 100, 0);
          delay(1000);
          RGB.brightness(20);
          delay(1000);
        }
        else
        {
          Particle.publish("o2sensor", "RhT Auto On Timer is disabled");

          // Turn RGB LED off
          rgb_led_off();
        }
      }
    }

    // toggle LED as a progress indicator
    digitalWrite(D7, HIGH);
    delay(1000);
    digitalWrite(D7, LOW);
}
