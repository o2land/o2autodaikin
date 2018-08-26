/*
  O2Land RHT Dakin Control, Version 6
  RHT-Daikin-Ctrl Arduino Module is needed

  Remote Trigger Command
  message to o2daikin
  command:
    daikin-auto                 enable auto control
    daikin-off                  disable auto contro
    auto-dryer                  enable auto control starting from dehumidifier

    auto-off-[hour]             turn off at [hour] o'clock  (25 to disable)
    auto-pause                  turn off at preset time until ambient temperature hits the limit
    cool-on-[degree]            set ac temperature to [degree] (22 ~ 26) (auto-control will be disabled)
    drying-on                   switch to dehumidifier (auto-control will be disabled)

    more-fan                    increase FAN speed
    less-fan                    decrease FAN speed
    max-fan                     maximize FAN speed
    quiet-fan                   quiet FAN speed
    too-hot                     decrease temperature by 1 degree
    too-cold                    increase temperature by 1 degree

  Author: Samson Chen
*/

#include "elapsedMillis.h"

// Adjustment Logic:
//
// Starting from 26 degrees
// Check every REMAIN minutes
// heat index > HI_HIGH --> down 1 degree
// heat index < HI_LOW --> up 1 degree
// otherwise stay in the degree range 24 - 26
//
// Auto Cooling is used internally
// Either Auto Dryer or Auto Pause may switch to Auto Cooling
//
// Auto Cooling criterias to switch mode to Cooling:
//  the temperature is higher than TEMP_TOO_HIGH
//  or lower than TEMP_TOO_LOW

// init log information
#define INIT_STR                "RhT Control System Initialized V6.9.6, 2018-08-27"

// ambient environmental control parameters
#define HI_HIGH                 26.60      // higher than this, AC gets 1 degree less 
#define HI_LOW                  25.90      // lower than this, AC gets 1 degree more

// ambient environmental parameters during H hours
#define HI_TOO_HIGH             28.50      // for remaining minutes determination
#define HI_TOO_LOW              23.00      // for remaining minutes determination
#define TEMP_TOO_HIGH           27.75      // for Auto Cooling cancellation
#define TEMP_TOO_LOW            23.75      // for Auto Cooling cancellation

// auto pause time and temperature limit
#define AUTO_PAUSE_HOUR         3          // auto pause time
#define AUTO_PAUSE_MINUTE       30         // at 3:30 AM
#define AUTO_PAUSE_GUARD_HOUR   3          // do not rerun AC
#define AUTO_PAUSE_GUARD_MINUTE 40         // until 3:40 AM

// avoid frequent ON-OFF mode switch
//   when the mode is switched, no more action is allowed before this timer is reached
#define REMAIN_MODE_TIME       10          // in minutes
#define REMAIN_MODE_TIME_EXT    2          // used when temperature goes extreme

// other control parameters
#define FAN_SPEED_NIGHT         "atf6"

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

bool rht_control_on = false;

int autoOffTimerHour = 25;    // auto off control
bool auto_pause = false;      // auto pause control
bool auto_cooling = false;    // once this is on, the mode can switch to cooling when criterias are met

unsigned int currentACsetting = AC_START_TEMP;
unsigned int currentFANsetting = 6;

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
    rht_control_on = false;
    autoOffTimerHour = 25;  // there is no clock hour 25
    auto_pause = false;

    // default environmental senttings
    currentACsetting = AC_START_TEMP;
    currentFANsetting = 6;

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

    // send AC on command to enable the new setting
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
 * set FAN speed
 */
void ac_fan_setting(void)
{
  switch(currentFANsetting)
  {
    case 1:
      Serial1.println("atf1");
      break;

    case 2:
      Serial1.println("atf2");
      break;

    case 3:
      Serial1.println("atf3");
      break;
      
    case 4:
      Serial1.println("atf4");
      break;
      
    case 5:
      Serial1.println("atf5");
      break;
      
    case 6:
    default:
      Serial1.println("atf6");
      break;
  }

  // send AC on command to enable the new setting
  daikin_ac_on();

  // log the change
  Particle.publish("o2sensor", "Changed FAN speed to " + String(currentFANsetting));
}


