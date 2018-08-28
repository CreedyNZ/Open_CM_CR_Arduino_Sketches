#include "globals.h"

//-----------------------------------------------------------------------------
// Enums and defines
//-----------------------------------------------------------------------------
enum {AX_SEARCH_FIRST_FF = 0, AX_SEARCH_SECOND_FF, PACKET_ID, PACKET_LENGTH,
      PACKET_INSTRUCTION, AX_SEARCH_RESET, AX_SEARCH_BOOTLOAD, AX_GET_PARAMETERS,
      AX_SEARCH_READ, AX_SEARCH_PING, AX_PASS_TO_SERVOS,
      DXL_P2_ID, DXL_P2_LENGTH_L, DXL_P2_LENGTH_H, DXL_P2_INSTRUCTION, DXL_P2_PARAMETERS, DXL_P2_PASS_TO_SERVOS
     };

// CM-904 address table
// Understand P1 and P2 packets
//     0   1  2  3   4   5    6     7   8...
// P1: ff ff id len INST P1...PN checksum
// P2: ff ff fd  0   id  lenL lenH INST P1...PN CRC_L CRC_H
enum {DXL_P2_ID_INDEX = 4, DXL_P2_LEN_L_INDEX, DXL_P2_LEN_H_INDEX, DXL_P2_INST_INDEX, DXL_P2_PARMS_INDEX};
enum {
  CM904_MODEL_NUMBER_L              = 0,
  CM904_MODEL_NUMBER_H              = 1,
  CM904_FIRMWARE_VERSION            = 2,
  CM904_ID                          = 3,
  CM904_BAUD_RATE                   = 4,
  CM904_RETURN_DELAY_TIME           = 5,
  CM904_SEND_TIMEOUT                = 6,
  CM904_RECEIVE_TIMEOUT             = 7,
  CM904_STATUS_RETURN_LEVEL         = 16,
  CM904_BUTTON_STATUS               = 26,
  CM904_RANDOM_NUMBER               = 77,
  CM904_GREEN_LED                   = 79,
  CM904_MOTION_LED                  = 82
};

//Dynamixel device Control table
#define AX_ID_DEVICE        200    // Default ID
#define AX_ID_BROADCAST     0xfe
#define MODEL_NUMBER_L      0x90  //  model #400 by E-manual
#define MODEL_NUMBER_H      0x01
#define FIRMWARE_VERSION    0x05  // Firmware version, needs to be updated with every new release
#define BAUD_RATE           0x03
#define RETURN_LEVEL         2
#define REG_TABLE_SIZE      83
#define     USART_TIMEOUT       50   //  x 20us
#define     SEND_TIMEOUT        4    //  x 20us
#define   RECEIVE_TIMEOUT       100  //  x 20us

/** Error Levels **/
#define ERR_NONE                    0
#define ERR_VOLTAGE                 1
#define ERR_ANGLE_LIMIT             2
#define ERR_OVERHEATING             4
#define ERR_RANGE                   8
#define ERR_CHECKSUM                16
#define ERR_OVERLOAD                32
#define ERR_INSTRUCTION             64

#define DXL_PING                    1
#define DXL_READ_DATA               2
#define DXL_WRITE_DATA              3
#define DXL_REG_WRITE               4
#define DXL_ACTION                  5
#define DXL_RESET                   6
#define DXL_SYNC_READ               0x82
#define DXL_SYNC_WRITE              0x83

#define SYNC_READ_START_ADDR  5
#define SYNC_READ_LENGTH      6

//=========================================================================
// Globals
//=========================================================================
uint8_t from_usb_buffer[BUFFER_SIZE];
uint16_t from_usb_buffer_count = 0;
uint8_t from_port_buffer[BUFFER_SIZE];
uint8_t tx_packet_buffer[DXL_MAX_RETURN_PACKET_SIZE];
unsigned long last_message_time;
uint16_t  packet_length;

uint16_t dxl_protocol1_checksum = 0;
uint8_t dxl_usb_input_state;
bool g_data_output_to_usb = false;

//                                                    0             1              2                   3            4             5              6              7
uint8_t g_controller_registers[REG_TABLE_SIZE] = {MODEL_NUMBER_L, MODEL_NUMBER_H, FIRMWARE_VERSION, AX_ID_DEVICE, USART_TIMEOUT, SEND_TIMEOUT, RECEIVE_TIMEOUT
                                                 };
