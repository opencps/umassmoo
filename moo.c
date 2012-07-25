/* See license.txt for license information. */

//******************************************************************************
//  Moo 1.1
//  Device: MSP430-2618
//
//  Version HW1.1_SW1.1
//
//  Pinout:
//             P1.1 = Data out signal
//             P1.2 = Data in signal
//             P1.3 = Data in enable
//
//             P2.0 = Supervisor In
//             P4.0 = Supervisor In
//
//
//	This is a partial implementation of the RFID EPCGlobal C1G2 protocol
//  for the Moo 1.1 hardware platform.
//
//	What's missing:
//        - SECURED and KILLED states.
//        - No support for WRITE, KILL, LOCK, ACCESS, BLOCKWRITE and BLOCKERASE
//          commands.
//        - Commands with EBVs always assume that the field is 8 bits long.
//        - SELECTS don't support truncation.
//        - READs ignore membank, wordptr, and wordcount fields. (What READs do
//          return is dependent on what application you have configured in step
//          1.)
//        - I sometimes get erroneous-looking session values in QUERYREP
//          commands. For the time being, I just parse the command as if the
//          session value is the same as my previous_session value.
//        - QUERYs use a pretty aggressive slotting algorithm to preserve power.
//          See the comments in that function for details.
//        - Session timeouts work differently than what's in table 6.15 of the
//          spec.  Here's what we do:
//            SL comes up as not asserted, and persists as long as it's in ram
//            retention mode or better.
//            All inventory flags come up as 'A'. S0's inventory flag persists
//            as long as it's in ram retention mode or better. S1's inventory
//            flag persists as long as it's in an inventory round and in ram
//            retention mode or better; otherwise it is reset to 'A' with every
//            reset at the top of the while loop. S2's and S3's inventory flag
//            is reset to 'A' with every reset at the top of the while loop.
//******************************************************************************

#if(MOO_VERSION != MOO1_1)
  #error "Moo version not supported"
#endif

#include "moo.h"
#include "rfid.h"

/*******************************************************************************
 ****************  Edit mymoo.h to configure this Moo  *************************
 ******************************************************************************/
#include "mymoo.h"

volatile unsigned char* destorig = &cmd[0]; // pointer to beginning of cmd

// #pragma data_alignment=2 is important in sendResponse() when the words are
// copied into arrays.  Sometimes the compiler puts reply[0] on an odd address,
// which cannot be copied as a word and thus screws everything up.
#pragma data_alignment=2

// compiler uses working register 4 as a global variable
// Pointer to &cmd[bits]
volatile __no_init __regvar unsigned char* dest @ 4;

// compiler uses working register 5 as a global variable
// count of bits received from reader
volatile __no_init __regvar unsigned short bits @ 5;
unsigned short TRcal=0;

int i;

