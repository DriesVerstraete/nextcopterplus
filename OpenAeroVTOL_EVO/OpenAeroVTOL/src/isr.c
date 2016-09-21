//***********************************************************
//* isr.c
//***********************************************************

//***********************************************************
//* Includes
//***********************************************************

#include <stdbool.h>
#include <avr/interrupt.h>
#include "io_cfg.h"
#include "main.h"
#include <stdlib.h>
#include <string.h>
#include <uart.h>

//***********************************************************
//* Prototypes
//***********************************************************

uint16_t TIM16_ReadTCNT1(void);
void init_int(void);
void Disable_RC_Interrupts(void);

//************************************************************
// Interrupt vectors
//************************************************************

volatile bool Interrupted;			// Flag that RX packet completed
volatile bool JitterFlag;			// Flag that interrupt occurred
volatile bool JitterGate;			// Area when we care about JitterFlag

volatile uint16_t RxChannel[MAX_RC_CHANNELS];
volatile uint16_t RxChannelStart[MAX_RC_CHANNELS];	
volatile uint16_t TempRxChannel[MAX_RC_CHANNELS]; // Temp regs for UDI. Look to remove this in the future.
volatile uint16_t PPMSyncStart;		// Sync pulse timer
volatile uint8_t ch_num;			// Current channel number
volatile uint8_t max_chan;			// Target channel number

volatile uint8_t rcindex;			// Serial data buffer pointer
volatile uint16_t chanmask16;
volatile uint16_t checksum;
volatile uint8_t bytecount;
volatile uint16_t TMR0_counter;		// Number of times Timer 0 has overflowed
volatile uint16_t FrameRate;		// Updated frame rate for serial packets
volatile uint8_t packet_size;

#define SYNCPULSEWIDTH 6750			// CPPM sync pulse must be more than 2.7ms
#define MINPULSEWIDTH 750			// Minimum CPPM pulse is 300us
#define PACKET_TIMER 2500			// Serial RC packet start timer. Minimum gap 500/2500000 = 1.0ms
#define MAX_CPPM_CHANNELS 8			// Maximum number of channels via CPPM

#define MODEB_SYNCBYTE 0xA1			// MODEB/UDI sync byte
#define MAXSUMDPACKET 69			// Maximum possible HoTT SUMD packet size
#define SUMD_SYNCBYTE 0xA8			// SUMD sync byte
#define XBUS_FRAME_SIZE_12 27		// Packet size for a 12-channel packet
#define XBUS_FRAME_SIZE_16 35		// Packet size for a 16-channel packet	
#define XBUS_CRC_BYTE_1 25
#define XBUS_CRC_BYTE_2 26
#define XBUS_CRC_AND_VALUE 0x8000
#define XBUS_CRC_POLY 0x1021
			
//************************************************************
//* Timer 0 overflow handler for extending TMR1
//************************************************************

ISR(TIMER0_OVF_vect)
{
	TMR0_counter++;
}

//************************************************************
//* Standard PWM mode
//* Sequential PWM inputs from a normal RC receiver
//************************************************************

ISR(INT1_vect)
{
	// Log interrupts that occur during PWM generation
	if (JitterGate)	JitterFlag = true;	

	if (RX_ROLL)	// Rising
	{
		RxChannelStart[AILERON] = TCNT1;
	} 
	else 
	{				// Falling
		RxChannel[AILERON] = TCNT1 - RxChannelStart[AILERON];
		if (Config.PWM_Sync == AILERON) 
		{
			Interrupted = true;						// Signal that interrupt block has finished
			Servo_TCNT2 = TCNT2;					// Reset signal loss timer and Overdue state 
			RC_Timeout = 0;
			Overdue = false;
		}
	}
}

ISR(INT0_vect)
{
	if (JitterGate)	JitterFlag = true;	

	if (RX_PITCH)	// Rising 
	{
		RxChannelStart[ELEVATOR] = TCNT1;
	} 
	else 
	{				// Falling
		RxChannel[ELEVATOR] = TCNT1 - RxChannelStart[ELEVATOR];
		if (Config.PWM_Sync == ELEVATOR) 
		{
			Interrupted = true;						// Signal that interrupt block has finished
			Servo_TCNT2 = TCNT2;					// Reset signal loss timer and Overdue state 
			RC_Timeout = 0;
			Overdue = false;
		}
	}
}