// Protocol 1 version
const uint8_t g_controller_registers_ranges[][2] =
{
  {1, 0},   //MODEL_NUMBER_L        0
  {1, 0},   //MODEL_NUMBER_H        1
  {1, 0},   //VERSION               2
  {0, 253}, //ID                    3
  {1, 254}, //BAUD_RATE             4
  {0, 254}, //Return Delay time     5
};




//-----------------------------------------------------------------------------
// Forward references
//-----------------------------------------------------------------------------
extern void pass_bytes(uint8_t nb_bytes);
extern void passBufferedDataToServos(void);
extern void LocalRegistersRead(uint16_t register_id, uint16_t count_bytes, uint8_t protocol);
extern void LocalRegistersWrite(uint16_t register_id, uint8_t* data, uint16_t count_bytes, uint8_t protocol);
extern uint16_t update_crc(uint16_t crc_accum, uint8_t *data_blk_ptr, uint16_t data_blk_size);

// Helper functions for write
extern uint8_t ValidateWriteData(uint8_t register_id, uint8_t* data, uint8_t count_bytes);
extern void SaveEEPromSectionsLocalRegisters(void);
extern void UpdateHardwareAfterLocalWrite(uint8_t register_id, uint8_t count_bytes);
extern void CheckHardwareForLocalReadRequest(uint8_t register_id, uint8_t count_bytes);

//-----------------------------------------------------------------------------
// InitializeHardwareAndRegisters
//-----------------------------------------------------------------------------
void InitalizeHardwareAndRegisters() {
  pinMode(BOARD_LED_PIN, OUTPUT);
  digitalWrite(BOARD_LED_PIN, LOW);

  // initialize the Button pin
  pinMode(BOARD_BUTTON_PIN, INPUT_PULLDOWN);

}