int main(void)
{
  //*******************************Timer setup**********************************
  WDTCTL = WDTPW + WDTHOLD;            // Stop Watchdog Timer

  P1SEL = 0;
  P2SEL = 0;

  P1IE = 0;
  P1IFG = 0;
  P2IFG = 0;

  DRIVE_ALL_PINS // set pin directions correctly and outputs to low.

  // Check power on bootup, decide to receive or sleep.
  if(!is_power_good())
    sleep();

  RECEIVE_CLOCK;

#if DEBUG_PINS_ENABLED
#if USE_2618
  DEBUG_PIN5_LOW;
#endif
#endif

#if ENABLE_SLOTS
  // setup int epc
  epc = ackReply[2]<<8;
  epc |= ackReply[3];

  // calculate RN16_1 table
  for (Q = 0; Q < 16; Q++)
  {
    rn16 = epc^Q;
    lfsr();

    if (Q > 8)
    {
      RN16[(Q<<1)-9] = __swap_bytes(rn16);
      RN16[(Q<<1)-8] = rn16;
    }
    else
    {
      RN16[Q] = rn16;
    }
  }
#endif

  TACTL = 0;

  asm("MOV #0000h, R9");
  // dest = destorig;

#if READ_SENSOR
  init_sensor();
#endif

#if !(ENABLE_SLOTS)
  queryReplyCRC = crc16_ccitt(&queryReply[0],2);
  queryReply[3] = (unsigned char)queryReplyCRC;
  queryReply[2] = (unsigned char)__swap_bytes(queryReplyCRC);
#endif

#if SENSOR_DATA_IN_ID
  // this branch is for sensor data in the id
  ackReply[2] = SENSOR_DATA_TYPE_ID;
  state = STATE_READ_SENSOR;
  timeToSample++;
#else
  ackReplyCRC = crc16_ccitt(&ackReply[0], 14);
  ackReply[15] = (unsigned char)ackReplyCRC;
  ackReply[14] = (unsigned char)__swap_bytes(ackReplyCRC);
#endif

#if ENABLE_SESSIONS
  initialize_sessions();
#endif

  state = STATE_READY;

  setup_to_receive();

  while (1)
  {

    // TIMEOUT!  reset timer
    if (TAR > 0x256 || delimiterNotFound)   // was 0x1000
    {
      if(!is_power_good()) {
        sleep();
      }

#if SENSOR_DATA_IN_ID
    // this branch is for sensor data in the id
      if ( timeToSample++ == 10 ) {
        state = STATE_READ_SENSOR;
        timeToSample = 0;
      }
#elif SENSOR_DATA_IN_READ_COMMAND
      if ( timeToSample++ == 10 ) {
        state = STATE_READ_SENSOR;
        timeToSample = 0;
      }
#else
#if !(ENABLE_READS)
    if(!is_power_good())
        sleep();
#endif
    inInventoryRound = 0;
    state = STATE_READY;

#endif

#if ENABLE_SESSIONS
    handle_session_timeout();
#endif

#if ENABLE_SLOTS
    if (shift < 4)
        shift += 1;
    else
        shift = 0;
#endif

      setup_to_receive();
    }

    switch (state)
    {
      case STATE_READY:
      {
        inInventoryRound = 0;
        //////////////////////////////////////////////////////////////////////
        // process the QUERY command
        //////////////////////////////////////////////////////////////////////
        if ( bits == NUM_QUERY_BITS  && ( ( cmd[0] & 0xF0 ) == 0x80 ) )
        {
          handle_query(STATE_REPLY);
          setup_to_receive();
        }
        //////////////////////////////////////////////////////////////////////
        // process the SELECT command
        //////////////////////////////////////////////////////////////////////
        // @ short distance has slight impact on performance
        else if ( bits >= 44  && ( ( cmd[0] & 0xF0 ) == 0xA0 ) )
        {
          handle_select(STATE_READY);
          delimiterNotFound = 1;
        } // select command
        //////////////////////////////////////////////////////////////////////
        // got >= 22 bits, and it's not the beginning of a select. just reset.
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= MAX_NUM_QUERY_BITS && ( ( cmd[0] & 0xF0 ) != 0xA0 ) )
        {
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;
        }
        break;
      }
      case STATE_ARBITRATE:
      {
        //////////////////////////////////////////////////////////////////////
        // process the QUERY command
        //////////////////////////////////////////////////////////////////////
        if ( bits == NUM_QUERY_BITS  && ( ( cmd[0] & 0xF0 ) == 0x80 ) )
        {
          handle_query(STATE_REPLY);
          setup_to_receive();
        }
        //////////////////////////////////////////////////////////////////////
        // got >= 22 bits, and it's not the beginning of a select. just reset.
        //////////////////////////////////////////////////////////////////////
        //else if ( bits >= NUM_QUERY_BITS )
        else if ( bits >= MAX_NUM_QUERY_BITS && ( ( cmd[0] & 0xF0 ) != 0xA0 ) )
        {
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;
        }
        // this state handles query, queryrep, queryadjust, and select commands.
        //////////////////////////////////////////////////////////////////////
        // process the QUERYREP command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYREP_BITS && ( ( cmd[0] & 0x06 ) == 0x00 ) )
        {
          handle_queryrep(STATE_REPLY);
          delimiterNotFound = 1;
        } // queryrep command
        //////////////////////////////////////////////////////////////////////
        // process the QUERYADJUST command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYADJ_BITS  && ( ( cmd[0] & 0xF8 ) == 0x48 ) )
        {
          handle_queryadjust(STATE_REPLY);
          setup_to_receive();
        } // queryadjust command
        //////////////////////////////////////////////////////////////////////
        // process the SELECT command
        //////////////////////////////////////////////////////////////////////
        // @ short distance has slight impact on performance
        else if ( bits >= 44  && ( ( cmd[0] & 0xF0 ) == 0xA0 ) )
        {
          handle_select(STATE_READY);
          delimiterNotFound = 1;
        } // select command

      break;
      }

      case STATE_REPLY:
      {
        // this state handles query, query adjust, ack, and select commands
        ///////////////////////////////////////////////////////////////////////
        // process the ACK command
        ///////////////////////////////////////////////////////////////////////
        if ( bits == NUM_ACK_BITS  && ( ( cmd[0] & 0xC0 ) == 0x40 ) )
        {
#if ENABLE_READS
          handle_ack(STATE_ACKNOWLEDGED);
          setup_to_receive();
#elif SENSOR_DATA_IN_ID
          handle_ack(STATE_ACKNOWLEDGED);
          delimiterNotFound = 1; // reset
#else
          // this branch for hardcoded query/acks
          handle_ack(STATE_ACKNOWLEDGED);
          //delimiterNotFound = 1; // reset
          setup_to_receive();
#endif
        }
        //////////////////////////////////////////////////////////////////////
        // process the QUERY command
        //////////////////////////////////////////////////////////////////////
        if ( bits == NUM_QUERY_BITS  && ( ( cmd[0] & 0xF0 ) == 0x80 ) )
        {
          // i'm supposed to stay in state_reply when I get this, but if I'm
          // running close to 1.8v then I really need to reset and get in the
          // sleep, which puts me back into state_arbitrate. this is complete
          // a violation of the protocol, but it sure does make everything
          // work better. - polly 8/9/2008
          handle_query(STATE_REPLY);
          setup_to_receive();
        }
        //////////////////////////////////////////////////////////////////////
        // process the QUERYREP command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYREP_BITS && ( ( cmd[0] & 0x06 ) == 0x00 ) )
        {
			do_nothing();
			state = STATE_ARBITRATE;
			setup_to_receive();
        } // queryrep command
        //////////////////////////////////////////////////////////////////////
        // process the QUERYADJUST command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYADJ_BITS  && ( ( cmd[0] & 0xF8 ) == 0x48 ) )
        {
          handle_queryadjust(STATE_REPLY);
          delimiterNotFound = 1;
        } // queryadjust command
        //////////////////////////////////////////////////////////////////////
        // process the SELECT command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= 44  && ( ( cmd[0] & 0xF0 ) == 0xA0 ) )
        {
          handle_select(STATE_READY);
          delimiterNotFound = 1;
        } // select command
        else if ( bits >= MAX_NUM_QUERY_BITS && ( ( cmd[0] & 0xF0 ) != 0xA0 ) &&
                ( ( cmd[0] & 0xF0 ) != 0x80 ) )
        {
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;
        }
        break;
      }
      case STATE_ACKNOWLEDGED:
      {
        // responds to query, ack, request_rn cmds
        // takes action on queryrep, queryadjust, and select cmds
        /////////////////////////////////////////////////////////////////////
        // process the REQUEST_RN command
        //////////////////////////////////////////////////////////////////////
        if ( bits >= NUM_REQRN_BITS && ( cmd[0] == 0xC1 ) )
        {
#if 1
          handle_request_rn(STATE_OPEN);
          setup_to_receive();
#else
          handle_request_rn(STATE_READY);
          delimiterNotFound = 1;
#endif
        }

#if 1
        //////////////////////////////////////////////////////////////////////
        // process the QUERY command
        //////////////////////////////////////////////////////////////////////
        if ( bits == NUM_QUERY_BITS  && ( ( cmd[0] & 0xF0 ) == 0x80 ) )
        {
          handle_query(STATE_REPLY);
          delimiterNotFound = 1;
        }
        ///////////////////////////////////////////////////////////////////////
        // process the ACK command
        ///////////////////////////////////////////////////////////////////////
        // this code doesn't seem to get exercised in the real world. if i ever
        // ran into a reader that generated an ack in an acknowledged state,
        // this code might need some work.
        //else if ( bits == 20  && ( ( cmd[0] & 0xC0 ) == 0x40 ) )
        else if ( bits == NUM_ACK_BITS  && ( ( cmd[0] & 0xC0 ) == 0x40 ) )
        {
          handle_ack(STATE_ACKNOWLEDGED);
          setup_to_receive();
        }
        //////////////////////////////////////////////////////////////////////
        // process the QUERYREP command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYREP_BITS && ( ( cmd[0] & 0x06 ) == 0x00 ) )
        {
          // in the acknowledged state, rfid chips don't respond to queryrep
          // commands
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;
        } // queryrep command

        //////////////////////////////////////////////////////////////////////
        // process the QUERYADJUST command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYADJ_BITS  && ( ( cmd[0] & 0xF8 ) == 0x48 ) )
        {
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;
        } // queryadjust command
        //////////////////////////////////////////////////////////////////////
        // process the SELECT command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= 44  && ( ( cmd[0] & 0xF0 ) == 0xA0 ) )
        {
          handle_select(STATE_READY);
          delimiterNotFound = 1;
        } // select command
        //////////////////////////////////////////////////////////////////////
        // process the NAK command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= 10 && ( cmd[0] == 0xC0 ) )
        {
          do_nothing();
          state = STATE_ARBITRATE;
          delimiterNotFound = 1;
        }
        //////////////////////////////////////////////////////////////////////
        // process the READ command
        //////////////////////////////////////////////////////////////////////
        // warning: won't work for read addrs > 127d
        if ( bits == NUM_READ_BITS && ( cmd[0] == 0xC2 ) )
        {
          handle_read(STATE_ARBITRATE);
          state = STATE_ARBITRATE;
          delimiterNotFound = 1 ;
        }
        // FIXME: need write, kill, lock, blockwrite, blockerase
        //////////////////////////////////////////////////////////////////////
        // process the ACCESS command
        //////////////////////////////////////////////////////////////////////
        if ( bits >= 56  && ( cmd[0] == 0xC6 ) )
        {
          do_nothing();
          state = STATE_ARBITRATE;
          delimiterNotFound = 1 ;
        }
#endif
        else if ( bits >= MAX_NUM_READ_BITS )
        {
          state = STATE_ARBITRATE;
          delimiterNotFound = 1 ;
        }

#if 0
        // kills performance ...
        else if ( bits >= 44 )
        {
          do_nothing();
          state = STATE_ARBITRATE;
          delimiterNotFound = 1;
        }
#endif
        break;
      }
      case STATE_OPEN:
      {
        // responds to query, ack, req_rn, read, write, kill, access,
        // blockwrite, and blockerase cmds
        // processes queryrep, queryadjust, select cmds
        //////////////////////////////////////////////////////////////////////
        // process the READ command
        //////////////////////////////////////////////////////////////////////
        // warning: won't work for read addrs > 127d
        if ( bits == NUM_READ_BITS  && ( cmd[0] == 0xC2 ) )
        {
          handle_read(STATE_OPEN);
          // note: setup_to_receive() et al handled in handle_read
        }
        //////////////////////////////////////////////////////////////////////
        // process the REQUEST_RN command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= NUM_REQRN_BITS  && ( cmd[0] == 0xC1 ) )
        {
          handle_request_rn(STATE_OPEN);
          setup_to_receive();
         }
        //////////////////////////////////////////////////////////////////////
        // process the QUERY command
        //////////////////////////////////////////////////////////////////////
        if ( bits == NUM_QUERY_BITS  && ( ( cmd[0] & 0xF0 ) == 0x80 ) )
        {
          handle_query(STATE_REPLY);
          delimiterNotFound = 1;
        }
        //////////////////////////////////////////////////////////////////////
        // process the QUERYREP command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYREP_BITS && ( ( cmd[0] & 0x06 ) == 0x00 ) )
        {
          do_nothing();
          state = STATE_READY;
          setup_to_receive();
        } // queryrep command
        //////////////////////////////////////////////////////////////////////
        // process the QUERYADJUST command
        //////////////////////////////////////////////////////////////////////
          else if ( bits == 9  && ( ( cmd[0] & 0xF8 ) == 0x48 ) )
        {
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;
        } // queryadjust command
        ///////////////////////////////////////////////////////////////////////
        // process the ACK command
        ///////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_ACK_BITS  && ( ( cmd[0] & 0xC0 ) == 0x40 ) )
        {
          handle_ack(STATE_OPEN);
          delimiterNotFound = 1;
        }
        //////////////////////////////////////////////////////////////////////
        // process the SELECT command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= 44  && ( ( cmd[0] & 0xF0 ) == 0xA0 ) )
        {
          handle_select(STATE_READY);
          delimiterNotFound = 1;
        } // select command
        //////////////////////////////////////////////////////////////////////
        // process the NAK command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= 10 && ( cmd[0] == 0xC0 ) )
        {
          handle_nak(STATE_ARBITRATE);
          delimiterNotFound = 1;
        }

        break;
      }

    case STATE_READ_SENSOR:
      {
#if SENSOR_DATA_IN_READ_COMMAND
        read_sensor(&readReply[0]);
        // crc is computed in the read state
        RECEIVE_CLOCK;
        state = STATE_READY;
        delimiterNotFound = 1; // reset
#elif SENSOR_DATA_IN_ID
        read_sensor(&ackReply[3]);
        RECEIVE_CLOCK;
        ackReplyCRC = crc16_ccitt(&ackReply[0], 14);
        ackReply[15] = (unsigned char)ackReplyCRC;
        ackReply[14] = (unsigned char)__swap_bytes(ackReplyCRC);
        state = STATE_READY;
        delimiterNotFound = 1; // reset
#endif

        break;
      } // end case
    } // end switch

  } // while loop
}