ISR(PCINT3_vect)
{
	if (JitterGate)	JitterFlag = true;	
		
	if (RX_COLL)	// Rising
	{
		RxChannelStart[THROTTLE] = TCNT1;
	} 
	else 
	{				// Falling
		RxChannel[THROTTLE] = TCNT1 - RxChannelStart[THROTTLE];
		if (Config.PWM_Sync == THROTTLE) 
		{
			Interrupted = true;						// Signal that interrupt block has finished
			Servo_TCNT2 = TCNT2;					// Reset signal loss timer and Overdue state 
			RC_Timeout = 0;
			Overdue = false;
		}
	}
}


ISR(PCINT1_vect)
{
	if (JitterGate)	JitterFlag = true;

	if (RX_AUX)	// Rising
	{
		RxChannelStart[GEAR] = TCNT1;
	} 
	else 
	{				// Falling
		RxChannel[GEAR] = TCNT1 - RxChannelStart[GEAR];
		if (Config.PWM_Sync == GEAR) 
		{
			Interrupted = true;						// Signal that interrupt block has finished
			Servo_TCNT2 = TCNT2;					// Reset signal loss timer and Overdue state 
			RC_Timeout = 0;
			Overdue = false;
		}
	}
}

//************************************************************
// INT2 is shared between RUDDER in PWM mode or CPPM in CPPM mode
// NB: Raw CPPM channel order (0,1,2,3,4,5,6,7) is 
// mapped via Config.ChannelOrder[]. Actual channel values are always
// in the sequence THROTTLE, AILERON, ELEVATOR, RUDDER, GEAR, AUX1, AUX2, AUX3
//
// Compacted CPPM RX code thanks to Edgar
//
//************************************************************

ISR(INT2_vect)
{
	if (JitterGate)	JitterFlag = true;	

    // Backup TCNT1
    uint16_t tCount;
	
    tCount = TIM16_ReadTCNT1();

	uint8_t curChannel;
	uint8_t prevChannel;

	if (Config.RxMode != CPPM_MODE)
	{
		if (RX_YAW)	// Rising
		{
			RxChannelStart[RUDDER] = tCount;
		} 
		else 
		{			// Falling
			RxChannel[RUDDER] = tCount - RxChannelStart[RUDDER];
			if (Config.PWM_Sync == RUDDER) 
			{
				Interrupted = true;					// Signal that interrupt block has finished
				Servo_TCNT2 = TCNT2;				// Reset signal loss timer and Overdue state 
				RC_Timeout = 0;
				Overdue = false;
			}
		}
	}
	
	//************************************************************
	// CPPM code:
	// This code keeps track of the number of channels received
	// within a frame and only signals the data received when the 
	// last data is complete. This makes it compatible with any 
	// number of channels. The minimum sync pulse is 2.7ms and the
	// minimum inter-channel pulse is 300us. This suits "27ms" FrSky
	// CPPM receivers.
	//************************************************************
	else
	{
		// Only respond to negative-going interrupts
		if (CPPM) return;

		// Check to see if previous period was a sync pulse or too small to be valid
		// If so, reset the channel number
		if (((tCount - PPMSyncStart) > SYNCPULSEWIDTH) || ((tCount - PPMSyncStart) < MINPULSEWIDTH))
		{
			ch_num = 0;
		}

		// Update PPMSyncStart with current value
		PPMSyncStart = tCount;

		// Get the channel number of the current channel in the requested channel order
        curChannel = Config.ChannelOrder[ch_num];

		// Set up previous channel number based on the requested channel order
		if (ch_num > 0)
		{
			prevChannel = Config.ChannelOrder[ch_num-1];
		}
		else
		{
			prevChannel = 0;
		}

		// Measure the channel data only for the first MAX_CPPM_CHANNELS (currently 8)
		// Prevent code from over-running RxChannelStart[]
        if (ch_num < MAX_CPPM_CHANNELS)
		{
            RxChannelStart[curChannel] = tCount;
		}

		// When ch_num = 0, the first channel has not yet been measured.
		// That only occurs at the second pulse. Prevent code from over-running RxChannel[]
        if ((ch_num > 0) && (ch_num <= MAX_CPPM_CHANNELS))
        {
		   RxChannel[prevChannel] = tCount - RxChannelStart[prevChannel];
		}

        // Increment to the next channel
		ch_num++;

		// Work out the highest channel number automatically.
		// Update the maximum channel seen so far.
		if (ch_num > max_chan) 
		{
			max_chan = ch_num;					// Update max channel number
		}
		// If the current channel is the highest channel, CPPM is complete
		else if (ch_num == max_chan)
		{
			Interrupted = true;					// Signal that interrupt block has finished
			Servo_TCNT2 = TCNT2;				// Reset signal loss timer and Overdue state 
			RC_Timeout = 0;
			Overdue = false;
		}
	
		// If the signal is ever lost, reset measured max channel number
		// and force a recalculation
		if (Overdue)
		{
			max_chan = 0;
			Overdue = false;
		}
	}
} // ISR(INT2_vect)

