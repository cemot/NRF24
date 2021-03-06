#include "NRF24.h"

/*********************************************************
 *
 * PUBLIC
 *
 *********************************************************/

bool NRF24::begin(uint8_t cePin, uint8_t csnPin, uint32_t _netmask)
{
	pinMode(cePin,OUTPUT);
	pinMode(csnPin,OUTPUT);

	// store registers for quicker access later on
	cePort = portOutputRegister(digitalPinToPort(cePin));
	ceInput = portInputRegister(digitalPinToPort(cePin));
	ceBitMask = digitalPinToBitMask(cePin);
	csnPort = portOutputRegister(digitalPinToPort(csnPin));
	csnBitMask = digitalPinToBitMask(csnPin);

	SPI.begin();
	// note: when using a prototype board with long wires it may be better to switch to /4 for better signal integrity
	// maximum clock frequency for NRF24L01+ is 10MHz
	// sometimes data can be corrupted so try putting a slower clock if the results are unpredicatable/weird/etc
	SPI.setClockDivider(SPI_CLOCK_DIV4);
	csnHigh();

	// Put us in a known state: power down mode
	ceLow();

	// the 4 high bits on the address are the netmask
	netmask = _netmask;

	// wait for 'power on reset'
	delay(100);

	// Some initial values
	setRetries(15, 15);

	// Set maximum output power
	setPowerAmplificationLevel(NRF24_PA_MAX);

	// Must match on both ends
	setDataRate(NRF24_2MBPS);

	// Must match on both ends
	setCRCMode(NRF24_CRC_16BIT);

	// Must match on both ends
	// When using 2MBPS datarate the channels should be at least 2MHz apart
	setChannel(76);

	// Activate features - otherwise we can't modify the FEATURES registry
	// 0x73 is a magic number from the datasheet
	writeRegister(ACTIVATE, 0x73);

	// to keep things simple we only support dynamic payloads so enable this feature with payloads
	// allow no-ack payloads too for broadcast
	// and of course payloads in ACK packets
	writeRegister(FEATURE, readRegister(FEATURE) | EN_DPL | EN_ACK_PAY | EN_DYN_ACK);

	// enable auto ack on all pipes, required for dynamic payloads
	writeRegister(EN_AA, ENAA_P0 | ENAA_P1 | ENAA_P2 | ENAA_P3 | ENAA_P4 | ENAA_P5);

	// enable dynamic payload on all pipes
	writeRegister(DYNPD, DPL_P0 | DPL_P1 | DPL_P2 | DPL_P3 | DPL_P4 | DPL_P5);

	// set address width to 5 bytes
	writeRegister(SETUP_AW, 0x3);

	// clear interrupt flags
	writeRegister(STATUS, readRegister(STATUS) | RX_DR | TX_DS | MAX_RT);

	// save power until needed
	setActive(false);

	// no pipe activated
	previousPipe = -1;
	numPipes = 0;
	previousTXAddress = 0;

	// enable ACK by default as it results in much more reliable transmission (at the expense of ~30% less throughput)
	setACKEnabled(true);

	// Clear any pending data
	flushRX();
	flushTX();

	return false;
}

/*********************************************************/

void NRF24::setAddress(uint8_t address)
{
	ownAddress = address;

	uint8_t buf[5];
	assembleFullAddress(address, buf);

	// pipe 0 is our own address
	writeRegister(RX_ADDR_P0, buf, 5);

	previousRXAddress = address;

	// make sure pipe 0 is enabled
	writeRegister(EN_RXADDR, readRegister(EN_RXADDR) | 0x01);
}

/********************************************************/