//************************** SETUP TO RECEIVE  *********************************
// note: port interrupt can also reset, but it doesn't call this function
//       because function call causes PUSH instructions prior to bit read
//       at beginning of interrupt, which screws up timing.  so, remember
//       to change things in both places.
static inline void setup_to_receive()
{
  _BIC_SR(GIE); // temporarily disable GIE so we can sleep and enable interrupts
                // at the same time

  P1OUT |= RX_EN_PIN;

  delimiterNotFound = 0;
  // setup port interrupt on pin 1.2
  P1SEL &= ~BIT2;  //Disable TimerA2, so port interrupt can be used
  // Setup timer. It has to setup because there is no setup time after done with
  // port1 interrupt.
  TACTL = 0;
  TAR = 0;
  TACCR0 = 0xFFFF;    // Set up TimerA0 register as Max
  TACCTL0 = 0;
  TACCTL1 = SCS + CAP;   //Synchronize capture source and capture mode
  TACTL = TASSEL1 + MC1 + TAIE;  // SMCLK and continuous mode and Timer_A
                                 // interrupt enabled.

  // initialize bits
  bits = 0;
  // initialize dest
  dest = destorig;  // = &cmd[0]
  // clear R6 bits of word counter from prior communications to prevent dest++
  // on 1st port interrupt
  asm("CLR R6");

  P1IE = 0;
  P1IES &= ~RX_PIN; // Make positive edge for port interrupt to detect start of
                    // delimiter
  P1IFG = 0;  // Clear interrupt flag

  P1IE  |= RX_PIN; // Enable Port1 interrupt
  _BIS_SR(LPM4_bits | GIE);
  return;
}