//=========================================================================
// Process USB input data...
//=========================================================================
bool ProcessUSBInputData() {
  bool we_did_something = false;
  // Main loop, lets loop through reading any data that is coming in from the USB
  while (Serial.available())
  {
    we_did_something = true;
    uint8_t ch = Serial.read();
    last_message_time = micros();
    switch (dxl_usb_input_state) {
      case AX_SEARCH_FIRST_FF:
        from_usb_buffer[0] = ch;
        if (ch == 0xFF) {
          dxl_usb_input_state = AX_SEARCH_SECOND_FF;
          from_usb_buffer_count = 1;
        } else {
          portHandler->writePort(&ch, 1);
        }
        break;

      case AX_SEARCH_SECOND_FF:
        from_usb_buffer[from_usb_buffer_count++] = ch;
        if (ch == 0xFF) {
          dxl_usb_input_state = PACKET_ID;
        } else {
          passBufferedDataToServos();
        }
        break;

      case PACKET_ID:
        from_usb_buffer[from_usb_buffer_count++] = ch;
        if (ch == 0xFF) { // we've seen 3 consecutive 0xFF
          from_usb_buffer_count--;
          pass_bytes(1); // let a 0xFF pass
        } else {
          dxl_usb_input_state = PACKET_LENGTH;

          // Check to see if we should start sending out the data here.
          if (from_usb_buffer[PACKET_ID] != g_controller_registers[CM904_ID] && from_usb_buffer[PACKET_ID] != AX_ID_BROADCAST ) {
            pass_bytes(from_usb_buffer_count);
          }
        }
        break;

      case PACKET_LENGTH:
        from_usb_buffer[from_usb_buffer_count++] = ch;
        packet_length = from_usb_buffer[PACKET_LENGTH];
        if ((from_usb_buffer[PACKET_ID] == 0xfd) && (ch == 0) ) {
          // Looks like a protocol 2 packet
          dxl_usb_input_state = DXL_P2_ID;

        } else if (from_usb_buffer[PACKET_ID] == g_controller_registers[CM904_ID] || from_usb_buffer[PACKET_ID] == AX_ID_BROADCAST ) {
          if (packet_length > 1 && packet_length < (AX_SYNC_READ_MAX_DEVICES + 4)) { // reject message if too short or too big for from_usb_buffer buffer
            dxl_usb_input_state = PACKET_INSTRUCTION;
          } else {
            sendProtocol1StatusPacket(ERR_RANGE, NULL, 0);
            passBufferedDataToServos();
          }
        } else {
          portHandler->writePort(&ch, 1);
          dxl_usb_input_state = AX_PASS_TO_SERVOS;
        }
        break;

      case PACKET_INSTRUCTION:
        from_usb_buffer[from_usb_buffer_count++] = ch;
        if (from_usb_buffer[PACKET_INSTRUCTION] == DXL_SYNC_READ) {
          dxl_usb_input_state = AX_GET_PARAMETERS;
          dxl_protocol1_checksum =  from_usb_buffer[PACKET_ID] + DXL_SYNC_READ + from_usb_buffer[PACKET_LENGTH];
        } else if (from_usb_buffer[PACKET_ID] == g_controller_registers[CM904_ID]) {
          if (from_usb_buffer[PACKET_INSTRUCTION] == DXL_PING) {
            dxl_usb_input_state = AX_SEARCH_PING;
          } else if (from_usb_buffer[PACKET_INSTRUCTION] == DXL_READ_DATA) {
            dxl_usb_input_state = AX_GET_PARAMETERS;
            dxl_protocol1_checksum = g_controller_registers[CM904_ID] + DXL_READ_DATA + from_usb_buffer[PACKET_LENGTH];
          } else if (from_usb_buffer[PACKET_INSTRUCTION] == DXL_WRITE_DATA) {
            dxl_usb_input_state = AX_GET_PARAMETERS;
            dxl_protocol1_checksum = g_controller_registers[CM904_ID] + DXL_WRITE_DATA + from_usb_buffer[PACKET_LENGTH];
          } else {
            passBufferedDataToServos();
          }
        } else {
          passBufferedDataToServos();
        }
        break;

      case AX_SEARCH_PING:
        from_usb_buffer[5] = ch;
        if (((g_controller_registers[CM904_ID] + 2 + DXL_PING + from_usb_buffer[5]) % 256) == 255) {
          sendProtocol1StatusPacket(ERR_NONE, NULL, 0);
          dxl_usb_input_state = AX_SEARCH_FIRST_FF;
        } else {
          passBufferedDataToServos();
        }
        break;

      case AX_GET_PARAMETERS:
        from_usb_buffer[from_usb_buffer_count] = ch;
        dxl_protocol1_checksum += from_usb_buffer[from_usb_buffer_count] ;
        from_usb_buffer_count++;
        if (from_usb_buffer_count >= (from_usb_buffer[PACKET_LENGTH] + 4)) { // we have read all the data for the packet
          if ((dxl_protocol1_checksum % 256) != 255) { // ignore message if checksum is bad
            passBufferedDataToServos();
          } else {
            if (from_usb_buffer[PACKET_INSTRUCTION] == DXL_SYNC_READ) {
#ifdef LATER
              uint8_t nb_servos_to_read = from_usb_buffer[PACKET_LENGTH] - 4;
              uint8_t packet_overhead = 6;
              if ( (from_usb_buffer[SYNC_READ_LENGTH] == 0)
                   || (from_usb_buffer[SYNC_READ_LENGTH] > AX_BUFFER_SIZE - packet_overhead) // the return packets from the servos must fit the return buffer
                   || ( (int16_t)from_usb_buffer[SYNC_READ_LENGTH] * nb_servos_to_read > DXL_MAX_RETURN_PACKET_SIZE - packet_overhead )) { // and the return packet to the host must not be bigger either
                sendProtocol1StatusPacket(ERR_RANGE, NULL, 0);
              } else {
                sync_read(from_usb_buffer[PACKET_ID], &from_usb_buffer[SYNC_READ_START_ADDR], from_usb_buffer[PACKET_LENGTH] - 2);
              }
#endif // LATER
            } else if (from_usb_buffer[PACKET_INSTRUCTION] == DXL_READ_DATA) {
              LocalRegistersRead(from_usb_buffer[5], from_usb_buffer[6], 1);
            } else if (from_usb_buffer[PACKET_INSTRUCTION] == DXL_WRITE_DATA) {
              LocalRegistersWrite(from_usb_buffer[5], &from_usb_buffer[6], from_usb_buffer[PACKET_LENGTH] - 3, 1);
            }
            dxl_usb_input_state = AX_SEARCH_FIRST_FF;
          }
        }
        break;

      case AX_PASS_TO_SERVOS:
        portHandler->writePort(&ch, 1);
        from_usb_buffer_count++;
        if (from_usb_buffer_count >= (from_usb_buffer[PACKET_LENGTH] + 4)) { // we have read all the data for the packet // we have let the right number of bytes pass
          dxl_usb_input_state = AX_SEARCH_FIRST_FF;
        }
        break;
      //------------------------------------------------------------------
      // Protocol 2 states
      case DXL_P2_ID:
        from_usb_buffer[from_usb_buffer_count++] = ch;
        dxl_usb_input_state = DXL_P2_LENGTH_L;

        // Check to see if we should start sending out the data here.
        if (ch != g_controller_registers[CM904_ID] ) {
          pass_bytes(from_usb_buffer_count);
        }
        break;
      case DXL_P2_LENGTH_L:
        from_usb_buffer[from_usb_buffer_count++] = ch;
        dxl_usb_input_state = DXL_P2_LENGTH_H;
        break;
      //enum {DXL_P2_ID_INDEX=4, DXL_P2_LEN_L_INDEX, DXL_P2_LEN_H_INDEX, DXL_P2_INST_INDEX};
      case DXL_P2_LENGTH_H:
        from_usb_buffer[from_usb_buffer_count++] = ch;
        packet_length = (from_usb_buffer[DXL_P2_LEN_H_INDEX] << 8) + from_usb_buffer[DXL_P2_LEN_L_INDEX];
        if (from_usb_buffer[DXL_P2_ID_INDEX] == g_controller_registers[CM904_ID]  ) {
          if (from_usb_buffer[PACKET_LENGTH] > 1 && from_usb_buffer[PACKET_LENGTH] < (AX_SYNC_READ_MAX_DEVICES + 4)) { // reject message if too short or too big for from_usb_buffer buffer
            dxl_usb_input_state = DXL_P2_INSTRUCTION;
          } else {
            sendProtocol1StatusPacket(ERR_RANGE, NULL, 0);
            passBufferedDataToServos();
          }
        } else {
          portHandler->writePort(&from_usb_buffer[DXL_P2_LEN_L_INDEX], 2);
          dxl_usb_input_state = DXL_P2_PASS_TO_SERVOS;
        }
        break;
      case DXL_P2_INSTRUCTION:
        from_usb_buffer[from_usb_buffer_count++] = ch;
        if ((ch == DXL_READ_DATA) || (ch == DXL_WRITE_DATA) || (ch == DXL_PING)) {
          dxl_usb_input_state = DXL_P2_PARAMETERS;
        } else {
          passBufferedDataToServos();
        }
        break;
      case DXL_P2_PARAMETERS:
        from_usb_buffer[from_usb_buffer_count] = ch;
        from_usb_buffer_count++;
        if (from_usb_buffer_count >= (packet_length + 7)) { // we have read all the data for the packet
          // Need to do P2 checksum....
          if (0 /*(dxl_protocol1_checksum % 256) != 255 */) { // ignore message if checksum is bad
            passBufferedDataToServos();
          } else {
            if (from_usb_buffer[PACKET_INSTRUCTION] == DXL_READ_DATA) {
              // Remember register index and count are 16 bits...
              LocalRegistersRead(from_usb_buffer[DXL_P2_PARMS_INDEX] + from_usb_buffer[DXL_P2_PARMS_INDEX + 1] << 8,
                                 from_usb_buffer[DXL_P2_PARMS_INDEX + 2] + from_usb_buffer[DXL_P2_PARMS_INDEX + 3], 2);
            } else if (from_usb_buffer[PACKET_INSTRUCTION] == DXL_WRITE_DATA) {
              LocalRegistersWrite(from_usb_buffer[DXL_P2_PARMS_INDEX] + from_usb_buffer[DXL_P2_PARMS_INDEX + 1] << 8,
                                  &from_usb_buffer[2], packet_length - 5, 2);
            } else if (from_usb_buffer[PACKET_INSTRUCTION] == DXL_PING) {
                sendProtocol2StatusPacket(0, g_controller_registers, 3);   // returns first three bytes of our register table...
            }
            dxl_usb_input_state = AX_SEARCH_FIRST_FF;
          }
        }

        break;
      case DXL_P2_PASS_TO_SERVOS:
        portHandler->writePort(&ch, 1);
        from_usb_buffer_count++;
        if (from_usb_buffer_count >= (packet_length + 7)) { // we have read all the data for the packet // we have let the right number of bytes pass
          dxl_usb_input_state = AX_SEARCH_FIRST_FF;
        }
      default:
        break;
    }
  }
  // Timeout on state machine while waiting on further USB data
  if (dxl_usb_input_state != AX_SEARCH_FIRST_FF) {
    if ((micros() - last_message_time) > (20 * g_controller_registers[CM904_RETURN_DELAY_TIME])) {
      pass_bytes(from_usb_buffer_count);
      dxl_usb_input_state = AX_SEARCH_FIRST_FF;
    }
  }

  return we_did_something;

}