int8_t NRF24::listenToAddress(uint8_t address)
{
	if (numPipes >= 5) return -1;

	uint8_t pipeIndex = numPipes + 1;

	if (pipeIndex <= 1)
	{
		uint8_t buf[5];
		assembleFullAddress(address, buf);

		// pipes 0 and 1
		writeRegister(RX_ADDR_P0 + pipeIndex, buf, 5);
	}
	else
	{
		writeRegister(RX_ADDR_P0 + pipeIndex, address);
	}

	writeRegister(EN_RXADDR, readRegister(EN_RXADDR) | (1 << pipeIndex));

	// We most likely want to listen to data so go into RX mode
	startListening();

	return numPipes++;
}

/********************************************************/

void NRF24::setChannel(uint8_t channel)
{
	// note: with 2mpbs mode the channel is 2mhz wide
	// because of this channels should be spaced more than 2 apart to prevent overlap
	// see datasheet page 23
	writeRegister(RF_CH, channel & 0x7F);
}

/********************************************************/

uint8_t NRF24::getChannel()
{
	return readRegister(RF_CH) & 0x7F;
}

/********************************************************/

void NRF24::setDataRate(nrf24_datarate_e dataRate)
{
	uint8_t rfSetup = readRegister(RF_SETUP);

	// note: this setup works for NRF24L01+ only (not without +)

	// clear previous. this results in 1mbps data rate
	rfSetup &= ~(RF_DR_HIGH | RF_DR_LOW);

	if (dataRate == NRF24_250KBPS)
	{
		rfSetup |= RF_DR_LOW;
	}
	else if (dataRate == NRF24_2MBPS)
	{
		rfSetup |= RF_DR_HIGH;
	}

	writeRegister(RF_SETUP, rfSetup);
}


/********************************************************/

void NRF24::setPowerAmplificationLevel(nrf24_pa_level_e level)
{
	uint8_t rfSetup = readRegister(RF_SETUP);

	// clear previous bits
	rfSetup &= ~(0x6); // low

	if (level == NRF24_PA_MID)
	{
		rfSetup |= RF_PA_LOW;
	}
	else if (level == NRF24_PA_HIGH)
	{
		rfSetup |= RF_PA_HIGH;
	}
	else if (level == NRF24_PA_MAX)
	{
		rfSetup |= RF_PA_LOW | RF_PA_HIGH;
	}

	// store new config
	writeRegister(RF_SETUP, rfSetup);
}

/********************************************************/

nrf24_pa_level_e NRF24::getPowerAmplificationLevel()
{
	return (nrf24_pa_level_e)((readRegister(RF_SETUP) >> 1) & 0x3);
}

/********************************************************/

bool NRF24::broadcast(uint8_t *data, uint8_t length)
{
	// send data "to ourselves" - anybody listening to our address will receive this
	return transmit(ownAddress, data, length, false);
}

/********************************************************/

bool NRF24::broadcast(char *message)
{
	return broadcast((uint8_t *)message, strlen(message) + 1);
}

/********************************************************/

bool NRF24::broadcast_P(const __FlashStringHelper *message)
{
	// copy PROGMEM string to RAM
	char buffer[32];
	memcpy_P(buffer, message, 32);
	return broadcast(buffer);
}

/********************************************************/

bool NRF24::send(uint8_t targetAddress, uint8_t *data, uint8_t length, uint8_t *numAttempts)
{
	// transmit to address, expect ACK
	bool sent = transmit(targetAddress, data, length, ackEnabled);

	if (numAttempts)
	{
		// Read number of attempts
		uint8_t observe = readRegister(OBSERVE_TX);
		*numAttempts = observe & 0xF;
	}

	return sent;
}

/********************************************************/

int8_t NRF24::send(uint8_t targetAddress, uint8_t *data, uint8_t length, uint8_t *responseBuffer, uint8_t bufferSize, uint8_t *numAttempts)
{
	// Clear any old data from FIFO
	flushRX();

	// send as normal, then see if the ACK contained a payload
	bool sent = send(targetAddress, data, length, NULL);

	if (!sent) return -1;

	// no point looking for ACK payload if ack was disabled
	if (ackEnabled)
	{
		csnLow();
		bool ackPayloadAvailable = SPI.transfer(NOP) & RX_DR;
		csnHigh();

		if (!ackPayloadAvailable)
		{
			return 0;
		}

		return read(responseBuffer, bufferSize);
	}
	
	return 0;
}