inline void sleep()
{
  P1OUT &= ~RX_EN_PIN;
  // enable port interrupt for voltage supervisor
  P2IES = 0;
  P2IFG = 0;
  P2IE |= VOLTAGE_SV_PIN;
  P1IE = 0;
  P1IFG = 0;
  TACTL = 0;

  _BIC_SR(GIE); // temporarily disable GIE so we can sleep and enable interrupts
                // at the same time
  P2IE |= VOLTAGE_SV_PIN; // Enable Port 2 interrupt

  if (is_power_good())
    P2IFG = VOLTAGE_SV_PIN;

  _BIS_SR(LPM4_bits | GIE);

  return;
}

unsigned short is_power_good()
{
  return P2IN & VOLTAGE_SV_PIN;
}

/**
 * This code comes from the Open Tag Systems Protocol Reference Guide version
 * 1.1 dated 3/23/2004.
 * (http://www.opentagsystems.com/pdfs/downloads/OTS_Protocol_v11.pdf)
 * No licensing information accompanied the code snippet.
 **/
unsigned short crc16_ccitt(volatile unsigned char *data, unsigned short n) {
  register unsigned short i, j;
  register unsigned short crc_16;

  crc_16 = 0xFFFF; // Equivalent Preset to 0x1D0F
  for (i=0; i<n; i++) {
    crc_16^=data[i] << 8;
    for (j=0;j<8;j++) {
      if (crc_16&0x8000) {
        crc_16 <<= 1;
        crc_16 ^= 0x1021; // (CCITT) x16 + x12 + x5 + 1
      }
      else {
        crc_16 <<= 1;
      }
    }
  }
  return(crc_16^0xffff);
}