//-----------------------------------------------------------------------------
// pass_bytes - Switch the AX Buss to output and output the number of bytes
//     from our buffered rx buffer.
//-----------------------------------------------------------------------------
void pass_bytes(uint8_t nb_bytes) {
  if (nb_bytes) {
    portHandler->writePort(from_usb_buffer, nb_bytes);
  }
}

//-----------------------------------------------------------------------------
// passBufferedDataToServos - take any data that we read in and now output the
// data over the AX Buss.
//-----------------------------------------------------------------------------
void passBufferedDataToServos(void) {
  // if the last byte read is not what it's expected to be, but is a 0xFF, it could be the first 0xFF of an incoming command
  if (from_usb_buffer[from_usb_buffer_count - 1] == 0xFF) {
    // we trade the latest 0xFF received against the first one that is necessarily in the first position of the buffer
    pass_bytes(from_usb_buffer_count - 1); // pass the discarded data, except the last 0xFF
    dxl_usb_input_state = AX_SEARCH_SECOND_FF;
    from_usb_buffer_count = 1; // keep the first 0xFF in the buffer
    last_message_time = 0;
  } else {
    pass_bytes(from_usb_buffer_count);
    dxl_usb_input_state = AX_SEARCH_FIRST_FF;
  }
}

extern void pass_bytes(uint8_t nb_bytes);

