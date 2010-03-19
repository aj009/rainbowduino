/*
* Implementation
*/

#include "Connection.h"

#include "WProgram.h"

#include <EEPROM.h>
#include "Wire.h"

Connection Con; 

void onReceiveInitCallback(int howMany) {
  uint8_t command = Wire.receive();  
  if( command == I2C_CMD_OK) {  //Maybe TODO send explizit -I2C_CMD_SET_ADDRESS- command
    Con.i2c_address = Wire.receive();
  }
}

//Master I2C Callback
void onReceiveMasterCallback(int howMany) {
  uint8_t command = Wire.receive();
  if( command == I2C_CMD_HELLO) {  //we can't instant replay here ;-(
    command = Wire.receive();
    if( Con.slave_address_to_register == 0 )  Con.slave_address_to_register = command;
  }
  else {   //forward data to serial
    //Serial.write(command);
    //Serial.println(command, DEC);
    //TODO send slave number???
    //TODO send message length??
    while( howMany > 1 ) {
      command = Wire.receive();
      //Serial.println( command, DEC);
      howMany--;
    } 
  }
}

//Slave I2C Callback
void onReceiveSlaveCallback(int howMany) {
  Con.inputMessage->length = howMany; //MaybeTODO use explizit -initMessage- command
  while( howMany > 0 ) {
    Con.processMessage( Wire.receive() );
    howMany--;
  }
}

void Connection::begin() {
  master = false;
  i2c_address = 0;

//TODO: if we detect a working serial port, take master role in any case, notify all others to restart
// -> http://stackoverflow.com/questions/195304/check-if-serial-port-is-listening

  // 1. read last_adress from EEPROM and normalize to value between 0-16 
  uint8_t old_i2c_address = EEPROM.read(I2C_EEPROM_ADR) & 0x0F;

  // 2. startup delay 
  if( old_i2c_address > I2C_MASTER_ADR) {
     int delay_time =  (old_i2c_address - I2C_MASTER_ADR ) * STARTUP_DELAY_FACTOR;
     delay(delay_time);
  }
  // 3. open I2C connection with I2C_START_ADR as offset
  i2c_address = old_i2c_address + I2C_START_ADR;  
  Wire.begin(i2c_address);

  // 4. register Callback fnc
  Wire.onReceive(onReceiveInitCallback);

  //5. wait for response with new slave address
  uint8_t c = I2C_WAIT_FOR_ADDRESS_RETRYS;  
  int delay_time2 =  max(1, old_i2c_address - I2C_MASTER_ADR) * I2C_WAIT_FOR_ADDRESS_TIMEOUT;
  while( i2c_address >= I2C_START_ADR && c > 0) {  
    // 5a. make call to master with static number.
    Wire.beginTransmission(I2C_MASTER_ADR);
    Wire.send(I2C_CMD_HELLO);
    Wire.send(i2c_address);   //send adress where to respond to
    Wire.endTransmission();
    delay(delay_time2);
    c--;
  }  

  if(i2c_address < I2C_START_ADR) {  //success
    beginSlave(i2c_address, false);
  }
  else { // failure
    beginMaster(I2C_MASTER_ADR, false);
  }
}

void Connection::beginMaster(uint8_t master_address, bool update_adress) {
  //6b. use master I2C address, start master mode, open serial
  i2c_address = master_address;
  Serial.begin(BAUD_RATE);

  //7. start new I2C with given address
  if(update_adress) {
    Wire.begin(i2c_address);
  }
  else {
    TWAR = i2c_address << 1; // twi_setAddress
  }
   
  Wire.onReceive(onReceiveMasterCallback);
  master = true;

  //8. save new adress to EEPROM
  EEPROM.write(I2C_EEPROM_ADR, i2c_address);
  
  inputMessage->reset();
  slave_address_to_register = 0;
  
  last_slave_address = I2C_SLAVE_ADR;
  
  inputMessage  = (Message*) malloc( sizeof(Message)); //buffer1; //point to buffers
  outputMessage = (Message*) malloc( sizeof(Message)); //buffer2; //point to buffers
}