inline void crc16_ccitt_readReply(unsigned int numDataBytes)
{

  // shift everything over by 1 to accomodate leading "0" bit.
  // first, grab address of beginning of array
  readReply[numDataBytes + 2] = 0; // clear out this spot for the loner bit of
                                   // handle
  readReply[numDataBytes + 4] = 0; // clear out this spot for the loner bit of
                                   // crc
  bits = (unsigned short) &readReply[0];
  // shift all bytes and later use only data + handle
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  // store loner bit in array[numDataBytes+2] position
  asm("RRC.b @R5+");
  // make first bit 0
  readReply[0] &= 0x7f;

  // compute crc on data + handle bytes
  readReplyCRC = crc16_ccitt(&readReply[0], numDataBytes + 2);
  readReply[numDataBytes + 4] = readReply[numDataBytes + 2];
  // XOR the MSB of CRC with loner bit.
  readReply[numDataBytes + 4] ^= __swap_bytes(readReplyCRC); // XOR happens with
                                                             // MSB of lower
                                                             // nibble
  // Just take the resulting bit, not the whole byte
  readReply[numDataBytes + 4] &= 0x80;

  unsigned short mask = __swap_bytes(readReply[numDataBytes + 4]);
  mask >>= 3;
  mask |= (mask >> 7);
  mask ^= 0x1020;
  mask >>= 1;  // this is because the loner bit pushes the CRC to the left by 1
  // but we don't shift the crc because it should get pushed out by 1 anyway
  readReplyCRC ^= mask;

  readReply[numDataBytes + 3] = (unsigned char) readReplyCRC;
  readReply[numDataBytes + 2] |= (unsigned char) (__swap_bytes(readReplyCRC) &
          0x7F);
}