//-----------------------------------------------------------------------------
// FlushUSBInutQueue - Flush all of the data out of the input queue...
//-----------------------------------------------------------------------------
void FlushUSBInputQueue(void)
{
  while (Serial.available()) {
    Serial.read();
  }
}

//-----------------------------------------------------------------------------
// If we are in pass through and we don't still have any data coming in to
// us, maybe we should tell USB to send back the data now!
//-----------------------------------------------------------------------------
void MaybeFlushUSBOutputData()
{
  if (g_data_output_to_usb)
  {
    g_data_output_to_usb = false;
    Serial.flush();
  }
}

//-----------------------------------------------------------------------------
// sendProtocol1StatusPacket - Send status packet back through USB
//-----------------------------------------------------------------------------
void sendProtocol1StatusPacket(uint8_t err, uint8_t* data, uint8_t count_bytes) {

  uint16_t checksum = g_controller_registers[CM904_ID] + 2 + count_bytes + err;
  Serial.write(0xff);
  Serial.write(0xff);
  Serial.write(g_controller_registers[CM904_ID]);
  Serial.write(2 + count_bytes);
  Serial.write(err);
  for (uint8_t i = 0; i < count_bytes; i++) {
    Serial.write(data[i]);
    checksum += data[i];
  }
  Serial.write(255 - (checksum % 256));
  g_data_output_to_usb = false;
  Serial.flush();
}

//-----------------------------------------------------------------------------
// sendProtocol2StatusPacket - Send status packet back through USB
//-----------------------------------------------------------------------------
void sendProtocol2StatusPacket(uint8_t err, uint8_t* data, uint16_t count_bytes) {

  uint8_t *packet = tx_packet_buffer;
  *packet++ = 0xFF;   //0
  *packet++ = 0xFF;   // 1
  *packet++ = 0xFd;   // 2
  *packet++ = 0;      // 3
  *packet++ = g_controller_registers[CM904_ID];     // 4
  *packet++ = (count_bytes + 4) & 0xff; // 5 LSB count
  *packet++ = (count_bytes + 4) >> 8; // 6 MSB count
  *packet++ = 0x55;   // 7 Instruction
  *packet++ = err;
  for (uint8_t i = 0; i < count_bytes; i++) {
    *packet++ = data[i];
  }
  uint16_t calculated_crc = update_crc ( 0, tx_packet_buffer, packet - tx_packet_buffer) ;
  *packet++ = calculated_crc & 0xff;
  *packet++ = (calculated_crc >> 8) & 0xff;
  Serial.write(tx_packet_buffer, packet - tx_packet_buffer);
  g_data_output_to_usb = false;
  Serial.flush();   // make sure it goes out as quick as possible
}