/**
 * increase FAN speed
 */
void fan_speed_up(void)
{
  if(currentFANsetting <= 1)
  {
    currentFANsetting = 1;    
  }
  else
  {
    currentFANsetting--;
  }

  ac_fan_setting();
}


/**
 * decrease FAN speed
 */
void fan_speed_down(void)
{
  if(currentFANsetting >= 6)
  {
    currentFANsetting = 6;    
  }
  else
  {
    currentFANsetting++;
  }

  ac_fan_setting();
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
    // Auto Pause control
    // ---------------------------------------------------------------------------------------------------------------------------------

    if(auto_pause && currentMode != MODE_OFF &&
       Time.hour() == AUTO_PAUSE_HOUR && Time.minute() == AUTO_PAUSE_MINUTE)
    {
      Particle.publish("o2sensor", "Auto Pause turned off AC");

      // turn AC off
      daikin_off();

      // no RHT control at this moment until Auto Cooling is cancelled
      rht_control_on = false;

      // temporarily disable Auto Cooling (usually enabled by Auto Dryer)
      auto_cooling = false;
    }
    else if(auto_pause && currentMode == MODE_OFF &&
            Time.hour() == AUTO_PAUSE_GUARD_HOUR && Time.minute() == AUTO_PAUSE_GUARD_MINUTE)
    {
      Particle.publish("o2sensor", "Auto Pause switched to Auto Cooling");

      // turn off Auto Pause
      auto_pause = false;

      // turn off RGB LED
      rgb_led_off();

      // enable Auto Cooling
      auto_cooling = true;
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Determine if the mode needs to be switched to cooling based on the current temperature
    // ---------------------------------------------------------------------------------------------------------------------------------
    if(auto_cooling)
    {
        if(currentTemp > 0 && (currentTemp > TEMP_TOO_HIGH || currentTemp < TEMP_TOO_LOW))
        {
          // criterias are met, ready to switch to cooling mode
          auto_cooling = false;

          // restore RHT auto-control
          rht_control_on = true;

          // requires at least one AC command to come back
          currentACsetting = AC_START_TEMP;
          daikin_ac_on_set_temp(currentACsetting);

          // exits from H hours
          Particle.publish("o2sensor", "current temperature goes out of range, exit Auto Cooling mode, switch current status to AC mode");
        }
    }

    // ---------------------------------------------------------------------------------------------------------------------------------
    // Determine temperature setting to use
    // ---------------------------------------------------------------------------------------------------------------------------------
    float hiHigh = HI_HIGH;
    float hiLow = HI_LOW;
    
    // ---------------------------------------------------------------------------------------------------------------------------------
    // Determine remain_mode timer based on the current mode
    // ---------------------------------------------------------------------------------------------------------------------------------
    unsigned int useRemainModeTime = REMAIN_MODE_TIME;
    
    // hot ambient environment takes different remain timer
    if(currentHI > 0 && (currentHI > HI_TOO_HIGH || currentHI < HI_TOO_LOW))
    {
      useRemainModeTime = REMAIN_MODE_TIME_EXT;
    }
            
    // ---------------------------------------------------------------------------------------------------------------------------------
    // Check auto off timer
    // ---------------------------------------------------------------------------------------------------------------------------------

    if(Time.hour() == autoOffTimerHour)
    {
      // disable RHT control
      rht_control_on = false;

      // also turn off the Auto Cooling mode and Auto Pause mode
      auto_cooling = false;
      auto_pause = false;

      // turn AC off
      daikin_off();

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
                            modeStr = "AC" + String(currentACsetting) + "/FAN" + String(currentFANsetting);
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
                    // Note: when Auto Cooling is on, RHT control is off
                    if(!rht_control_on && !auto_cooling)
                    {
                        if(currentMode == MODE_OFF)
                        {
                            txtOutput += ", NO_Ctrl";

                            if(auto_pause)
                            {
                              txtOutput += ", Auto_Pause_Guarding";
                            }
                        }
                        else
                        {
                            txtOutput += ", " + modeStr + ", Constant";
                        }
                    }
                    else
                    {
                      // current mode
                      txtOutput += ", " + modeStr;

                      if(lastCommandSentInMinutes < useRemainModeTime)
                      {
                          txtOutput += ", Remain";
                      }
                      else if(auto_cooling == true)
                      {
                          txtOutput += ", StandingBy_Auto_Cooling";
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

    // performing daikin control only when RHT Control is enabled and all ambient parameters were obtained
    if(rht_control_on && currentHI > 0 && currentTemp > 0)
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

        // switch on auto-control
        if(!rht_control_on)
        {
          // log the event
          Particle.publish("o2sensor", "RhT-Auto Enabled");

          // restore the defaults
          currentACsetting = AC_START_TEMP;
          daikin_ac_on_set_temp(currentACsetting);
          currentFANsetting = 6;
          auto_cooling = false;
          auto_pause = false;

          // enable RHT Control
          rht_control_on = true;
        }
    }
    else if(ingredient.indexOf("daikin-off") > -1)
    {
      // disable RHT control
      rht_control_on = false;

      // turn AC off
      daikin_off();

      // rht off also cancel Auto Pause
      auto_pause = false;

      // Turn RGB LED off
      rgb_led_off();
    }
    else if(ingredient.indexOf("auto-dryer") > -1)
    {
      // starting RHT auto-control but starting from a dryer mode

      // make it like RHT start
      elapsed_remain_mode_timer = 60 * (long) 60000;
      elapsed_system_up_timer = 0;

      // no RHT control at this moment until Auto Cooling is cancelled
      rht_control_on = false;

      // restore the defaults
      currentACsetting = AC_START_TEMP;
      currentFANsetting = 6;

      // enable Auto Cooling so the dehumidifier mode can be cancelled by temperature
      auto_cooling = true;

      // switch to dehumidifier mode
      daikin_dehumidifier_on();

      // log the event
      Particle.publish("o2sensor", "RhT-Auto Enabled starting from dehumidifier");
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
    else if(ingredient.indexOf("auto-pause") > -1)
    {
      // auto pause timer is a one time valid setting
      // set Auto Pause again to cancel the previously setting
      // it can be set independently from rht on/off control

      if(!auto_pause)
      {
        Particle.publish("o2sensor", "RhT Auto Pause enabled");

        auto_pause = true;

        // lid the RGB LED
        RGB.control(true);
        RGB.color(0, 100, 0);
        delay(1000);
        RGB.brightness(20);
        delay(1000);
      }
      else
      {
        Particle.publish("o2sensor", "RhT Auto Pause cancelled");

        auto_pause = false;

        // Turn RGB LED off
        rgb_led_off();
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

          // mark the current settings
          currentACsetting = coolTemp;
          currentFANsetting = 6;

          // once temperature is controlled, no RHT auto-control
          rht_control_on = false;
        }
        else
        {
          Particle.publish("o2sensor", "Invalid AC temperature command " + String(coolTemp));
        }
      }
    }
    else if(ingredient.indexOf("more-fan") > -1)
    {
      fan_speed_up();
    }
    else if(ingredient.indexOf("less-fan") > -1)
    {
      fan_speed_down();
    }
    else if(ingredient.indexOf("max-fan") > -1)
    {
      currentFANsetting = 1;
      ac_fan_setting();
    }
    else if(ingredient.indexOf("quiet-fan") > -1)
    {
      currentFANsetting = 6;
      ac_fan_setting();
    }
    else if(ingredient.indexOf("too-hot") > -1)
    {
      ac_setting_down();
    }
    else if(ingredient.indexOf("too-cold") > -1)
    {
      ac_setting_up();
    }
    else if(ingredient.indexOf("drying-on") > -1)
    {
      daikin_dehumidifier_on();

      // restore the defaults
      currentACsetting = AC_START_TEMP;
      currentFANsetting = 6;

      // once mode is controlled, no RHT auto-control
      rht_control_on = false;
    }

    // toggle LED as a progress indicator
    digitalWrite(D7, HIGH);
    delay(1000);
    digitalWrite(D7, LOW);
}