#if ENABLE_SLOTS

void lfsr()
{
    // calculate LFSR
    rn16 = (rn16 << 1) | (((rn16 >> 15) ^ (rn16 >> 13) ^
                (rn16 >> 9) ^ (rn16 >> 8)) & 1);
    rn16 = rn16 & 0xFFFF;

    // fit 2^Q-1
    rn16 = rn16>>(15-Q);
}

inline void loadRN16()
{
#if 1
  if (Q > 8)
  {
    queryReply[0] = RN16[(Q<<1)-9];
    queryReply[1] = RN16[(Q<<1)-8];
  }
  else
  {
    int index = ((Q+shift) & 0xF);
    queryReply[0] = RN16[index];
    queryReply[1] = RN16[index+1];
  }
#else
    queryReply[0] = 0xf0;
    queryReply[1] = 0x0f;
#endif

}

inline void mixupRN16()
{
  unsigned short tmp;
  unsigned short newQ = 0;
  unsigned short swapee_index = 0;

  newQ = RN16[shift] & 0xF;
  swapee_index = RN16[newQ];
  tmp = RN16[shift];
  RN16[Q] = RN16[swapee_index];
  RN16[swapee_index] = tmp;
}


#endif


//*************************************************************************
//************************ PORT 2 INTERRUPT *******************************

// Pin Setup :
// Description : Port 2 interrupt wakes on power good signal from supervisor.

#pragma vector=PORT2_VECTOR
__interrupt void Port2_ISR(void)   // (5-6 cycles) to enter interrupt
{
  P2IFG = 0x00;
  P2IE = 0;       // Interrupt disable
  P1IFG = 0;
  P1IE = 0;
  TACTL = 0;
  TACCTL0 = 0;
  TACCTL1 = 0;
  TAR = 0;
  state = STATE_READY;
  LPM4_EXIT;
}