//-----------------------------------------------------------------------------
// LocalRegistersRead
//-----------------------------------------------------------------------------
void LocalRegistersRead(uint16_t register_id, uint16_t count_bytes, uint8_t protocol)
{

  // Several ranges of logical registers to process.
  uint16_t top = (uint16_t)register_id + count_bytes;
  if ( count_bytes == 0  || (top >= REG_TABLE_SIZE))
  {
    if (protocol == 1)
      sendProtocol1StatusPacket(ERR_RANGE, g_controller_registers + register_id, count_bytes);
    else
      sendProtocol2StatusPacket(ERR_RANGE, g_controller_registers + register_id, count_bytes);
    return;
  }

  // See if we need to do any preprocesing.
  CheckHardwareForLocalReadRequest(register_id, count_bytes);
  if (protocol == 1)
    sendProtocol1StatusPacket(ERR_NONE, g_controller_registers + register_id, count_bytes);
  else
    sendProtocol2StatusPacket(ERR_NONE, g_controller_registers + register_id, count_bytes);
}


//-----------------------------------------------------------------------------
// LocalRegistersWrite: Update the local registers
//-----------------------------------------------------------------------------
void LocalRegistersWrite(uint16_t register_id, uint8_t* data, uint16_t count_bytes, uint8_t protocol)
{
  if ( ! ValidateWriteData(register_id, data, count_bytes) ) {
    if (protocol == 1)
      sendProtocol1StatusPacket(ERR_RANGE, g_controller_registers + register_id, count_bytes);
    else
      sendProtocol2StatusPacket(ERR_RANGE, g_controller_registers + register_id, count_bytes);
  } else {
    memcpy(g_controller_registers + register_id, data, count_bytes);

#ifdef LATER
    // If at least some of the registers set is in the EEPROM area, save updates
    if (register_id <= CM730_STATUS_RETURN_LEVEL)
      SaveEEPromSectionsLocalRegisters();
#endif
    if (protocol == 1)
      sendProtocol1StatusPacket(ERR_NONE, g_controller_registers + register_id, count_bytes);
    else
      sendProtocol2StatusPacket(ERR_NONE, g_controller_registers + register_id, count_bytes);

    // Check to see if we need to do anything to the hardware in response to the
    // changes
    UpdateHardwareAfterLocalWrite(register_id, count_bytes);
  }
}

//-----------------------------------------------------------------------------
// CheckHardwareForLocalReadRequest - Some of this will change later to probably
//        some background tasks...
//-----------------------------------------------------------------------------
void CheckHardwareForLocalReadRequest(uint8_t register_id, uint8_t count_bytes)
{
  while (count_bytes) {
    switch (register_id) {
      case CM904_BUTTON_STATUS:
        g_controller_registers[CM904_BUTTON_STATUS] = digitalRead(BOARD_BUTTON_PIN);
        break;
    }
    register_id++;
    count_bytes--;
  }
}

//-----------------------------------------------------------------------------
// ValidateWriteData: is this a valid range of registers to update?
//-----------------------------------------------------------------------------
uint8_t ValidateWriteData(uint8_t register_id, uint8_t* data, uint8_t count_bytes)
{
  uint16_t top = (uint16_t)register_id + count_bytes;
  if (count_bytes == 0  || ( top >= REG_TABLE_SIZE)) {
    return false;
  }
  // Check that the value written are acceptable
  for (uint8_t i = 0 ; i < count_bytes; i++ ) {
    uint8_t val = data[i];
    if ((val < g_controller_registers_ranges[register_id][0] ) ||
        (val > g_controller_registers_ranges[register_id][1] ))
    {
      return false;
    }
  }
  return true;
}

//-----------------------------------------------------------------------------
// UpdateHardwareAfterLocalWrite
//-----------------------------------------------------------------------------
void UpdateHardwareAfterLocalWrite(uint8_t register_id, uint8_t count_bytes)
{
  while (count_bytes) {
    switch (register_id) {
      case CM904_GREEN_LED:
        digitalWriteFast(BOARD_LED_PIN, g_controller_registers[CM904_GREEN_LED]);
        break;
      case CM904_MOTION_LED:
        break;
    }
    register_id++;
    count_bytes--;
  }
}