/********************************************************/

bool NRF24::send(uint8_t targetAddress, char *message)
{
	// Both TX and RX on pipe 0 must match the target address
	// RX is required to receive ACK

	return send(targetAddress, (uint8_t *)message, strlen(message) + 1, NULL);
}

/********************************************************/

bool NRF24::queueResponse(uint8_t *data, uint8_t length)
{
	if (length > 32) length = 32;

	// must be listening to access ACK FIFO
	bool wasListening = listening;
	if (!listening) startListening();

	// first check if the FIFO is already full
	if (readRegister(FIFO_STATUS) & TX_FULL_FIFO) return false;

	// all good, clock in the data
	csnLow();
	SPI.transfer(W_ACK_PAYLOAD);
	while (length--)
	{
		SPI.transfer(*data++);
	}
	csnHigh();

	if (!wasListening) stopListening();

	return true;
}

/********************************************************/

uint8_t NRF24::available(uint8_t *listener)
{
	uint8_t status = readRegister(STATUS);
	bool rxDataReady = status & RX_DR;

	if (rxDataReady)
	{
		if (listener)
		{
			*listener = (status >> RX_P_NO) & 0x3;
		}

		// get number of bytes available
		return readRegister(R_RX_PL_WID);
	}

	return 0;
}

/********************************************************/

uint8_t NRF24::read(uint8_t *buf, uint8_t bufferSize)
{
	// disable RX mode
	ceLow();

	uint8_t payloadSize = readRegister(R_RX_PL_WID);

	// make sure we don't overflow the buffer
	if (bufferSize > payloadSize) bufferSize = payloadSize;

	// fetch data from fifo
	csnLow();
	SPI.transfer(R_RX_PAYLOAD);
	while (bufferSize--)
	{
		*buf++ = SPI.transfer(NOP);
	}
	csnHigh();

	if (*(buf - payloadSize) == 0)
	{
		// SOMETIMES we end up here even if the transmission is identical
	}

	// clear RX bit so we can receive more data
	writeRegister(STATUS, readRegister(STATUS) | RX_DR);

	// continue listening
	ceHigh();

	return payloadSize;
}

/********************************************************/

uint8_t NRF24::read(char *buf, uint8_t bufferSize)
{
	uint8_t bytesRead = read((uint8_t *)buf, bufferSize - 1);
	buf[bufferSize - 1] = '\0';
	return bytesRead;
}

/********************************************************/

void NRF24::setActive(bool active)
{
	uint8_t config = readRegister(CONFIG);
	config &= ~PWR_UP;
	if (active) config |= PWR_UP;
	writeRegister(CONFIG, config);

	// Need to wait for activation. Datasheet says this should be controlled by MCU so let's be good citizens
	// Actually only 150uS is required with external oscillator (likely) but let's be on the safe side and
	// use 1.5mS which is the delay when using the internal oscillator
	delayMicroseconds(1500);
}

/********************************************************/

bool NRF24::getActive()
{
	return readRegister(CONFIG) & PWR_UP;
}

/********************************************************/