#if USE_2618
#pragma vector=TIMERA0_VECTOR
#else
#pragma vector=TIMERA0_VECTOR
#endif
__interrupt void TimerA0_ISR(void)   // (5-6 cycles) to enter interrupt
{
  TACTL = 0;    // have to manually clear interrupt flag
  TACCTL0 = 0;  // have to manually clear interrupt flag
  TACCTL1 = 0;  // have to manually clear interrupt flag
  LPM4_EXIT;
}
/*
//*************************************************************************
//************************ PORT 1 INTERRUPT *******************************

// warning   :  Whenever the clock frequency changes, the value of TAR should be
//              changed in aesterick lines
// Pin Setup :  P1.2
// Description : Port 1 interrupt is used as finding delimeter.

#pragma vector=PORT1_VECTOR
__interrupt void Port1_ISR(void)   // (5-6 cycles) to enter interrupt
{


#if USE_2618
  asm("MOV TAR, R7");  // move TAR to R7(count) register (3 CYCLES)
#else
  asm("MOV TAR, R7");  // move TAR to R7(count) register (3 CYCLES)
#endif
  P1IFG = 0x00;       // 4 cycles
  TAR = 0;            // 4 cycles
  LPM4_EXIT;

  asm("CMP #0000h, R5\n");          // if (bits == 0) (1 cycle)
  asm("JEQ bit_Is_Zero_In_Port_Int\n");                // 2 cycles
  // bits != 0:
  asm("MOV #0000h, R5\n");          // bits = 0  (1 cycles)

  asm("CMP #0010h, R7\n");          // finding delimeter (12.5us, 2 cycles)
                                    // 2d -> 14
  asm("JNC delimiter_Value_Is_wrong\n");            //(2 cycles)
  asm("CMP #0040h, R7");            // finding delimeter (12.5us, 2 cycles)
                                    // 43H
  asm("JC  delimiter_Value_Is_wrong\n");
  asm("CLR P1IE");
#if USE_2618
  asm("BIS #8010h, TACCTL1\n");     // (5 cycles)   TACCTL1 |= CM1 + CCIE
#else
  asm("BIS #8010h, TACCTL1\n");     // (5 cycles)   TACCTL1 |= CM1 + CCIE
#endif
  asm("MOV #0004h, P1SEL\n");       // enable TimerA1    (4 cycles)
  asm("RETI\n");

  asm("delimiter_Value_Is_wrong:\n");
  asm("BIC #0004h, P1IES\n");
  asm("MOV #0000h, R5\n");          // bits = 0  (1 cycles)
  delimiterNotFound = 1;
  asm("RETI");

  asm("bit_Is_Zero_In_Port_Int:\n");                 // bits == 0
#if USE_2618
  asm("MOV #0000h, TAR\n");     // reset timer (4 cycles)
#else
  asm("MOV #0000h, TAR\n");     // reset timer (4 cycles)
#endif
  asm("BIS #0004h, P1IES\n");   // 4 cycles  change port interrupt edge to neg
  asm("INC R5\n");            // 1 cycle
  asm("RETI\n");

}*/
//*************************************************************************
//************************ Timer INTERRUPT *******************************

// Pin Setup :  P1.2
// Description :