//==========================================================================================
// Protocol 2 stuff.
uint16_t update_crc(uint16_t crc_accum, uint8_t *data_blk_ptr, uint16_t data_blk_size)
{
  uint16_t i, j;
  uint16_t crc_table[256] = {
    0x0000, 0x8005, 0x800F, 0x000A, 0x801B, 0x001E, 0x0014, 0x8011,
    0x8033, 0x0036, 0x003C, 0x8039, 0x0028, 0x802D, 0x8027, 0x0022,
    0x8063, 0x0066, 0x006C, 0x8069, 0x0078, 0x807D, 0x8077, 0x0072,
    0x0050, 0x8055, 0x805F, 0x005A, 0x804B, 0x004E, 0x0044, 0x8041,
    0x80C3, 0x00C6, 0x00CC, 0x80C9, 0x00D8, 0x80DD, 0x80D7, 0x00D2,
    0x00F0, 0x80F5, 0x80FF, 0x00FA, 0x80EB, 0x00EE, 0x00E4, 0x80E1,
    0x00A0, 0x80A5, 0x80AF, 0x00AA, 0x80BB, 0x00BE, 0x00B4, 0x80B1,
    0x8093, 0x0096, 0x009C, 0x8099, 0x0088, 0x808D, 0x8087, 0x0082,
    0x8183, 0x0186, 0x018C, 0x8189, 0x0198, 0x819D, 0x8197, 0x0192,
    0x01B0, 0x81B5, 0x81BF, 0x01BA, 0x81AB, 0x01AE, 0x01A4, 0x81A1,
    0x01E0, 0x81E5, 0x81EF, 0x01EA, 0x81FB, 0x01FE, 0x01F4, 0x81F1,
    0x81D3, 0x01D6, 0x01DC, 0x81D9, 0x01C8, 0x81CD, 0x81C7, 0x01C2,
    0x0140, 0x8145, 0x814F, 0x014A, 0x815B, 0x015E, 0x0154, 0x8151,
    0x8173, 0x0176, 0x017C, 0x8179, 0x0168, 0x816D, 0x8167, 0x0162,
    0x8123, 0x0126, 0x012C, 0x8129, 0x0138, 0x813D, 0x8137, 0x0132,
    0x0110, 0x8115, 0x811F, 0x011A, 0x810B, 0x010E, 0x0104, 0x8101,
    0x8303, 0x0306, 0x030C, 0x8309, 0x0318, 0x831D, 0x8317, 0x0312,
    0x0330, 0x8335, 0x833F, 0x033A, 0x832B, 0x032E, 0x0324, 0x8321,
    0x0360, 0x8365, 0x836F, 0x036A, 0x837B, 0x037E, 0x0374, 0x8371,
    0x8353, 0x0356, 0x035C, 0x8359, 0x0348, 0x834D, 0x8347, 0x0342,
    0x03C0, 0x83C5, 0x83CF, 0x03CA, 0x83DB, 0x03DE, 0x03D4, 0x83D1,
    0x83F3, 0x03F6, 0x03FC, 0x83F9, 0x03E8, 0x83ED, 0x83E7, 0x03E2,
    0x83A3, 0x03A6, 0x03AC, 0x83A9, 0x03B8, 0x83BD, 0x83B7, 0x03B2,
    0x0390, 0x8395, 0x839F, 0x039A, 0x838B, 0x038E, 0x0384, 0x8381,
    0x0280, 0x8285, 0x828F, 0x028A, 0x829B, 0x029E, 0x0294, 0x8291,
    0x82B3, 0x02B6, 0x02BC, 0x82B9, 0x02A8, 0x82AD, 0x82A7, 0x02A2,
    0x82E3, 0x02E6, 0x02EC, 0x82E9, 0x02F8, 0x82FD, 0x82F7, 0x02F2,
    0x02D0, 0x82D5, 0x82DF, 0x02DA, 0x82CB, 0x02CE, 0x02C4, 0x82C1,
    0x8243, 0x0246, 0x024C, 0x8249, 0x0258, 0x825D, 0x8257, 0x0252,
    0x0270, 0x8275, 0x827F, 0x027A, 0x826B, 0x026E, 0x0264, 0x8261,
    0x0220, 0x8225, 0x822F, 0x022A, 0x823B, 0x023E, 0x0234, 0x8231,
    0x8213, 0x0216, 0x021C, 0x8219, 0x0208, 0x820D, 0x8207, 0x0202
  };

  for (j = 0; j < data_blk_size; j++)
  {
    i = ((uint16_t)(crc_accum >> 8) ^ data_blk_ptr[j]) & 0xFF;
    crc_accum = (crc_accum << 8) ^ crc_table[i];
  }

  return crc_accum;
}