//************************************************************
//* Serial receive interrupt
//************************************************************

ISR(USART0_RX_vect)
{
	char temp = 0;			// RX characters
	uint16_t temp16 = 0;	// Unsigned temp reg for mask etc
	int16_t itemp16 = 0;	// Signed temp reg 
	uint8_t sindex = 0;		// Serial buffer index
	uint8_t j = 0;			// GP counter and mask index

	uint8_t chan_mask = 0;	// Common variables
	uint8_t chan_shift = 0;
	uint8_t data_mask = 0;
	uint16_t crc = 0;
	uint16_t checkcrc = 0;
	
	uint16_t Save_TCNT1;	// Timer1 (16bit) - run @ 2.5MHz (400ns) - max 26.2ms
	uint16_t CurrentPeriod;	
	
	//************************************************************
	//* Common entry code
	//************************************************************

	// Log interrupts that occur during PWM generation
	if (JitterGate)	JitterFlag = true;

	// Read error flags first
	temp =  UCSR0A;

	// Check Framing error, Parity error bits
	if (temp & ((1<<FE0)|(1<<UPE0)))
	{
		// Read byte to remove from buffer
		temp = UDR0;
	}

	// Check all for Data overrun
	else if (temp & (1<<DOR0))
	{
		// Read byte to remove from buffer
		temp = UDR0;
		// Read byte to remove from buffer
		temp = UDR0;
	}

	// Valid data
	else
	{
		// Read byte first
		temp = UDR0;

		// Save current time stamp
		Save_TCNT1 = TIM16_ReadTCNT1();
	
		// Work out frame rate properly
		// Note that CurrentPeriod cannot be larger than 26.2ms
	
		//CurrentPeriod = Save_TCNT1 - PPMSyncStart;
		if (Save_TCNT1 < PPMSyncStart)
		{
			CurrentPeriod = (65536 - PPMSyncStart + Save_TCNT1);
		}
		else
		{
			CurrentPeriod = (Save_TCNT1 - PPMSyncStart);
		}

		// Handle start of new packet
		if (CurrentPeriod > PACKET_TIMER) // 1.0ms
		{
			// Reset variables
			rcindex = 0;
			bytecount = 0;
			ch_num = 0;
			checksum = 0;
			chanmask16 = 0;

			// Save frame rate to global
			FrameRate = CurrentPeriod;
			
			// Clear buffer
			memset(&sBuffer[0],0,SBUFFER_SIZE);
		}

		// Timestamp this interrupt
		PPMSyncStart = Save_TCNT1;
	
		// Put received byte in buffer if space available
		if (rcindex < SBUFFER_SIZE)
		{
			sBuffer[rcindex++] = temp;			
		}

		//************************************************************
		//* XPS Xtreme format (8-N-1/250Kbps) (1480us for a 37 bytes packet)
		//*
		//* Byte 0: Bit 3 should always be 0 unless there really is a lost packet.
		//* Byte 1: RSS
		//* Byte 2: Mask 
 		//* 		The mask value determines the number of channels in the stream. 
		//*			A 6 channel stream is going to have a mask of 0x003F (00000000 00111111) 
		//*			if outputting all 6 channels.  It is possible to output only channels 2 
		//*			and 4 in the stream (00000000 00001010).  In which case the first word 
		//*			of data will be channel 2 and the 2nd word will be channel.
 		//*  
		//*  0x00   0x23   0x000A   0x5DC   0x5DD   0xF0
		//*  ^^^^   ^^^^   ^^^^^^   ^^^^^   ^^^^^   ^^^^
		//*  Flags  dBm     Mask    CH 2    CH 4    ChkSum
		//*
		//************************************************************

		if (Config.RxMode == XTREME)
		{
			// Look at flag byte to see if the data is meant for us
			if (bytecount == 0)
			{
				// Check top 3 bits for channel bank
				// Trash checksum if not clear
				if (temp & 0xE0)
				{
					checksum +=	0x55;
				}
			}

			// Get MSB of mask byte
			if (bytecount == 2)
			{
				chanmask16 = 0;
				chanmask16 = temp << 8;		// High byte of Mask
			}

			// Combine with LSB of mask byte
			// Work out how many channels there are supposed to be
			if (bytecount == 3)
			{
				chanmask16 += (uint16_t)temp;	// Low byte of Mask
				temp16 = chanmask16;			// Need to keep a copy od chanmask16

				// Count bits set (number of active channels)				 
				for (ch_num = 0; temp16; ch_num++)
				{
					temp16 &= temp16 - 1;
				}
			}

			// Add up checksum up until final packet
			if (bytecount < ((ch_num << 1) + 4))
			{
				checksum +=	temp;
			}
	
			// Process data when all packets received
			else
			{
				// Check checksum 
				checksum &= 0xff;

				// Ignore packet if checksum wrong
				if (checksum != temp) // temp holds the transmitted checksum byte
				{
					Interrupted = false;
					ch_num = 0;
					checksum = 0;
				}
				else
				{
					// RC sync established
					Interrupted = true;	

					// Reset signal loss timer and Overdue state 
					Servo_TCNT2 = TCNT2;
					RC_Timeout = 0;
					Overdue = false;
			
					// Set start of channel data per format
					sindex = 4; // Channel data from byte 5

					// Work out which channel the data is intended for from the mask bit position
					// Channels can be anywhere in the lower 16 channels of the Xtreme format
					for (j = 0; j < 16; j++)
					{
						// If there is a bit set, allocate channel data for it
						if (chanmask16 & (1 << j))
						{
							// Reconstruct word
							temp16 = (sBuffer[sindex] << 8) + sBuffer[sindex + 1];

							// Expand to OpenAero2 units if a valid channel
							if (j < MAX_RC_CHANNELS)
							{
								RxChannel[Config.ChannelOrder[j]] = ((temp16 * 10) >> 2);
							} 		

							// Within the bounds of the buffer
							if (sindex < SBUFFER_SIZE)
							{
								sindex += 2;
							}
						}
					} // For each mask bit	
				} // Checksum
			} // Check end of data
		} // (Config.RxMode == XTREME)

		//************************************************************
		//* Futaba S-Bus format (8-E-2/100Kbps) (2500us for a 25 byte packet)
		//*	S-Bus decoding algorithm borrowed in part from Arduino
		//*
		//* The protocol is 25 Bytes long and is sent every 14ms (analog mode) or 7ms (high speed mode).
		//* One Byte = 1 start bit + 8 data bit + 1 parity bit + 2 stop bit (8E2), baud rate = 100,000 bit/s
		//*
		//* The highest bit is sent first. The logic is inverted :( Stupid Futaba.
		//*
		//* [start byte] [data1] [data2] .... [data22] [flags][end byte]
		//* 
		//* 0 start byte = 11110000b (0xF0)
		//* 1-22 data = [ch1, 11bit][ch2, 11bit] .... [ch16, 11bit] (Values = 0 to 2047)
		//* 	channel 1 uses 8 bits from data1 and 3 bits from data2
		//* 	channel 2 uses last 5 bits from data2 and 6 bits from data3
		//* 	etc.
		//* 
		//* 23 flags = 
		//*		bit7 = ch17 = digital channel (0x80)
		//* 	bit6 = ch18 = digital channel (0x40)
		//* 	bit5 = Frame lost, equivalent red LED on receiver (0x20)
		//* 	bit4 = failsafe activated (0x10)
		//* 	bit3 = n/a
		//* 	bit2 = n/a
		//* 	bit1 = n/a
		//* 	bit0 = n/a
		//* 24 endbyte = 00000000b (SBUS) or (variable) (SBUS2)
		//*
		//* Data size:	0 to 2047, centered on 1024 (1.520ms)
		//* 
		//* 0 		= 880us
		//* 224		= 1020us
		//* 1024 	= 1520us +/-800 for 1-2ms (OAV = +/-1250)
		//* 1824	= 2020us
		//* 2047 	= 2160us
		//*
		//************************************************************

		if (Config.RxMode == SBUS)
		{
			// Flag that packet has completed
			// End bytes can be 00, 04, 14, 24, 34 and possibly 08 for FASSTest 12-channel
			//if ((bytecount == 24) && ((temp == 0x00) || (temp == 0x04) || (temp == 0x14) || (temp == 0x24) || (temp == 0x34) || (temp == 0x08)))
			if (bytecount == 24)
			{
				// RC sync established
				Interrupted = true;
				Servo_TCNT2 = TCNT2;
				RC_Timeout = 0;
				Overdue = false;
				
				// Clear channel data
				for (j = 0; j < MAX_RC_CHANNELS; j++)
				{
					RxChannel[j] = 0;
				}

				// Start from second byte
				sindex = 1;

				// Deconstruct S-Bus data
				// 8 channels * 11 bits = 88 bits
				for (j = 0; j < 88; j++)
				{
					if (sBuffer[sindex] & (1 << chan_mask))
					{
						// Place the RC data into the correct channel order for the transmitted system
						RxChannel[Config.ChannelOrder[chan_shift]] |= (1 << data_mask);
					}

					chan_mask++;
					data_mask++;

					// If we have done 8 bits, move to next byte in buffer
					if (chan_mask == 8)
					{
						chan_mask = 0;
						sindex++;
					}

					// If we have reconstructed all 11 bits of one channel's data (2047)
					// increment the channel number
					if (data_mask == 11)
					{
						data_mask =0;
						chan_shift++;
					}
				}

				// Convert to  OpenAero2 values
				for (j = 0; j < MAX_RC_CHANNELS; j++)
				{
					// Subtract Futaba offset
					itemp16 = RxChannel[j] - 1024;
						
					// Expand into OpenAero2 units x1.562 (1.562) (1250/800)
					itemp16 = itemp16 + (itemp16 >> 1) + (itemp16 >> 4);

					// Add back in OpenAero2 offset
					RxChannel[j] = itemp16 + 3750;		
				} 	
			
			} // Packet ended flag
	
		} // (Config.RxMode == SBUS)

		//************************************************************
		//* Spektrum Satellite format (8-N-1/115Kbps) MSB sent first (1391us for a 16 byte packet)
		//* DX7/DX6i: One data-frame at 115200 baud every 22ms.
		//* DX7se:    One data-frame at 115200 baud every 11ms.
		//*
		//*    byte1: is a frame loss counter
		//*    byte2: [0 0 0 R 0 0 N1 N0]
		//*    byte3:  and byte4:  channel data (FLT-Mode)	= FLAP 6
		//*    byte5:  and byte6:  channel data (Roll)		= AILE A
		//*    byte7:  and byte8:  channel data (Pitch)		= ELEV E
		//*    byte9:  and byte10: channel data (Yaw)		= RUDD R
		//*    byte11: and byte12: channel data (Gear Switch) GEAR 5
		//*    byte13: and byte14: channel data (Throttle)	= THRO T
		//*    byte15: and byte16: channel data (AUX2)		= AUX2 8
		//* 
		//* DS9 (9 Channel): One data-frame at 115200 baud every 11ms,
		//* alternating frame 1/2 for CH1-7 / CH8-9
		//*
		//*   1st Frame:
		//*    byte1: is a frame loss counter
		//*    byte2: [0 0 0 R 0 0 N1 N0]
		//*    byte3:  and byte4:  channel data
		//*    byte5:  and byte6:  channel data
		//*    byte7:  and byte8:  channel data
		//*    byte9:  and byte10: channel data
		//*    byte11: and byte12: channel data
		//*    byte13: and byte14: channel data
		//*    byte15: and byte16: channel data
		//*   2nd Frame:
		//*    byte1: is a frame loss counter
		//*    byte2: [0 0 0 R 0 0 N1 N0]
		//*    byte3:  and byte4:  channel data
		//*    byte5:  and byte6:  channel data
		//*    byte7:  and byte8:  0xffff
		//*    byte9:  and byte10: 0xffff
		//*    byte11: and byte12: 0xffff
		//*    byte13: and byte14: 0xffff
		//*    byte15: and byte16: 0xffff
		//* 
		//* Each channel data (16 bit= 2byte, first msb, second lsb) is arranged as:
		//* 
		//* Bits: F 00 C3 C2 C1 C0  D9 D8 D7 D6 D5 D4 D3 D2 D1 D0 for 10-bit data (0 to 1023) or
		//* Bits: F C3 C2 C1 C0 D10 D9 D8 D7 D6 D5 D4 D3 D2 D1 D0 for 11-bit data (0 to 2047) 
		//* 
		//* R: 0 for 10 bit resolution 1 for 11 bit resolution channel data
		//* N1 to N0 is the number of frames required to receive all channel data. 
		//* F: 1 = indicates beginning of 2nd frame for CH8-9 (DS9 only)
		//* C3 to C0 is the channel number. 0 to 9 (4 bit, as assigned in the transmitter)
		//* D9 to D0 is the channel data 
		//*		(10 bit) 0xaa..0x200..0x356 for 100% transmitter-travel
		//*		(11 bit) 0x154..0x400..0x6ac for 100% transmitter-travel
		//*
		//* The data values can range from 0 to 1023/2047 to define a servo pulse width 
		//* 
		//* 0 		= 920us
		//* 157		= 1010us
		//* 1024 	= 1510us +/- 867.5 for 1-2ms or 0.576ns/bit
		//* 1892	= 2010us
		//* 2047 	= 2100us
		//*
		//************************************************************

		// Handle Spektrum format
		if (Config.RxMode == SPEKTRUM)
		{
			// Process data when all packets received
			if (bytecount == 15)
			{
				// Just stick the last byte into the buffer manually...(hides)
				sBuffer[15] = temp;

				// Set start of channel data per format
				sindex = 2; // Channel data from byte 3

				// Work out if this is 10 or 11 bit data
				if ((sBuffer[1] & 0xF0) != 0) 	// 0 for 10 bit resolution, otherwise 11 bit resolution
				{
					chan_mask = 0x78;	// 11 bit (2048)
					data_mask = 0x07;
					chan_shift = 0x03;
				}
				else
				{
					chan_mask = 0x3C;	// 10 bit (1024)
					data_mask = 0x03;
					chan_shift = 0x02;
				}

				// Work out which channel the data is intended for from the channel number data
				// Channels can also be in the second packet. Spektrum has 7 channels per packet.
				for (j = 0; j < 7; j++)
				{
					// Extract channel number
					ch_num = (sBuffer[sindex] & chan_mask) >> chan_shift;

					// Reconstruct channel data
					temp16 = ((sBuffer[sindex] & data_mask) << 8) + sBuffer[sindex + 1];

					// Expand to OpenAero2 units if a valid channel
					// Blank channels have the channel number of 16
					if (ch_num < MAX_RC_CHANNELS)
					{
						// Subtract Spektrum center offset
						if (chan_shift == 0x03) // 11-bit
						{
							itemp16 = temp16 - 1024;
						}
						else
						{
							itemp16 = temp16 - 512;	
						}					

						// Spektrum to System
						// Expand into OpenAero2 units (1250/867.5) x2 = 2.8818 (2.875) 2+.5+.25-1/8
						itemp16 = (itemp16 << 1) + (itemp16 >> 1) + (itemp16 >> 2) + (itemp16 >> 3);

						if (chan_shift == 0x03) // 11-bit
						{
							// Divide in case of 11-bit value
							itemp16 = itemp16 >> 1;								
						}

						// Add back in OpenAero2 offset
						itemp16 += 3750;										

						RxChannel[Config.ChannelOrder[ch_num]] = itemp16;
					}

					sindex += 2;

				} // For each pair of bytes
			
				// RC sync established
				Interrupted = true;
				
				// Reset signal loss timer and Overdue state 
				Servo_TCNT2 = TCNT2;
				RC_Timeout = 0;
				Overdue = false;
			
			} // Check end of data
		
		} // (Config.RxMode == SPEKTRUM)

		//************************************************************
		//* XBUS Mode B/UDI RX Data format 115200Kbit/s, 8 data bit, no parity, and one stop bit. 
		//* Portions of code adopted from MultiWii and from GruffyPuffy/cleanflight.
		//* 
		//* First byte = vendor ID		0xA1 = 12-Ch Data
		//*								0xA2 = 16-Ch Data
		//*
		//* Next 24/32 bytes = 12/16 channels of 16-bit servo data, high-byte first
		//* Last 2 bytes = CRC value over first 25/33 bytes, using CRC-CCITT algorithm.
		//*
		//* Pulse length conversion from [0...4095] to �s:
		//*      800�s  -> 0x000
		//*      1500�s -> 0x800 (2048)
		//*      2200�s -> 0xFFF
		//*
		//* Total range is: 2200 - 800 = 1400 <==> 4096
		//* Use formula: 800 + value * 1400 / 4096 (i.e. a shift by 12)
		//*
		//* The data values can range from 0 to 4095 to define a servo pulse width.
		//* Each bit in servo data corresponds to pulse width change of 0.342�s. 
		//* 
		//* 0 		= 800us
		//* 585		= 1000us
		//* 2048 	= 1500us +/- 1463 for 1-2ms
		//* 3511	= 2000us
		//* 4095 	= 2200us		 
		//*
		//************************************************************
		
		// Handle SXRL format
		if (Config.RxMode == MODEB)
		{
			// Work out the expected number of bytes based on the vendor ID (1st byte)
			if (bytecount == 0)
			{
				// Process data when all packets received
				if (sBuffer[0] == MODEB_SYNCBYTE)		// 12-channel packet
				{
					packet_size = XBUS_FRAME_SIZE_12;
				}
				else									// Probably a 16-channel packet
				{
					packet_size = XBUS_FRAME_SIZE_16;
				}
			}

			// Check checksum when all data received
			if (bytecount == (packet_size - 1))
			{
				crc = 0;
			
				// Add up checksum for all bytes up to but not including the checksum
				for (j = 0; j < (packet_size - 2); j++)
				{
					crc = CRC16(crc, sBuffer[j]);
				}
			
				// Extract the packet's own checksum
				checkcrc = ((uint16_t)(sBuffer[packet_size - 2] << 8) | (uint16_t)(sBuffer[packet_size - 1]));
				
				// Compare with the calculated one and process data if ok
				if (checkcrc == crc)
				{
					// RC sync established
					Interrupted = true;
					
					// Reset signal loss timer and Overdue state 					
					Servo_TCNT2 = TCNT2;
					RC_Timeout = 0;
					Overdue = false;
			
					// Copy unconverted channel data
					for (j = 0; j < MAX_RC_CHANNELS; j++)
					{
						// Combine bytes from buffer
						TempRxChannel[j] = (uint16_t)(sBuffer[(j << 1) + 1] << 8) | (sBuffer[(j << 1) + 2]);
					}

					// Convert to system values
					for (j = 0; j < MAX_RC_CHANNELS; j++)
					{
						// Subtract MODEB offset
						itemp16 = TempRxChannel[j] - 2048;
						
						// Expand into OpenAero2 units x0.8544 (0.8555)	(1250/1463)
						itemp16 = (itemp16 >> 1) + (itemp16 >> 2) + (itemp16 >> 4) + (itemp16 >> 5) + (itemp16 >> 7) + (itemp16 >> 8);

						// Add back in OpenAero2 offset
						RxChannel[Config.ChannelOrder[j]] = itemp16 + 3750;
					}
				}
			}
		} // (Config.RxMode == MODEB)

		//************************************************************
		//* HoTT SUMD RX Data format 115200Kbit/s, 8 data bit, no parity, and one stop bit. 
		//* 
		//* First byte = vendor ID		0xA8 
		//* Second byte = status		0x01	Valid byte
		//*								0x81	Valid byte with failsafe bit set
		//* Third byte			0x02 to 0x20	Number of channels (2 to 32)
		//*
		//* Next 2 to 32 bytes = n channels of 16-bit servo data, high-byte first
		//* Last 2 bytes = CRC value over first n*2 + 3 bytes (data + header), using CRC-CCITT algorithm.
		//*
		//* Pulse length conversion from [0...4095] to �s:
		//*      900�s  -> 0x1c20 (7200)
		//*      1500�s -> 0x2ee0 (12000)
		//*      2100�s -> 0x41a0 (16800)
		//*
		//* Total range is: 16800 - 7200 = 9600 or +/-4800 bits and +/- 600us
		//*
		//* The data values can range from 7200 to 16800 to define a servo pulse width.
		//* Each bit in servo data corresponds to pulse width change of 125ns or 0.125us. 
		//* 
		//* 8000	= 1000us
		//* 12000 	= 1500us +/- 4000 for 1-2ms
		//* 16000	= 2000us
		//*
		//************************************************************
		
		// Handle HoTT SUMD format
		if (Config.RxMode == SUMD)
		{
			// Work out the expected number of bytes based on the channel info (3rd byte)
			if (bytecount == 2)
			{
				// Look at the number of channels x 2 + 2(CRC) + 3(Header)
				packet_size = (sBuffer[2] << 1) + 5;
				
				// Sanity check for packet size
				if (packet_size > MAXSUMDPACKET)
				{
					packet_size = MAXSUMDPACKET;
				}
			}

			// Check checksum when all data received and packet size determined
			if ((packet_size > 0) && (bytecount == (packet_size - 1)))
			{
				crc = 0;
			
				// Add up checksum for all bytes up to but not including the checksum
				for (j = 0; j < (packet_size - 2); j++)
				{
					crc = CRC16(crc, sBuffer[j]);
				}
			
				// Extract the packet's own checksum
				checkcrc = ((uint16_t)(sBuffer[packet_size - 2] << 8) | (uint16_t)(sBuffer[packet_size - 1]));
				
				// Compare with the calculated one and process data if ok
				if (checkcrc == crc)
				{
					// RC sync established
					Interrupted = true;
					
					// Reset signal loss timer and Overdue state 					
					Servo_TCNT2 = TCNT2;
					RC_Timeout = 0;
					Overdue = false;
			
					// Copy unconverted channel data
					for (j = 0; j < MAX_RC_CHANNELS; j++)
					{
						// Combine bytes from buffer
						TempRxChannel[j] = (uint16_t)(sBuffer[(j << 1) + 3] << 8) | (sBuffer[(j << 1) + 4]);
					}

					// Convert to system values
					for (j = 0; j < MAX_RC_CHANNELS; j++)
					{
						// Subtract SUMD offset
						itemp16 = TempRxChannel[j] - 12000;
						
						// Expand into OpenAero2 units x0.3125 (0.3125)	(1250/4000)
						// 0.25 + 0.0625 (1/4 + 1/16)
						itemp16 = (itemp16 >> 2) + (itemp16 >> 4);

						// Add back in OpenAero2 offset
						RxChannel[Config.ChannelOrder[j]] = itemp16 + 3750;
					}
				}
			}
		} // (Config.RxMode == SUMD)

		//************************************************************
		//* Common exit code
		//************************************************************

		// Increment byte count
		bytecount++;
	
	} // Valid data
}