#if USE_2618
#pragma vector=TIMERA1_VECTOR
#else
#pragma vector=TIMERA1_VECTOR
#endif
__interrupt void TimerA1_ISR(void)   // (6 cycles) to enter interrupt
{

    asm("MOV 0174h, R7");  // move TACCR1 to R7(count) register (3 CYCLES)
    TAR = 0;               // reset timer (4 cycles)
    TACCTL1 &= ~CCIFG;      // must manually clear interrupt flag (4 cycles)

    //<------up to here 26 cycles + 6 cyles of Interrupt == 32 cycles -------->
    asm("CMP #0003h, R5\n");      // if (bits >= 3).  it will do store bits
    asm("JGE bit_Is_Over_Three\n");
    // bit is not 3
    asm("CMP #0002h, R5\n");   // if ( bits == 2)
    asm("JEQ bit_Is_Two\n");         // if (bits == 2).

    // <----------------- bit is not 2 ------------------------------->
    asm("CMP #0001h, R5\n");      // if (bits == 1) -- measure RTcal value.
    asm("JEQ bit_Is_One\n");          // bits == 1

    // <-------------------- this is bit == 0 case --------------------->
    asm("bit_Is_Zero_In_Timer_Int:");
    asm("CLR R6\n");
    asm("INC R5\n");        // bits++
    asm("RETI");
    // <------------------- end of bit 0  --------------------------->

    // <-------------------- this is bit == 1 case --------------------->
    asm("bit_Is_One:\n");         // bits == 1.  calculate RTcal value
    asm("MOV R7, R9\n");       // 1 cycle
    asm("RRA R7\n");    // R7(count) is divided by 2.   1 cycle
    asm("MOV #0FFFFh, R8\n");   // R8(pivot) is set to max value    1 cycle
    asm("SUB R7, R8\n");        // R8(pivot) = R8(pivot) -R7(count/2) make new
                                // R8(pivot) value     1 cycle
    asm("INC R5\n");        // bits++
    asm("CLR R6\n");
    asm("RETI\n");
    // <------------------ end of bit 1 ------------------------------>

    // <-------------------- this is bit == 2 case --------------------->
    asm("bit_Is_Two:\n");
    asm("CMP R9, R7\n");    // if (count > (R9)(180)) this is hardcoded number,
                            // so have  to change to proper value
    asm("JGE this_Is_TRcal\n");
    // this is data
    asm("this_Is_Data_Bit:\n");
    asm("ADD R8, R7\n");   // count = count + pivot
    // store bit by shifting carry flag into cmd[bits]=(dest*) and increment
    // dest*  (5 cycles)
    asm("ADDC.b @R4+,-1(R4)\n"); // roll left (emulated by adding to itself ==
                                 // multiply by 2 + carry)
    // R6 lets us know when we have 8 bits, at which point we INC dest* (1
    // cycle)
    asm("INC R6\n");
    asm("CMP #0008,R6\n\n");   // undo increment of dest* (R4) until we have 8
                               // bits
    asm("JGE out_p\n");
    asm("DEC R4\n");
    asm("out_p:\n");           // decrement R4 if we haven't gotten 16 bits yet
                               // (3 or 4 cycles)
    asm("BIC #0008h,R6\n");   // when R6=8, this will set R6=0   (1 cycle)
    asm("INC R5\n");
    asm("RETI");
    // <------------------ end of bit 2 ------------------------------>

    asm("this_Is_TRcal:\n");
    asm("MOV R7, R5\n");    // bits = count. use bits(R5) to assign new value of
                            // TRcal
    TRcal = bits;       // assign new value     (4 cycles)
    asm("MOV #0003h, R5\n");      // bits = 3..assign 3 to bits, so it will keep
                                  // track of current bits    (2 cycles)
    asm("CLR R6\n"); // (1 cycle)
    asm("RETI");

   // <------------- this is bits >= 3 case ----------------------->
    asm("bit_Is_Over_Three:\n");     // bits >= 3 , so store bits
    asm("ADD R8, R7\n");    // R7(count) = R8(pivot) + R7(count),
    // store bit by shifting carry flag into cmd[bits]=(dest*) and increment
    // dest* (5 cycles)
    asm("ADDC.b @R4+,-1(R4)\n"); // roll left (emulated by adding to itself ==
                                 // multiply by 2 + carry)
    // R6 lets us know when we have 8 bits, at which point we INC dest* (1
    // cycle)
    asm("INC R6\n");
    asm("CMP #0008,R6\n");   // undo increment of dest* (R4) until we have 8
                             // bits
    asm("JGE out_p1\n");
    asm("DEC R4\n");
    asm("out_p1:\n");           // decrement R4 if we haven't gotten 16 bits yet
                                // (3 or 4 cycles)
    asm("BIC #0008h,R6\n");   // when R6=8, this will set R6=0   (1 cycle)
    asm("INC R5\n");              // bits++
    asm("RETI\n");
    // <------------------ end of bit is over 3 ------------------------------>
}