nrf24_mode_e NRF24::getCurrentMode()
{
	// Determine state. based on page 22, 23 in datasheet
	// Requirements:
	//   power down:	PWR_UP=0
	//   stamdby 1:		PWR_UP=1, CE=0
	//   standby 2:		PWR_UP=1, CE=1, PRIM_RX=0, TX_FIFO=empty
	//   rx:			PWR_UP=1, CE=1, PRIM_RX=1
	//   tx:			PWR_UP=1, CE=1, PRIM_RX=0, TX_FIFO=not empty

	// I guess it's possible we're in a transition state too but they last 130uS and
	// the code changing states is blocking so probably don't need to worry about that

	uint8_t config = readRegister(CONFIG);

	// not doing anything, power saving mode, crystal disabled
	if (!config & PWR_UP) return NRF24_MODE_POWER_DOWN;

	// waiting for some magic to happen, crystal enabled so quicker startup
	if (!ceIsHigh()) return NRF24_MODE_STANDBY1;

	// listening to data
	if (config & PRIM_RX) return NRF24_MODE_RX;

	uint8_t fifoEmpty = readRegister(FIFO_STATUS) & TX_EMPTY;

	// if the fifo is empty we're ready to send data whenever the fifo gets filled again
	if (fifoEmpty) return NRF24_MODE_STANDBY2;

	// transmitting data. we shouldn't stay in this state as the call is blocking and automatically switches back
	// maybe this is useful for debugging? :)
	return NRF24_MODE_TX;
}

/********************************************************/

void NRF24::startListening()
{
	// Enter RX mode
	writeRegister(CONFIG, readRegister(CONFIG) | PRIM_RX | PWR_UP);
	writeRegister(STATUS, RX_DR | TX_DS | MAX_RT);

	// we might have sent data before which caused pipe0 to get the target address
	if (previousRXAddress != ownAddress)
	{
		// restore our own address
		uint8_t buf[5];
		assembleFullAddress(ownAddress, buf);
		writeRegister(RX_ADDR_P0, buf, 5);

		previousRXAddress = ownAddress;
	}

	// Transition to RX mode
	ceHigh();

	// Make sure we start from a clean slate
	flushRX();
	flushTX();

	listening = true;

	// We're in RX mode in 130uS. No point blocking this though
}

/********************************************************/

void NRF24::stopListening()
{
	// Enter Power down mode
	writeRegister(CONFIG, readRegister(CONFIG) & ~PRIM_RX & ~PWR_UP);
	ceLow();

	// Clear any remaining data from FIFOs
	flushRX();
	flushTX();

	// Store state so we know what to transition to in the future
	listening = false;
}

/********************************************************/

void NRF24::setRetries(uint8_t delay, uint8_t count)
{
	// Delay between retries: delay * 250uS + 258uS
	// Count how many times to try before giving up. Max 15
	if (delay > 0xF) delay = 0xF;
	if (count > 0xF) count = 0xF;
	writeRegister(SETUP_RETR, (delay << 4) | count);
}

/********************************************************/

void NRF24::setCRCMode(nrf24_crc_mode_e mode)
{
	uint8_t config = readRegister(CONFIG);
	config &= ~EN_CRC;
	config &= ~CRCO;
	if (mode == NRF24_NO_CRC) return;
	config |= EN_CRC;
	if (mode == NRF24_CRC_16BIT) config |= CRCO;
	writeRegister(CONFIG, config);
}

/********************************************************/

void NRF24::setACKEnabled(bool ack)
{
	// automatically does magic on transmission
	ackEnabled = ack;
}


/*********************************************************
 *
 * PRIVATE
 *
 *********************************************************/

uint8_t NRF24::readRegister(uint8_t reg)
{
	csnLow();
	SPI.transfer(R_REGISTER | reg);
	uint8_t result = SPI.transfer(NOP);
	csnHigh();

	return result;
}

/*********************************************************/

void NRF24::writeRegister(uint8_t reg, uint8_t value)
{
	csnLow();
	SPI.transfer(W_REGISTER | (REGISTER_MASK & reg));
	SPI.transfer(value);
	csnHigh();
}

/*********************************************************/

void NRF24::writeRegister(uint8_t reg, uint8_t *value, uint8_t numBytes)
{
	csnLow();
	SPI.transfer(W_REGISTER | (REGISTER_MASK & reg));
	while (numBytes--)
	{
		SPI.transfer(*value++);
	}
	csnHigh();
}

/*********************************************************/