//***********************************************************
//* TCNT1 atomic read subroutine
//* from Atmel datasheet
//* TCNT1 is the only 16-bit timer
//***********************************************************

uint16_t TIM16_ReadTCNT1(void)
{
	uint8_t sreg;
	uint16_t i;
	
	/* Save global interrupt flag */
	sreg = SREG;
	
	/* Disable interrupts */
	cli();
	
	/* Read TCNTn into i */
	i = TCNT1;
	
	/* Restore global interrupt flag */
	SREG = sreg;
	return i;
}

//***********************************************************
// Disable RC interrupts as required
//***********************************************************

void Disable_RC_Interrupts(void)
{
	cli();	// Disable interrupts

	// Disable PWM input interrupts
	PCMSK1 = 0;							// Disable AUX
	PCMSK3 = 0;							// Disable THR
	EIMSK  = 0;							// Disable INT0, 1 and 2

	// Disable receiver (flushes buffer)
	UCSR0B &= ~(1 << RXEN0);	

	// Disable serial interrupt	
	UCSR0B &= ~(1 << RXCIE0);
	
	// Clear interrupt flags
	PCIFR	= 0x0F;						// Clear PCIF0~PCIF3 interrupt flags
	EIFR	= 0x00; 					// Clear INT0~INT2 interrupt flags (Elevator, Aileron, Rudder/CPPM)
	
	sei(); // Re-enable interrupts
}

