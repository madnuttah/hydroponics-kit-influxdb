//This code is for the Atlas Scientific wifi hydroponics kit that uses the Adafruit huzzah32 as its computer.
//Modified by madnuttah to save to a InfluxDB instead of Thingspeak and extended to use 2 peristaltic pumps for pH control.

#include <iot_cmd.h>
#include <WiFi.h>            //include wifi library
#include <InfluxDbClient.h>  //include influxdb client library
#include <sequencer4.h>      //imports a 4 function sequencer
#include <sequencer1.h>      //imports a 1 function sequencer
#include <Ezo_i2c_util.h>    //brings in common print statements
#include <Ezo_i2c.h>         //include the EZO I2C library from https://github.com/Atlas-Scientific/Ezo_I2c_lib
#include <Wire.h>            //include arduinos i2c library

WiFiClient client;  //declare that this device connects to a Wi-Fi network,create a connection to a specified internet IP address

//----------------Wi-Fi Credentials-------
const String ssid = "";                  //add your wifi ssid
const String pass = "";                  //add your wifi password
//------------------------------------------------------------------

Ezo_board PH = Ezo_board(99, "PH");                     //create a PH circuit object, who's address is 99 and name is "PH"
Ezo_board EC = Ezo_board(100, "EC");                    //create an EC circuit object who's address is 100 and name is "EC"
Ezo_board RTD = Ezo_board(102, "RTD");                  //create an RTD circuit object who's address is 102 and name is "RTD"
Ezo_board PMP_PH_UP = Ezo_board(103, "PMP_PH_UP");      // peristaltic pump – base (raises pH)
Ezo_board PMP_PH_DOWN = Ezo_board(104, "PMP_PH_DOWN");  // peristaltic pump – acid (lowers pH)

Ezo_board device_list[] = {  //an array of boards used for sending commands to all or specific boards
  PH,
  EC,
  RTD,
  PMP_PH_UP,
  PMP_PH_DOWN
};

Ezo_board *default_board = &device_list[0];  //used to store the board were talking to

//gets the length of the array automatically so we dont have to change the number every time we add new boards
const uint8_t device_list_len = sizeof(device_list) / sizeof(device_list[0]);

//------For version 1.4 use these enable pins for each circuit------
//const int EN_PH = 13;
//const int EN_EC = 12;
//const int EN_RTD = 33;
//const int EN_AUX = 27;
//------------------------------------------------------------------

//------For version 1.5 use these enable pins for each circuit------
const int EN_PH = 12;
const int EN_EC = 27;
const int EN_RTD = 15;
const int EN_AUX = 33;
//------------------------------------------------------------------

const unsigned long reading_delay = 1000;    //how long we wait to receive a response, in milliseconds
const unsigned long influxdb_delay = 15000;  //how long we wait to send values to influxdb, in milliseconds

unsigned int poll_delay = 2000 - reading_delay * 2 - 300;  //how long to wait between polls after accounting for the times it takes to send readings

//parameters for setting the pump output
#define PH_LOW 5.70         // Min pH
#define PH_HIGH 6.10        // Max pH
#define PH_UP_DOSAGE 0.5    // mL to dose if pH too low
#define PH_DOWN_DOSAGE 0.5  // mL to dose if pH too high
#define MIXING_TIME 120000  // Time until mixed

//parameters for influxdb
#define INFLUXDB_DB_NAME  ""        // The name of your database
#define INFLUXDB_URL      ""        // URL of your InfluxDB host, like "http://192.168.1.1:8086"
#define INFLUXDB_USER     ""        // Your InfluxDB user
#define INFLUXDB_PASSWORD ""        // Your InfluxDB password

volatile bool mixing = false;            // true while mixing is in progress
volatile unsigned long mixingStart = 0;  // millis() when mixing started

InfluxDBClient influxclient(INFLUXDB_URL, INFLUXDB_DB_NAME);  //create an influxdb client

Point pointMeasurement("hydroponics");  //create an influxdb datapoint

float k_val = 0;  //holds the k value for determining what to print in the help menu

bool polling = true;           //variable to determine whether or not were polling the circuits
bool send_to_influxdb = true;  //variable to determine whether or not were sending data to influxdb

bool wifi_isconnected() {  //function to check if wifi is connected
  return (WiFi.status() == WL_CONNECTED);
}