bool NRF24::transmit(uint8_t targetAddress, uint8_t *data, uint8_t length, bool ack)
{
	// what's the point of transmitting 0 bytes? :)
	if (length == 0) return false;

	// max 32 bytes allowed
	if (length > 32) length = 32;

	// we only need to update the TX address if it's changed
	uint8_t buf[5];
	if (previousTXAddress != targetAddress)
	{
		assembleFullAddress(targetAddress, buf);
		writeRegister(TX_ADDR, buf, 5);

		previousTXAddress = targetAddress;
	}

	// RX address doesn't matter if we won't receive an ACK
	if (ack && previousRXAddress != targetAddress)
	{
		assembleFullAddress(targetAddress, buf);
		writeRegister(RX_ADDR_P0, buf, 5);

		previousRXAddress = targetAddress;

		// note that pipe0 now has the target address; this is required to receive the ACK
		// when we call startListening() our own address gets restored
	}

	writeRegister(STATUS, readRegister(STATUS) | RX_DR | TX_DS | MAX_RT);

	uint8_t config = readRegister(CONFIG);
	bool wasActive = config & PWR_UP;
	bool wasListening = listening;

	// Need to go through Standby-I in order to transition to TX
	ceLow();

	// stopListening();

	config |= PWR_UP;	// set to active to enable transmission
	config &= ~PRIM_RX;	// disable rx mode (aka enable tx mode)

	// go into PTX mode
	writeRegister(CONFIG, config);

	if (!wasActive) delay(2);	// wait to enter Standby-I mode

	// transfer payload data to FIFO
	csnLow();
	if (ack)
	{
		SPI.transfer(W_TX_PAYLOAD);
	}
	else
	{
		SPI.transfer(W_TX_PAYLOAD_NO_ACK);
	}
	while (length--)
	{
		SPI.transfer(*data++);
	}
	csnHigh();

	// transmit!
	ceHigh();

	// PLL takes 130uS to start up

	// -------

	// Technically we could exit at this point and separately listen to the the interrupt
	// To keep things simple let's block this though and just poll the register

	// Could also poll the IRQ pin (if connected) but there doesn't seem to be a huge performance benefit so let's keep things simple

	// If we wanted even higher throughput we'd upload more data to the FIFO during transmission. 
	// This becomes quite messy and is very much and edge case so this is not supported.

	// the timeout can occur if the chip isn't responding, shouldn't happen if everything is in order
	static const uint16_t timeout = 500;
	uint32_t txStarted = millis();
	bool txComplete = false;
	bool maxRetriesPassed = false;
	static uint8_t status;
	do
	{
		// Any SPI write will return the status register so we can save a byte by not having to input the status address
		// Huge performance improvement! (not really, but why not)
		csnLow();
		status = SPI.transfer(NOP);
		csnHigh();
	}
	while (
		!(txComplete = status & TX_DS) && 
		!(maxRetriesPassed = status & MAX_RT) &&
		millis() < txStarted + timeout
	);

	// interrupt has now occurred, status register updated
	// we'll clear this interrupt on next transmission

	// If txComplete is false it means the transmission failed after all the attempts set in setRetries().
	// No ACK was received

	// switch to Standby-I
	ceLow();

	if (!wasActive)
	{
		// switch to power down
		setActive(false);
	}
	else
	{
		// return back to RX if we were there before, otherwise to Standby 1 (ce low)
		if (wasListening) startListening();
	}

	return txComplete;
}

/*********************************************************/

void NRF24::assembleFullAddress(uint8_t address, uint8_t buf[5])
{
	buf[4] = (netmask >> 24) & 0xFF;
	buf[3] = (netmask >> 16) & 0xFF;
	buf[2] = (netmask >> 8) & 0xFF;
	buf[1] = (netmask >> 0) & 0xFF;
	buf[0] = address;
}

/*********************************************************/

void NRF24::flushTX()
{
	csnLow();
	SPI.transfer(FLUSH_TX);
	csnHigh();
}

/*********************************************************/

void NRF24::flushRX()
{
	csnLow();
	SPI.transfer(FLUSH_RX);
	csnHigh();
}