//***********************************************************
// Reconfigure RC interrupts
//***********************************************************

void init_int(void)
{
	cli();	// Disable interrupts
	
	switch (Config.RxMode)
	{
		case CPPM_MODE:
			PCMSK1 = 0;							// Disable AUX
			PCMSK3 = 0;							// Disable THR
			EIMSK = 0x04;						// Enable INT2 (Rudder/CPPM input)
			UCSR0B &= ~(1 << RXCIE0);			// Disable serial interrupt
			UCSR0B &= ~(1 << RXEN0);			// Disable receiver and flush buffer
			break;

		case PWM:
			PCMSK1 |= (1 << PCINT8);			// PB0 (Aux pin change mask)
			PCMSK3 |= (1 << PCINT24);			// PD0 (Throttle pin change mask)
			EIMSK  = 0x07;						// Enable INT0, 1 and 2 
			UCSR0B &= ~(1 << RXCIE0);			// Disable serial interrupt
			UCSR0B &= ~(1 << RXEN0);			// Disable receiver and flush buffer
			break;

		case SUMD:
		case MODEB:
		case XTREME:
		case SBUS:
		case SPEKTRUM:
			// Disable PWM input interrupts
			PCMSK1 = 0;							// Disable AUX
			PCMSK3 = 0;							// Disable THR
			EIMSK  = 0;							// Disable INT0, 1 and 2 
			
			// Enable serial receiver and interrupts
			UCSR0B |= (1 << RXCIE0);			// Enable serial interrupt
			UCSR0B |= (1 << RXEN0);				// Enable receiver
			
			packet_size = 0;					// Reset packet size until new data comes in
			
			break;

		default:
			break;	
	}	

	// Clear interrupt flags
	PCIFR	= 0x0F;								// Clear PCIF0~PCIF3 interrupt flags
	EIFR	= 0x00; 							// Clear INT0~INT2 interrupt flags (Elevator, Aileron, Rudder/CPPM)

	sei(); // Re-enable interrupts

} // init_int