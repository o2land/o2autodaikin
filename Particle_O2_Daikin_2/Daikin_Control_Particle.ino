/*
  O2Land RHT Dakin Control, Version 2
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
// if temp > AC_TEMP, use AC
// if COLD_TEMP <= temp <= DEH_TEMP, use DEH
// if temp < COLD_TEMP, switch to FAN only
//
// mode is determine and switching every REMAIN_MODE minutes
// during REMAIN_MODE_TIME, if mode = FAN && temp > DEH_TEMP, enable external FAN
//
// boost mode, use lower temp and higher fan without further control
//

// ambient environmental parameters

#define DEH_TEMP                26.00       // DEH_TEMP must be lower than TEMP_AC_CMD degree, otherwise the system will switch between COOLING and DEHUMIDIFIER modes
#define COLD_TEMP               24.00
#define HEAT_INDEX_TOO_HIGH     27.50       // use Heat Index to check if the ambient condition needs to be changed

#define TEMP_AC_CMD             "att24"
#define TEMP_BOOST_CMD          "att23"
#define FAN_SPEED_NIGHT         "atf6"
#define FAN_SPEED_BOOST         "atf4"

// avoid frequent ON-OFF mode switch
//   when the mode is switched, no more action is allowed before this timer is reached
#define REMAIN_MODE_TIME        25          // in munutes

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

// -----------------------------------------------------------------------------------
void setup() {
    Serial1.begin(38400);

    // Blue LED
    pinMode(D7, OUTPUT);
    
    // Turn RGB LED off
    RGB.control(true);
    RGB.color(0, 0, 0);
    delay(1000);
    RGB.brightness(0);
    delay(1000);

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

    // set the air conditioning to the default modes
    Serial1.println(TEMP_AC_CMD);
    Serial1.println(FAN_SPEED_NIGHT);
            
    // log the event
    Particle.publish("o2sensor", "RhT Control System Initialized, 2016-10-10");
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
                    String txtOutput = "Rh" + String(xRh, 2) + ", T" + String(xTemp, 2) + ", HI" + String(xHI,2) + ", " + modeStr;
                    
                    // added the conditional stat events
                    if(!rht_control_on)
                    {
                        txtOutput += ", NO_Ctrl";
                    }
                    else if(lastCommandSentInMinutes < REMAIN_MODE_TIME)
                    {
                        txtOutput += ", Remain";
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

    // performing daikin control only when RHT Control is enabled and not in boost mode
    if(rht_control_on && !daikin_boost)
    {
      // avoid frequent mode switching
      if(lastCommandSentInMinutes >= REMAIN_MODE_TIME)
      {
        // if temp > DEH_TEMP, use AC
        if(currentMode != MODE_COOLING &&
           ((float) currentTemp > (float) DEH_TEMP))
        {
          daikin_ac_on();
        }
        
        // .....................................................

        // if COLD_TEMP <= temp <= DEH_TEMP, use DEH
        else if(currentMode != MODE_DEHUMIDIFIER &&
           ((float) currentTemp >= (float) COLD_TEMP) &&
           ((float) currentTemp <= (float) DEH_TEMP))
        {
          daikin_dehumidifier_on();
        }

        // .....................................................

        // if temp < COLD_TEMP, switch to FAN only
        else if(currentMode != MODE_FAN &&
           ((float) currentTemp < (float) COLD_TEMP))
        {
          daikin_fan_on();
        }
      } // end REMAIN_MODE_TIME
      
      //*******************************************************
      
      // outside the REMAIN_MODE_TIME, control the external fan
      if(!current_fan_mode_on)
      {
        // external fan currently OFF
        if(currentMode == MODE_FAN &&
           ((float) currentTemp > (float) DEH_TEMP))
        {
          fan_on();
        }
      }
      else
      {
        // external fan currently ON
        if(currentMode != MODE_FAN)
        {
          fan_off();
        }
      }
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
    }

    // toggle LED as a progress indicator
    digitalWrite(D7, HIGH);
    delay(1000);
    digitalWrite(D7, LOW);
}