void Connection::beginSlave(uint8_t slave_address, bool update_adress) {
  //6a. use slave I2C adress, start slave mode
  i2c_address = slave_address;

  //7. start new I2C with given address
  if(update_adress) {
    Wire.begin(i2c_address);
  }
  else {
    TWAR = i2c_address << 1; // twi_setAddress
  }  
  Wire.onReceive(onReceiveSlaveCallback);
  master = false;

  //8. save new adress to EEPROM
  EEPROM.write(I2C_EEPROM_ADR, i2c_address);
}

/******************************************************************/
void Connection::registerPendingSlave() {
  if( slave_address_to_register == 0) return;
  last_slave_address++;
  Wire.beginTransmission(slave_address_to_register);
  Wire.send(I2C_CMD_OK);
  Wire.send(last_slave_address);   //send adress where to respond to
  Wire.endTransmission();

  //notify serial about new slave
  command( SLAVE_NEW, last_slave_address);
  
  slave_address_to_register = 0;
}


/******************************************************************/
void Connection::onMessageAvailable(conCallbackFunction newFunction) {
  onMessageAvailableCallback = newFunction;
}

void Connection::loop() {  
  registerPendingSlave();
}  


uint8_t Connection::process(uint8_t serialByte) {
  Rainbowduino.set_frame_line(0,7,serialByte,0,0);
  
  if( inputMessage->type == 0) {
    if( serialByte == COMMAND || serialByte == OK || serialByte == ERROR ) { 
      inputMessage->type = serialByte;
      inputMessage->index = 0;
    }
    return 1;
  }
  
  if( inputMessage->receiver > 128 ) {
    inputMessage->receiver = serialByte;
    return 2;
  }    
  
  if( inputMessage->length < 1 ) {
    //TODO waht if size > 64????
    inputMessage->length = serialByte;
    return 3;
  }     

  processMessage(serialByte);
}

uint8_t Connection::processMessage(uint8_t serialByte) {
  if( inputMessage->type == 0 || inputMessage->receiver > 128 || inputMessage->length < 1 ) return 3;
  inputMessage->data[inputMessage->index++] = serialByte;

  if( inputMessage->index == inputMessage->length ) {
    //message fully received
    //1. swap buffers
    Message* tmpPointer = outputMessage;
    outputMessage = inputMessage;
    outputMessage->index = 0; //reset()
    
    //2. reset inputBuffer and read process
    inputMessage = tmpPointer;
    inputMessage->reset();
    //3. execute callback
    if(onMessageAvailableCallback != NULL) (*onMessageAvailableCallback)();
  }
  //TODO if (bufferIndex >= bufferLastIndex) reset();
  return 10;
}

uint8_t Connection::read() {
  if( available() < 1) return 0;
  return outputMessage->data[outputMessage->index++];
}

bool Connection::available() {
  return outputMessage->ready();
}


void Connection::write(uint8_t w) {
}

void Connection::ok(uint8_t command, uint8_t param) {
  if( master ) {
//    Serial.flush();
      Serial.write(OK);
      Serial.write((byte) 0); //as we are master!
      Serial.write(2); //two more to follow
      Serial.write(command);
      Serial.write(param);
  }
  else {
    //TODO I2C
  }
}

void Connection::command(uint8_t command, uint8_t param) {
  if( master ) {
//    Serial.flush();
    Serial.write(COMMAND);
    Serial.write((byte) 0); //as we are master!
    Serial.write(2); //two more to follow
    Serial.write(command);
    Serial.write(param);
  }
  else {
    //TODO I2C
  }
}

/*********************************************************/
Message::Message() {
    reset();
}

void Message::reset() {
    type = 0;
    receiver = 255;  
    length = 0;
    command = 0;
    index = 0;
}

bool Message::ready() {
    return (length == index);
}