void reconnect_wifi() {  //function to reconnect wifi if its not connected
  if (!wifi_isconnected()) {
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.println("connecting to wifi");
  }
}

void influxdb_send() {                          //routine to send to influxdb
  if (send_to_influxdb == true) {               //if we're datalogging
    if (wifi_isconnected()) {                   //and connected to wifi
      if (influxclient.validateConnection()) {  //check if the influxdbclient is connected to the database
        Serial.print("Connected to InfluxDB: ");
        Serial.println(influxclient.getServerUrl());
        influxclient.writePoint(pointMeasurement);  //write the datapoint to influxdb
        pointMeasurement.clearFields();             //clearing the fields
        Serial.println("Datapoint written to database");
      } else {
        Serial.print("InfluxDB connection failed: ");
        Serial.println(influxclient.getLastErrorMessage());
      }
    }
  }
}

void step1();  //forward declarations of functions to use them in the sequencer before defining them
void step2();
void step3();
void step4();

Sequencer4 Seq(&step1, reading_delay,  //calls the steps in sequence with time in between them
               &step2, 300,
               &step3, reading_delay,
               &step4, poll_delay);

Sequencer1 Wifi_Seq(&reconnect_wifi, 10000);  //calls the wifi reconnect function every 10 seconds

Sequencer1 Influxdb_Seq(&influxdb_send, influxdb_delay);  //sends data to influxdb with the time determined by influxdb delay

void setup() {

  pinMode(EN_PH, OUTPUT);  //set enable pins as outputs
  pinMode(EN_EC, OUTPUT);
  pinMode(EN_RTD, OUTPUT);
  pinMode(EN_AUX, OUTPUT);
  digitalWrite(EN_PH, LOW);  //set enable pins to enable the circuits
  digitalWrite(EN_EC, LOW);
  digitalWrite(EN_RTD, HIGH);
  digitalWrite(EN_AUX, LOW);
  
  timeSync("Europe/Berlin", "pool.ntp.org"); //set your timezone and ntp server

  Wire.begin();        //start the I2C
  Serial.begin(9600);  //start the serial communication to the computer

  WiFi.mode(WIFI_STA);  //set ESP32 mode as a station to be connected to wifi network

  influxclient.setInsecure();                                                                            //comment out if using ssl
  influxclient.setWriteOptions(WriteOptions().writePrecision(WritePrecision::MS));                       //this creates precise timestamps
  influxclient.setConnectionParamsV1(INFLUXDB_URL, INFLUXDB_DB_NAME, INFLUXDB_USER, INFLUXDB_PASSWORD);  //the influxdb v1 connection parameters

  pointMeasurement.addTag("measurement", "pH/EC/Temp");  //add tags to the point
  pointMeasurement.addTag("location", "");

  Wifi_Seq.reset();  //initialize the sequencers
  Seq.reset();
  Influxdb_Seq.reset();
}

void loop() {
  String cmd;  //variable to hold commands we send to the kit

  Wifi_Seq.run();  //run the sequncer to do the polling

  if (receive_command(cmd)) {                                             //if we sent the kit a command it gets put into the cmd variable
    polling = false;                                                      //we stop polling
    send_to_influxdb = false;                                             //and sending data to influxdb
    if (!process_coms(cmd)) {                                             //then we evaluate the cmd for kit specific commands
      process_command(cmd, device_list, device_list_len, default_board);  //then if its not kit specific, pass the cmd to the IOT command processing function
    }
  }

  if (polling == true) {  //if polling is turned on, run the sequencer
    Seq.run();
    Influxdb_Seq.run();
  }
}

//function that controls the pumps activation and output
void pump_function(Ezo_board &pump, double dose) {
  // Skip if we are mixing
  if (mixing && (millis() - mixingStart < MIXING_TIME)) {
    Serial.println("Mixing in progress – no dosing");
    unsigned long elapsed = millis() - mixingStart;
    Serial.print(F("Mixing… "));
    Serial.print((MIXING_TIME - elapsed) / 1000);
    Serial.println(F("s remaining"));
    return;
  }

  // Stop the mixing timer if the window has elapsed
  if (mixing && (millis() - mixingStart >= MIXING_TIME)) {
    mixing = false;
    Serial.println("Mixing period ended – ready for next dose");
  }

  if (!mixing) {
    if (PH.get_error() == Ezo_board::SUCCESS){
     pump.send_cmd_with_num("d,", dose); //Dispense the dose
     delay(100);
     Serial.print(pump.get_name());  //get pump data to tell the user if the command was received successfully
     Serial.print(" ");
     Serial.print("pump dispensed ");
     pump.send_cmd("x");  // stop the pump
     mixing = true;
     mixingStart = millis();  // start mixing timer
    }
  }
}

void step1() {
  //send a read command. we use this command instead of RTD.send_cmd("R");
  //to let the library know to parse the reading
  RTD.send_read_cmd();
}

void step2() {
  receive_and_print_reading(RTD);                                                                //get the reading from the RTD circuit
  if ((RTD.get_error() == Ezo_board::SUCCESS) && (RTD.get_last_received_reading() > -1000.0)) {  //if the temperature reading has been received and it is valid
    PH.send_cmd_with_num("T,", RTD.get_last_received_reading());
    EC.send_cmd_with_num("T,", RTD.get_last_received_reading());
    pointMeasurement.addField("Temperature", RTD.get_last_received_reading());  //add measurement to influxdb point
  } else {                                                                      //if the temperature reading is invalid
    PH.send_cmd_with_num("T,", 25.0);                                           //send default temp = 25 deg C to PH sensor
    EC.send_cmd_with_num("T,", 25.0);
    pointMeasurement.addField("Temperature", 25.0);  //add measurement to influxdb point
  }

  Serial.print(" ");
}

void step3() {
  //send a read command. we use this command instead of PH.send_cmd("R");
  //to let the library know to parse the reading
  PH.send_read_cmd();
  EC.send_read_cmd();
}

void step4() {
  PMP_PH_UP.send_read_cmd();
  PMP_PH_DOWN.send_read_cmd();
  bool phError = false;
  receive_and_print_reading(PH);                                      //get the reading from the PH circuit
  if (PH.get_error() == Ezo_board::SUCCESS) {                         //if the PH reading was successful (back in step 1)
    pointMeasurement.addField("pH", PH.get_last_received_reading());  //add measurement to influxdb point
  } else {
    phError = true;
  }

  Serial.print("  ");
  receive_and_print_reading(EC);                                      //get the reading from the EC circuit
  if (EC.get_error() == Ezo_board::SUCCESS) {                         //if the EC reading was successful (back in step 1)
    pointMeasurement.addField("EC", EC.get_last_received_reading());  //add measurement to influxdb field
  }

  Serial.println();

  if (phError) {
    Serial.print("pH error, skipping dosing");
    return;
  }

  double pH = PH.get_last_received_reading();

  // Decide which pump to use based on the pH reading
  if (pH <= PH_LOW) {
    // Base pump first
    Serial.println("pH LOW – dosing base");
    pump_function(PMP_PH_UP, PH_UP_DOSAGE);
    // Make sure acid pump stays off
    PMP_PH_DOWN.send_cmd("x");
  } else if (pH >= PH_HIGH) {
    // Acid pump
    Serial.println("pH HIGH – dosing acid");
    pump_function(PMP_PH_DOWN, PH_DOWN_DOSAGE);
    // Keep base pump off
    PMP_PH_UP.send_cmd("x");
  } else {
    // pH within target – turn both pumps off
    Serial.println("pH nominal - nothing to do");
    PMP_PH_UP.send_cmd("x");
    PMP_PH_DOWN.send_cmd("x");
  }
  Serial.println("pH: " + String(pH));
}

void start_datalogging() {
  polling = true;  //set poll to true to start the polling loop
  send_to_influxdb = true;
  Influxdb_Seq.reset();
}

bool process_coms(const String &string_buffer) {  //function to process commands that manipulate global variables and are specifc to certain kits
  if (string_buffer == "HELP") {
    print_help();
    return true;
  } else if (string_buffer.startsWith("DATALOG")) {
    start_datalogging();
    return true;
  } else if (string_buffer.startsWith("POLL")) {
    polling = true;
    Seq.reset();

    int16_t index = string_buffer.indexOf(',');                        //check if were passing a polling delay parameter
    if (index != -1) {                                                 //if there is a polling delay
      float new_delay = string_buffer.substring(index + 1).toFloat();  //turn it into a float

      float mintime = reading_delay * 2 + 300;
      if (new_delay >= (mintime / 1000.0)) {                 //make sure its greater than our minimum time
        Seq.set_step4_time((new_delay * 1000.0) - mintime);  //convert to milliseconds and remove the reading delay from our wait
      } else {
        Serial.println("delay too short");  //print an error if the polling time isnt valid
      }
    }
    return true;
  }
  return false;  //return false if the command is not in the list, so we can scan the other list or pass it to the circuit
}

void get_ec_k_value() {  //function to query the value of the ec circuit
  char rx_buf[10];       //buffer to hold the string we receive from the circuit
  EC.send_cmd("k,?");    //query the k value
  delay(300);
  if (EC.receive_cmd(rx_buf, 10) == Ezo_board::SUCCESS) {  //if the reading is successful
    k_val = String(rx_buf).substring(3).toFloat();         //parse the reading into a float
  }
}

void print_help() {
  get_ec_k_value();
  Serial.println(F("Atlas Scientific I2C hydroponics kit                                       "));
  Serial.println(F("Commands:                                                                  "));
  Serial.println(F("datalog      Takes readings of all sensors every 15 sec send to InfluxDB   "));
  Serial.println(F("             Entering any commands stops datalog mode.                     "));
  Serial.println(F("poll         Takes readings continuously of all sensors                    "));
  Serial.println(F("                                                                           "));
  Serial.println(F("ph:cal,mid,7     calibrate to pH 7                                         "));
  Serial.println(F("ph:cal,low,4     calibrate to pH 4                                         "));
  Serial.println(F("ph:cal,high,10   calibrate to pH 10                                        "));
  Serial.println(F("ph:cal,clear     clear calibration                                         "));
  Serial.println(F("                                                                           "));
  Serial.println(F("ec:cal,dry           calibrate a dry EC probe                              "));
  Serial.println(F("ec:k,[n]             used to switch K values, standard probes values are 0.1, 1, and 10 "));
  Serial.println(F("ec:cal,clear         clear calibration                                     "));

  if (k_val > 9) {
    Serial.println(F("For K10 probes, these are the recommended calibration values:            "));
    Serial.println(F("  ec:cal,low,12880     calibrate EC probe to 12,880us                    "));
    Serial.println(F("  ec:cal,high,150000   calibrate EC probe to 150,000us                   "));
  } else if (k_val > .9) {
    Serial.println(F("For K1 probes, these are the recommended calibration values:             "));
    Serial.println(F("  ec:cal,low,12880     calibrate EC probe to 12,880us                    "));
    Serial.println(F("  ec:cal,high,80000    calibrate EC probe to 80,000us                    "));
  } else if (k_val > .09) {
    Serial.println(F("For K0.1 probes, these are the recommended calibration values:           "));
    Serial.println(F("  ec:cal,low,84        calibrate EC probe to 84us                        "));
    Serial.println(F("  ec:cal,high,1413     calibrate EC probe to 1413us                      "));
  }

  Serial.println(F("                                                                           "));
  Serial.println(F("rtd:cal,t            calibrate the temp probe to any temp value            "));
  Serial.println(F("                     t= the temperature you have chosen                    "));
  Serial.println(F("rtd:cal,clear        clear calibration                                     "));

  Serial.println(F("PMP[N]:[query]       issue a query to a pump named PMP[N]                  "));
  Serial.println(F("  ex: PMP_PH_UP:status    sends the status command to pump named PMP2           "));
  Serial.println(F("      PMP_PH_DOW :d,100     requests that PMP1 dispenses 100ml                    "));
  Serial.println();
  Serial.println(F("      The list of all pump commands is available in the Tri PMP datasheet  "));
  Serial.println();

  Serial.println(F("                                                                           "));
  Serial.println(F("pmp_ph_up:cal,ml     calibrate the pump to a ml value                      "));
  Serial.println(F("                     ml= the volume you have chosen                        "));
  Serial.println(F("pmp_ph_up:cal,clear  clear calibration                                     "));

  Serial.println(F("                                                                           "));
  Serial.println(F("pmp_ph_down:cal,ml   calibrate the pump to a ml value                      "));
  Serial.println(F("                     ml= the volume you have chosen                        "));
  Serial.println(F("pmp_ph_down:cal,clear  clear calibration                                   "));
}
