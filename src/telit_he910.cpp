#include "telit_he910.h"
#include "telit_he910_platforms.h"
#include "interface/uart.h"
#include "util/log.h"
#include "util/timer.h"
#include "gpio.h"
#include "config.h"
#include "can/canread.h"
#include "WProgram.h"
#include "http.h"
#include "commands/commands.h"
#include "interface/interface.h"
#include <string.h>
#include <stdio.h>

namespace gpio = openxc::gpio;
namespace uart = openxc::interface::uart;
namespace can = openxc::can; // using this to get publishVehicleMessage() for GPS...no sense re-inventing the wheel
namespace http = openxc::http;
namespace telit = openxc::telitHE910;
namespace commands = openxc::commands;

using openxc::interface::uart::UartDevice;
using openxc::gpio::GpioValue;
using openxc::gpio::GPIO_DIRECTION_OUTPUT;
using openxc::gpio::GPIO_DIRECTION_INPUT;
using openxc::gpio::GPIO_VALUE_HIGH;
using openxc::gpio::GPIO_VALUE_LOW;
using openxc::util::time::delayMs;
using openxc::util::log::debug;
using openxc::util::time::uptimeMs;
using openxc::config::getConfiguration;
using openxc::telitHE910::TELIT_CONNECTION_STATE;
using openxc::payload::PayloadFormat;

/*PRIVATE MACROS*/

#define TELIT_MAX_MESSAGE_SIZE		512
#define NETWORK_CONNECT_TIMEOUT		120000

/*PRIVATE VARIABLES*/

static unsigned int bauds[3] = {
	230400,
	115200,
	57600
};

static const char* gps_fix_enum[openxc::telitHE910::FIX_MAX_ENUM] = {"NO_FIX_0", "NO_FIX", "2D_FIX", "3D_FIX"};

static char recv_data[1024];	// common buffer for receiving data from the modem
static char* pRx = recv_data;
#warning "TelitDevice* can be removed with some refactoring"
static TelitDevice* telitDevice;
static bool connect = false;
static const unsigned int sendBufferSize = 4096;
static uint8_t sendBuffer[sendBufferSize];
static uint8_t* pSendBuffer = sendBuffer;
static const unsigned int commandBufferSize = 512;
static uint8_t commandBuffer[commandBufferSize];
static uint8_t* pCommandBuffer = commandBuffer;

static TELIT_CONNECTION_STATE state = telit::POWER_OFF;

/*PRIVATE FUNCTIONS*/

static bool autobaud(openxc::telitHE910::TelitDevice* device);
static void telit_setIoDirection(void);
static void setPowerState(bool enable);
static bool sendCommand(TelitDevice* device, char* command, char* response, uint32_t timeoutMs);
static void sendData(TelitDevice* device, char* data, unsigned int len);
static void clearRxBuffer(void);
static bool getResponse(char* startToken, char* stopToken, char* response, unsigned int maxLen);
static bool parseGPSACP(const char* GPSACP);

TELIT_CONNECTION_STATE openxc::telitHE910::getDeviceState() {
	return state;
}

TELIT_CONNECTION_STATE openxc::telitHE910::connectionManager(TelitDevice* device) {

	static unsigned int subState = 0;
	static unsigned int timer = 0;
	static NetworkDescriptor current_network = {};
	static bool pdp_connected = false;

	// device stats
	static unsigned int SIMstatus = 0;
	static NetworkConnectionStatus connectStatus = UNKNOWN;
	static bool GPSenabled = false;
	static char ICCID[32];
	static char IMEI[32];
	
	switch(state)
	{
		case POWER_OFF:
		
			device->descriptor.type = openxc::interface::InterfaceType::TELIT;
		
			setPowerState(false);
			telitDevice = device;
			state = POWER_ON_DELAY;
			
			break;
			
		case POWER_ON_DELAY:
		
			switch(subState)
			{
				case 0:
				
					timer = uptimeMs() + 1000;
					subState = 1;
					
					break;
					
				case 1:
				
					if(uptimeMs() > timer)
					{
						state = POWER_ON;
						subState = 0;
					}
					
					break;
			}
		
			break;
			
		case POWER_ON:
		
			setPowerState(true);
			state = POWER_UP_DELAY;
		
			break;
			
		case POWER_UP_DELAY:
		
			switch(subState)
			{
				case 0:
				
					timer = uptimeMs() + 8500;
					subState = 1;
					
					break;
					
				case 1:
				
					if(uptimeMs() > timer)
					{
						state = INITIALIZE;
						subState = 0;
					}
					
					break;
			}
		
			break;
			
		case INITIALIZE:
		
			switch(subState)
			{
				case 0:
				
					// figure out the baud rate
					if(autobaud(device))
					{
						timer = uptimeMs() + 1000;
						subState = 1;
					}
					else
					{
						debug("Failed to set the baud rate for Telit HE910...is the device connected to 12V power?");
						state = POWER_OFF;
						break;
					}
				
					break;
					
				case 1:
				
					if(uptimeMs() > timer)
					{
						subState = 2;
					}
				
					break;
					
				case 2:
					
					// save settings
					if(saveSettings() == false)
					{
						debug("Failed to save modem settings, continuing with device initialization.");
					}
					
					// check SIM status
					if(getSIMStatus(&SIMstatus) == false)
					{
						subState = 0;
						state = POWER_OFF;
						break;
					}
					
					// start the GPS chip
					if(device->config.globalPositioningSettings.gpsEnable)
					{
						if(setGPSPowerState(true) == false)
						{
							subState = 0;
							state = POWER_OFF;
							break;
						}
					}
					
					// make sure SIM is installed, else exit
					if(SIMstatus != 1)
					{
						debug("SIM not detected, aborting device initialization.");
						subState = 0;
						state = POWER_OFF;
						break;
					}
					
					// get device identifier (IMEI)
					if(getDeviceIMEI(IMEI) == false)
					{
						subState = 0;
						state = POWER_OFF;
						break;
					}
					memcpy(device->deviceId, IMEI, strlen(IMEI) < MAX_DEVICE_ID_LENGTH ? strlen(IMEI) : MAX_DEVICE_ID_LENGTH);
					
					// get SIM number (ICCID)
					if(getICCID(ICCID) == false)
					{
						subState = 0;
						state = POWER_OFF;
						break;
					}
					memcpy(device->ICCID, ICCID, strlen(ICCID) < MAX_ICCID_LENGTH ? strlen(ICCID) : MAX_ICCID_LENGTH);
					
					// set mobile operator connect mode
					if(setNetworkConnectionMode(device->config.networkOperatorSettings.operatorSelectMode, device->config.networkOperatorSettings.networkDescriptor) == false)
					{
						subState = 0;
						state = POWER_OFF;
						break;
					}
					
					// configure data session
					if(configurePDPContext(device->config.networkDataSettings) == false)
					{
						subState = 0;
						state = POWER_OFF;
						break;
					}
					
					// configure a single TCP/IP socket
					if(configureSocket(1, device->config.socketConnectSettings) == false)
					{
						subState = 0;
						state = POWER_OFF;
						break;
					}
					
					subState = 0;
					state = WAIT_FOR_NETWORK;
					
					break;
			}
		
			break;
			
		case WAIT_FOR_NETWORK:
		
			switch(subState)
			{
				case 0:
					
					if(getNetworkConnectionStatus(&connectStatus))
					{
						if(connectStatus == REGISTERED_HOME || (device->config.networkOperatorSettings.allowDataRoaming && connectStatus == REGISTERED_ROAMING))
						{
							if(getCurrentNetwork(&current_network))
							{
								debug("Telit connected to PLMN %u, access type %u", current_network.PLMN, current_network.networkType);
							}
							subState = 0;
							state = CLOSE_PDP;
						}
						else
						{
							timer = uptimeMs() + 500;
							subState = 1;
						}
					}
					
					break;
					
				case 1:
				
					if(uptimeMs() > timer)
						subState = 0;
				
					break;
			}
		
			break;
			
		case CLOSE_PDP:
		
			// deactivate data session (just in case the network thinks we still have an active PDP context)
			if(closePDPContext())
			{
				subState = 0;
				state = OPEN_PDP_DELAY;
			}
			else
			{	
				subState = 0;
				state = POWER_OFF;
			}

			break;
			
		case OPEN_PDP_DELAY:
		
			switch(subState)
			{
				case 0:
				
					timer = uptimeMs() + 1000;
					subState = 1;
					
					break;
					
				case 1:

					if(uptimeMs() > timer)
					{
						state = OPEN_PDP;
						subState = 0;
					}
					
					break;
			}
			
			break;
			
		case OPEN_PDP:
		
			// activate data session
			if(openPDPContext())
			{
				state = READY;
			}
			else
			{
				state = CLOSE_PDP;
			}
		
			break;
			
		case READY:
		
			switch(subState)
			{
				case 0:
				
					if(getNetworkConnectionStatus(&connectStatus), (connectStatus != REGISTERED_HOME && connectStatus != REGISTERED_ROAMING))
					{
						debug("Modem has lost network connection");
						connect = false;
						state = WAIT_FOR_NETWORK;
					}
					else if(getPDPContext(&pdp_connected), !pdp_connected)
					{
						debug("Modem has lost data session");
						connect = false;
						state = CLOSE_PDP;
					}
					else
					{
						connect = true;
						timer = uptimeMs() + 1000;
						subState = 1;
					}
				
					break;
					
				case 1:
				
					if(uptimeMs() > timer)
						subState = 0;
				
					break;
			}
		
			break;
			
		default:
		
			state = POWER_OFF;
		
			break;
	}
	
	return state;
}

void openxc::telitHE910::deinitialize() {
	state = POWER_OFF;
	setPowerState(false);
}

bool openxc::telitHE910::connected(TelitDevice* device) {

	return connect;

}

static bool autobaud(openxc::telitHE910::TelitDevice* device) {

	bool rc = false;
	unsigned int i = 0;

	// loop through a set of baud rates looking for a valid response
	for(i = 0; i < 3; ++i)
	{
		// set local baud rate to next attempt
		uart::changeBaudRate(device->uart, bauds[i]);
		
		// attempt set the remote baud rate to desired (config.h) value
		if(openxc::telitHE910::setBaud(UART_BAUD_RATE) == true)
		{
			// match local baud to desired value
			uart::changeBaudRate(device->uart, UART_BAUD_RATE);
			
			// we're good!
			rc = true;
			break;
		}
	}
	
	fcn_exit:
	return rc;

}

/*MODEM AT COMMANDS*/

bool openxc::telitHE910::saveSettings() {

	bool rc = true;
	
	if(sendCommand(telitDevice, "AT&W0\r\n", "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::setBaud(unsigned int baudRate) {

	bool rc = true;
	char command[32] = {};
	
	sprintf(command, "AT+IPR=%u\r\n", baudRate);
	if(sendCommand(telitDevice, command, "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::getDeviceFirmwareVersion(char* firmwareVersion) {

	bool rc = true;
	
	if(sendCommand(telitDevice, "AT+CGMR\r\n", "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(getResponse("AT+CGMR\r\n\r\n", "\r\n\r\nOK\r\n", firmwareVersion, 31) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::getSIMStatus(unsigned int* status) {

	bool rc = true;
	char temp[8];

	if(sendCommand(telitDevice, "AT#QSS?\r\n", "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(getResponse("#QSS: ", "\r\n\r\n", temp, 7) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	*status = atoi(&temp[2]);
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::getICCID(char* ICCID) {

	bool rc = true;

	if(sendCommand(telitDevice, "AT#CCID\r\n", "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(getResponse("#CCID: ", "\r\n\r\n", ICCID, 31) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::getDeviceIMEI(char* IMEI) {

	bool rc = true;

	if(sendCommand(telitDevice, "AT+CGSN\r\n", "\r\n\r\nOK\r\n", 2000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(getResponse("AT+CGSN\r\n\r\n", "\r\n\r\nOK\r\n", IMEI, 31) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return true;

}

bool openxc::telitHE910::setNetworkConnectionMode(OperatorSelectMode mode, NetworkDescriptor network) {

	bool rc = true;
	char command[64] = {};

	switch(mode)
	{
		case AUTOMATIC:
		
			sprintf(command, "AT+COPS=0,2\r\n");

			break;
			
		case MANUAL:
		
			sprintf(command, "AT+COPS=1,2,%u,%u\r\n", network.PLMN, network.networkType);
		
			break;
			
		case DEREGISTER:
		
			sprintf(command, "AT+COPS=2,2");
			
			break;
			
		case SET_ONLY:
		
			sprintf(command, "AT+COPS=3,2");
			
			break;
			
		case MANUAL_AUTOMATIC:
		
			sprintf(command, "AT+COPS=4,2,%u,%u\r\n", network.PLMN, network.networkType);
			
			break;
			
		default:
		
			debug("Modem received invalid operator select mode");
			
			break;
	}
	
	if(sendCommand(telitDevice, command, "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}

	fcn_exit:
	return rc;	

}

bool openxc::telitHE910::getNetworkConnectionStatus(NetworkConnectionStatus* status) {

	bool rc = true;
	char temp[8] = {};
	
	if(sendCommand(telitDevice, "AT+CREG?\r\n", "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(getResponse("+CREG: ", "\r\n\r\nOK\r\n", temp, 7) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	*status = (NetworkConnectionStatus)atoi(&temp[2]);
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::getCurrentNetwork(NetworkDescriptor* network) {

	bool rc = true;
	char temp[32] = {};
	char* p = NULL;
	
	// response: +COPS: <mode>,<format>,<oper>,<AcT>
	
	if(sendCommand(telitDevice, "AT+COPS?\r\n", "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(getResponse("+COPS: ", "\r\n\r\nOK\r\n", temp, 31) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(p = strchr(temp, ','), p)
	{
		p++;
		if(p = strchr(p, ','), p)
		{
			p+=2;
			network->PLMN = atoi(p);
			if(p = strchr(p, ','), p)
			{
				p++;
				network->networkType = (openxc::telitHE910::NetworkType)atoi(p);
			}
		}
	}
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::configurePDPContext(NetworkDataSettings dataSettings) {

	bool rc = true;
	char command[64] = {};
	
	sprintf(command,"AT+CGDCONT=1,\"IP\",\"%s\"\r\n", dataSettings.APN);
	if(sendCommand(telitDevice, command, "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::configureSocket(unsigned int socketNumber, SocketConnectSettings socketSettings) {

	bool rc = true;
	char command[64] = {};
	
	if(socketNumber == 0 || socketNumber > 6)
	{
		rc = false;
		goto fcn_exit;
	}
	
	sprintf(command,"AT#SCFG=%u,1,%u,%u,%u,%u\r\n", socketNumber, socketSettings.packetSize, socketSettings.idleTimeout, socketSettings.connectTimeout, socketSettings.txFlushTimer);
	if(sendCommand(telitDevice, command, "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::openPDPContext() {

	bool rc = true;
	
	if(sendCommand(telitDevice, "AT#SGACT=1,1\r\n", "\r\n\r\nOK\r\n", 30000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::closePDPContext() {

	bool rc = true;
	
	if(sendCommand(telitDevice, "AT#SGACT=1,0\r\n", "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::getPDPContext(bool* connected) {

	bool rc = true;
	char command[16] = {};
	char temp[32] = {};
	
	if(sendCommand(telitDevice, "AT#SGACT?\r\n", "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(getResponse("#SGACT: 1,", "\r\n\r\nOK\r\n", temp, 31) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	*connected = (bool)atoi(temp);
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::openSocket(unsigned int socketNumber, ServerConnectSettings serverSettings) {

	bool rc = true;
	char command[128] = {};

	sprintf(command,"AT#SD=%u,0,%u,\"%s\",255,1,1\r\n", socketNumber, serverSettings.port, serverSettings.host);
	if(sendCommand(telitDevice, command, "\r\n\r\nOK\r\n", 15000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;
	
}

bool openxc::telitHE910::isSocketOpen(unsigned int socketNumber) {

	SocketStatus status;
	
	if(getSocketStatus(socketNumber, &status))
	{
		return (status != SOCKET_CLOSED);
	}
	else
	{
		return false;
	}

}

bool openxc::telitHE910::closeSocket(unsigned int socketNumber) {

	bool rc = true;
	char command[16] = {};

	sprintf(command,"AT#SH=%u\r\n", socketNumber);
	if(sendCommand(telitDevice, command, "\r\n\r\nOK\r\n", 5000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;
	
}

bool openxc::telitHE910::getSocketStatus(unsigned int socketNumber, SocketStatus* status) {

	bool rc = true;
	char command[16] = {};
	char temp[8] = {};
	
	sprintf(command, "AT#SS=%u\r\n", socketNumber);
	if(sendCommand(telitDevice, command, "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(getResponse("#SS: ", "\r\n\r\nOK\r\n", temp, 7) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	*status = (SocketStatus)atoi(&temp[2]);
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::isSocketDataAvailable(unsigned int socketNumber) {

	SocketStatus status;
	
	if(getSocketStatus(socketNumber, &status))
	{
		return status == SOCKET_SUSPENDED_DATA_PENDING;
	}
	else
	{
		return false;
	}

}

bool openxc::telitHE910::writeSocket(unsigned int socketNumber, char* data, unsigned int* len) {

	bool rc = true;
	char command[32];
	unsigned long timer = 0;
	unsigned int i = 0;
	int rx_byte = 0;
	unsigned int maxWrite = 0;
	
	// calculate max bytes to write
	maxWrite = (*len > TELIT_MAX_MESSAGE_SIZE) ? TELIT_MAX_MESSAGE_SIZE : *len;
	
	// issue the socket write command
	sprintf(command, "AT#SSENDEXT=%u,%u\r\n", socketNumber, maxWrite);
	if(sendCommand(telitDevice, command, "> ", 5000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	// clear the receive buffer
	clearRxBuffer();
	
	// send data through the socket
	sendData(telitDevice, data, maxWrite);
	
	// start the receive timer
	timer = uptimeMs() + 10000;
	
	// read out the socket data echo (don't need to store it)
	while(1)
	{
		if(rx_byte = uart::readByte(telitDevice->uart), rx_byte > -1)
		{
			++i;
		}
		if(i == maxWrite)
		{
			break;
		}
		if(uptimeMs() >= timer)
		{
			rc = false;
			goto fcn_exit;
		}
	}

	// get the OK
	while(1)
	{
		if(rx_byte = uart::readByte(telitDevice->uart), rx_byte > -1)
		{
			*pRx++ = rx_byte;
		}
		if(strstr(recv_data, "OK"))
		{
			rc = true;
			break;
		}
		if(uptimeMs() >= timer)
		{
			rc = false;
			goto fcn_exit;
		}
	}

	// update write count
	*len = maxWrite;
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::readSocket(unsigned int socketNumber, char* data, unsigned int* len) {

	bool rc = true;
	char command[32] = {};
	char reply[32] = {};
	char* pS = NULL;
	unsigned int readCount = 0;
	unsigned int i = 0;
	unsigned int maxRead = 0;
	int rx_byte = -1;
	unsigned long timer = 0;
	
	// calculate max bytes to read
	maxRead = (*len > TELIT_MAX_MESSAGE_SIZE) ? TELIT_MAX_MESSAGE_SIZE : *len;
	
	// issue the socket read command
	sprintf(command, "AT#SRECV=%u,%u\r\n", socketNumber, maxRead);
	sprintf(reply, "#SRECV: %u,", socketNumber);
	if(sendCommand(telitDevice, command, reply, 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	// start the receive timer
	timer = uptimeMs() + 10000;
	
	// find the start of the reply
	pS = recv_data;
	if(pS = strstr(pS, reply), !pS)
	{
		rc = false;
		goto fcn_exit;
	}
	pS += 10;
	pRx = pS;
	
	// read to end of line
	while(1)
	{
		if(rx_byte = uart::readByte(telitDevice->uart), rx_byte > -1)
		{
			*pRx++ = rx_byte;
		}
		if(strstr(pS, "\r\n"))
		{
			break;
		}
		if(uptimeMs() >= timer)
		{
			rc = false;
			goto fcn_exit;
		}
	}	
	// get the read count
	readCount = atoi(pS);
	pS = pRx;
	
	// read the socket data
	i = 0;
	while(1)
	{
		if(rx_byte = uart::readByte(telitDevice->uart), rx_byte > -1)
		{
			*pRx++ = rx_byte;
			++i;
		}
		if(i == readCount)
		{
			break;
		}
		if(uptimeMs() >= timer)
		{
			rc = false;
			goto fcn_exit;
		}
	}
	// put the read data into caller
	memcpy(data, pS, readCount);
	*len = readCount;
	pS = pRx;
	
	// finish with OK
	while(1)
	{
		if(rx_byte = uart::readByte(telitDevice->uart), rx_byte > -1)
		{
			*pRx++ = rx_byte;
		}
		if(strstr(pS, "\r\n\r\nOK\r\n"))
		{
			break;
		}
		if(uptimeMs() >= timer)
		{
			rc = false;
			goto fcn_exit;
		}
	}

	fcn_exit:
	return rc;

}

bool readSocketOne(unsigned int socketNumber, char* data, unsigned int* len) {

	bool rc = true;
	char command[32] = {};
	char reply[32] = {};
	char* pS = NULL;
	unsigned int readCount = 0;
	unsigned int i = 0;
	unsigned int maxRead = 0;
	int rx_byte = -1;
	unsigned long timer = 0;
	
	// calculate max bytes to read
	maxRead = (*len > 1) ? 1 : *len;
	
	// issue the socket read command
	sprintf(command, "AT#SRECV=%u,%u\r\n", socketNumber, maxRead);
	sprintf(reply, "#SRECV: %u,", socketNumber);
	if(sendCommand(telitDevice, command, reply, 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	// start the receive timer
	timer = uptimeMs() + 10000;
	
	// find the start of the reply
	pS = recv_data;
	if(pS = strstr(pS, reply), !pS)
	{
		rc = false;
		goto fcn_exit;
	}
	pS += 10;
	pRx = pS;
	
	// read to end of line
	while(1)
	{
		if(rx_byte = uart::readByte(telitDevice->uart), rx_byte > -1)
		{
			*pRx++ = rx_byte;
		}
		if(strstr(pS, "\r\n"))
		{
			break;
		}
		if(uptimeMs() >= timer)
		{
			rc = false;
			goto fcn_exit;
		}
	}	
	// get the read count
	readCount = atoi(pS);
	pS = pRx;
	
	// read the socket data
	i = 0;
	while(1)
	{
		if(rx_byte = uart::readByte(telitDevice->uart), rx_byte > -1)
		{
			*pRx++ = rx_byte;
			++i;
		}
		if(i == readCount)
		{
			break;
		}
		if(uptimeMs() >= timer)
		{
			rc = false;
			goto fcn_exit;
		}
	}
	// put the read data into caller
	memcpy(data, pS, readCount);
	*len = readCount;
	pS = pRx;
	
	// finish with OK
	while(1)
	{
		if(rx_byte = uart::readByte(telitDevice->uart), rx_byte > -1)
		{
			*pRx++ = rx_byte;
		}
		if(strstr(pS, "\r\n\r\nOK\r\n"))
		{
			break;
		}
		if(uptimeMs() >= timer)
		{
			rc = false;
			goto fcn_exit;
		}
	}

	fcn_exit:
	return rc;

}

/*SEND/RECEIVE*/

static bool sendCommand(TelitDevice* device, char* command, char* response, uint32_t timeoutMs) {

	bool rc = false;
	
	unsigned int tx_cnt = 0;
	unsigned int tx_size = 0;
	
	int rx_byte = 0;
	
	unsigned long timer = 0;
	
	// clear the receive buffer
	clearRxBuffer();
	
	// send the command
	tx_size = strlen(command);
	for(tx_cnt = 0; tx_cnt < tx_size; ++tx_cnt)
	{
		uart::writeByte(device->uart, command[tx_cnt]);
	}
	
	// start the receive timer
	timer = uptimeMs();
	
	// receive the response
	while(uptimeMs() - timer < timeoutMs)
	{
		if(rx_byte = uart::readByte(device->uart), rx_byte > -1)
		{
			*pRx++ = rx_byte;
		}
		if(strstr(recv_data, response))
		{
			rc = true;
			break;
		}
	}
	
	fcn_exit:
	return rc;

}

static void clearRxBuffer() {

	// purge the HardwareSerial buffer
	((HardwareSerial*)telitDevice->uart->controller)->purge();

	// clear the modem buffer
	memset(recv_data, 0x00, 256);
	pRx = recv_data;
	
	return;

}

static bool getResponse(char* startToken, char* stopToken, char* response, unsigned int maxLen) {
	
	bool rc = true;
	
	char* p1 = NULL;
	char* p2 = NULL;
	
	if(p1 = strstr(recv_data, startToken), !p1)
	{
		rc = false;
		goto fcn_exit;
	}
	if(p2 = strstr(recv_data, stopToken), !p2)
	{
		rc = false;
		goto fcn_exit;
	}
	p1 += strlen(startToken);
	memcpy(response, p1, (maxLen < p2-p1) ? (maxLen) : (p2-p1));
	
	fcn_exit:
	return rc;

}

static void sendData(TelitDevice* device, char* data, unsigned int len) {

	unsigned int i = 0;
	
	for(i = 0; i < len; ++i)
	{
		uart::writeByte(device->uart, data[i]);
		// as I send bytes I'll bet there are bytes coming back and I don't know how the hardware interface is
		// handling them. I assume there is more than the HW registers available under the hood, but if that buffer fills // before I return.....well I'm screwed as bytes will drop
		// the HardwareSerial implementation (cores/pic32) has a 512 byte ring buffer
	}
	
	return;

}

/*MODEM POWER MANAGEMENT*/

// Only want to set the directly once because it flips the power on/off.
static void telit_setIoDirection() {
#ifdef TELIT_HE910_ENABLE_SUPPORT
    static bool directionSet = false;
    if(!directionSet) {
        // be aware that setting the direction here will default it to the off
        // state, so the Bluetooth module will go *off* and then back *on*
        gpio::setDirection(TELIT_HE910_ENABLE_PORT, TELIT_HE910_ENABLE_PIN,
                GPIO_DIRECTION_OUTPUT);
        directionSet = true;
    }
#endif
}

static void setPowerState(bool enabled) {

	#ifdef TELIT_HE910_ENABLE_SUPPORT
    
	enabled = TELIT_HE910_ENABLE_PIN_POLARITY ? enabled : !enabled;
    debug("Turning Telit HE910 %s", enabled ? "on" : "off");
    telit_setIoDirection();
    gpio::setValue(TELIT_HE910_ENABLE_PORT, TELIT_HE910_ENABLE_PIN,
            enabled ? GPIO_VALUE_HIGH : GPIO_VALUE_LOW);
	
	#endif
	
	return;

}

/*GPS*/

bool openxc::telitHE910::setGPSPowerState(bool enable) {

	bool rc = true;
	char command[64] = {};
	
	sprintf(command, "AT$GPSP=%u\r\n", (unsigned int)enable);
	if(sendCommand(telitDevice, command, "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::getGPSPowerState(bool* enable) {

	bool rc = true;
	char command[64] = {};
	char temp[8];
	
	sprintf(command, "AT$GPSP?\r\n");
	if(sendCommand(telitDevice, command, "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(getResponse("$GPSP: ", "\r\n\r\nOK\r\n", temp, 7) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	*enable = (bool)atoi(&temp[0]);
	
	fcn_exit:
	return rc;

}

bool openxc::telitHE910::getGPSLocation() {

	bool rc = true;
	char temp[128] = {};
	static unsigned long next_update = 0;
	
	if(uptimeMs() < next_update)
		goto fcn_exit;
	
	// retrieve the GPS location string from the modem
	if(sendCommand(telitDevice, "AT$GPSACP\r\n", "\r\n\r\nOK\r\n", 1000) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	if(getResponse("$GPSACP: ", "\r\n\r\nOK\r\n", temp, 127) == false)
	{
		rc = false;
		goto fcn_exit;
	}
	
	// now we have the GPS string in 'temp', send to parser to publish signals
	rc = parseGPSACP(temp);
	
	next_update = uptimeMs() + getConfiguration()->telit.config.globalPositioningSettings.gpsInterval;
	
	fcn_exit:
	return rc;

}

static void publishGPSSignal(const char* field_name, char* field_value, openxc::pipeline::Pipeline* pipeline) {
	can::read::publishStringMessage(field_name, field_value, pipeline);
}

static void publishGPSSignal(const char* field_name, float field_value, openxc::pipeline::Pipeline* pipeline) {
	can::read::publishNumericalMessage(field_name, field_value, pipeline);
}

static bool parseGPSACP(const char* GPSACP) {

	bool rc = true;
	char* p1 = NULL;
	char* p2 = NULL;
	
	// pass these to publishVehicleMessage
	char tmp[8] = {};
	float field_value_numerical = 0;
	openxc::pipeline::Pipeline* pipeline;
	openxc::telitHE910::GlobalPositioningSettings* gpsConfig;
	
	pipeline = &getConfiguration()->pipeline;
	gpsConfig = &getConfiguration()->telit.config.globalPositioningSettings;
	
	char splitString[11][16] = {};
	bool validString[11] = {};
	unsigned int i = 0;
	
	// $GPSACP: <UTC>,<latitude>,<longitude>,<hdop>,<altitude>,<fix>,<cog>,<spkm>,<spkn>,<date>,<nsat>
	
	// e.g.: $GPSACP: 080220.479,4542.82691N,01344.26820E,259.07,3,2.1,0.1,0.0,0.0,270705,09 (gps fixed)
	// e.g.: $GPSACP: ,,,,,1,,,,, (gps acquiring satellites)
	// e.g.: $GPSACP: (gps disabled)
	
	// OpenXC will want a set of individual JSON fields like:
	 // "latitude" (-89.9999 to +89.9999 degrees)
	 // "longitude" (-179.9999 to +179.9999 degrees)
	 // "altitude" (n.n meters above MSL)
	 // "gps_course" (n.n degrees from True North)
	 // "gps_speed" (n.n kph)
	 // "gps_nsat" (n)
	 // "gps_time" (hhmmss.sss UTC)
	 // "gps_date" (ddmmyy)
	
	// split 'er up
	p1 = (char*)GPSACP;
	for(i = 0; i < 10; ++i)
	{
		if(p2 = strchr(p1, ','), !p2)
		{
			rc = false;
			goto fcn_exit;
		}
		memcpy(&splitString[i][0], p1, p2-p1);
		validString[i] = (p2-p1 > 0) ? true : false;
		p1=p2+1;
	}
	if(*p1 > 0x2F && *p1 < 0x3A)
	{
		memcpy(&splitString[10][0], p1, 2);
		validString[10] = true;
	}
	
	// 'gps_time'
	if(validString[0] && gpsConfig->gpsEnableSignal_gps_time)
	{
		publishGPSSignal("gps_time", &splitString[0][0], pipeline);
	}
	
	// 'gps_latitude'
	if(validString[1] && gpsConfig->gpsEnableSignal_gps_latitude) 
	{
		memcpy(tmp, &splitString[1][0], 2); // pull out the degrees (always two digits xx)
		field_value_numerical = (float)atoi(tmp); // turn degrees into float
		field_value_numerical += atof(&splitString[1][0]+2) / 60.0; // get the minutes and convert to degrees
		if(strchr(&splitString[1][0], 'S')) field_value_numerical = -field_value_numerical; // negative sign for S
		publishGPSSignal("gps_latitude", field_value_numerical, pipeline);
	}
	
	// 'gps_longitude'
	if(validString[2] && gpsConfig->gpsEnableSignal_gps_longitude)
	{
		memcpy(tmp, &splitString[2][0], 3); // pull out the degrees (always three digits xxx)
		field_value_numerical = (float)atoi(tmp); // turn degrees into float
		field_value_numerical += atof(&splitString[2][0]+3) / 60.0; // get the minutes and convert to degrees
		if(strchr(&splitString[2][0], 'W')) field_value_numerical = -field_value_numerical; // negative sign for W
		publishGPSSignal("gps_longitude", field_value_numerical, pipeline);
	}
	
	// 'gps_hdop'
	if(validString[3] && gpsConfig->gpsEnableSignal_gps_hdop)
	{
		field_value_numerical = atof(&splitString[3][0]);
		publishGPSSignal("gps_hdop", field_value_numerical, pipeline);
	}
	
	// 'gps_altitude'
	if(validString[4] && gpsConfig->gpsEnableSignal_gps_altitude)
	{
		field_value_numerical = atof(&splitString[4][0]);
		publishGPSSignal("gps_altitude", field_value_numerical, pipeline);
	}
	
	// 'gps_fix'
	if(validString[5] && gpsConfig->gpsEnableSignal_gps_fix)
	{
		field_value_numerical = (float)atoi(&splitString[5][0]);
		if(field_value_numerical < openxc::telitHE910::FIX_MAX_ENUM)
			publishGPSSignal("gps_fix", (char*)gps_fix_enum[(unsigned int)field_value_numerical], pipeline);
	}
	
	// 'gps_course'
	if(validString[6] && gpsConfig->gpsEnableSignal_gps_course)
	{
		field_value_numerical = atof(&splitString[6][0]);
		publishGPSSignal("gps_course", field_value_numerical, pipeline);
	}
	
	// 'gps_speed'
	if(validString[7] && gpsConfig->gpsEnableSignal_gps_speed)
	{
		field_value_numerical = atof(&splitString[7][0]);//* 100;	// there is a bug in the telit firmware that reports speed/100...
		publishGPSSignal("gps_speed", field_value_numerical, pipeline);
	}
	
	// 'gps_speed_knots'
	if(validString[8] && gpsConfig->gpsEnableSignal_gps_speed_knots)
	{
		field_value_numerical = atof(&splitString[8][0]);//* 100;	// there is a bug in the telit firmware that reports speed/100...
		publishGPSSignal("gps_speed_knots", field_value_numerical, pipeline);
	}
	
	// 'gps_date'
	if(validString[9] && gpsConfig->gpsEnableSignal_gps_date)
	{
		publishGPSSignal("gps_date", &splitString[9][0], pipeline);
	}
	
	// 'gps_nsat'
	if(validString[10] && gpsConfig->gpsEnableSignal_gps_nsat)
	{
		field_value_numerical = atof(&splitString[10][0]);
		publishGPSSignal("gps_nsat", field_value_numerical, pipeline);
	}
	
	fcn_exit:
	return rc;
}

// typedef for SERVER API return codes
typedef enum {
	None,
	Working,
	Success,
	Failed
} API_RETURN;

/*SERVER API*/

#define GET_FIRMWARE_SOCKET		1
#define POST_DATA_SOCKET		2
#define GET_COMMANDS_SOCKET		3

static API_RETURN serverPOSTdata(char* deviceId, char* host, char* data, unsigned int len) {

	static API_RETURN ret = None;
	static http::httpClient client;
	static http::HTTP_STATUS stat;
	static char header[256];
	static unsigned int state = 0;
	static const char* ctJSON = "application/json";
	static const char* ctPROTOBUF = "application/x-protobuf";
	
	switch(state)
	{
		default:
			state = 0;
		case 0:
			ret = Working;
			// compose the header for POST /data
			sprintf(header, "POST /api/%s/data HTTP/1.1\r\n"
					"Content-Length: %u\r\n"
					"Content-Type: %s\r\n"
					"Host: %s\r\n"
					"Connection: Keep-Alive\r\n\r\n", deviceId, len, getConfiguration()->payloadFormat == PayloadFormat::PROTOBUF ? ctPROTOBUF : ctJSON, host);
			// configure the HTTP client
			client = http::httpClient();
			client.socketNumber = POST_DATA_SOCKET;
			client.requestHeader = header;
			client.requestBody = data;
			client.requestBodySize = len;
			client.cbGetRequestData = NULL;
			client.cbPutResponseData = NULL;
			client.sendSocketData = &openxc::telitHE910::writeSocket;
			client.isReceiveDataAvailable = &openxc::telitHE910::isSocketDataAvailable;
			client.receiveSocketData = &openxc::telitHE910::readSocket;
			state = 1;
			break;
			
		case 1:
			// run the HTTP client
			switch(client.execute())
			{
				case http::HTTP_READY:
				case http::HTTP_SENDING_REQUEST_HEADER:
				case http::HTTP_SENDING_REQUEST_BODY:
				case http::HTTP_RECEIVING_RESPONSE:
					// nothing to do while client is in progress
					break;
				case http::HTTP_COMPLETE:
					ret = Success;
					state = 0;
					break;
				case http::HTTP_FAILED:
					ret = Failed;
					state = 0;
					break;
			}
			break;
	}
	
	return ret;

}

static int cbHeaderComplete(http_parser* parser) {
	if(parser->status_code == 200)
		SoftReset();
	else
		return 1;
}

static int cbOnStatus(http_parser* parser, const char* at, size_t length) {
	if(parser->status_code == 204)
		return 1; // cause parser to exit immediately (we're throwing an error to force the client to abort)
	else
		return 0;
}

static API_RETURN serverGETfirmware(char* deviceId, char* host) {

	static API_RETURN ret = None;
	static http::httpClient client;
	static http::HTTP_STATUS stat;
	static char header[256];
	static unsigned int state = 0;
	
	switch(state)
	{
		default:
			state = 0;
		case 0:
			ret = Working;
			// compose the header for GET /firmware
			sprintf(header, "GET /api/%s/firmware HTTP/1.1\r\n"
					"If-None-Match: \"%s\"\r\n"
					"Host: %s\r\n"
					"Connection: Keep-Alive\r\n\r\n", deviceId, getConfiguration()->flashHash, host);
			// configure the HTTP client
			client = http::httpClient();
			client.socketNumber = GET_FIRMWARE_SOCKET;
			client.requestHeader = header;
			client.requestBody = NULL;
			client.requestBodySize = 0;
			client.cbGetRequestData = NULL;
			client.parser_settings.on_headers_complete = &cbHeaderComplete;
			client.parser_settings.on_status = &cbOnStatus;
			client.cbPutResponseData = NULL;
			client.sendSocketData = &openxc::telitHE910::writeSocket;
			client.isReceiveDataAvailable = &openxc::telitHE910::isSocketDataAvailable;
			client.receiveSocketData = &readSocketOne;
			state = 1;
			break;
			
		case 1:
			// run the HTTP client
			switch(client.execute())
			{
				case http::HTTP_READY:
				case http::HTTP_SENDING_REQUEST_HEADER:
				case http::HTTP_SENDING_REQUEST_BODY:
				case http::HTTP_RECEIVING_RESPONSE:
					// nothing to do while client is in progress
					break;
				case http::HTTP_COMPLETE:
					ret = Success;
					state = 0;
					break;
				case http::HTTP_FAILED:
					ret = Failed;
					state = 0;
					break;
			}
			break;
	}
	
	return ret;

}

static int cbOnBody(http_parser* parser, const char* at, size_t length) {
	unsigned int i = 0;
	for(i = 0; i < length && pCommandBuffer < (commandBuffer + commandBufferSize); ++i)
		*pCommandBuffer++ = *at++;
	return 0;
}

static API_RETURN serverGETcommands(char* deviceId, char* host) {
	
	static API_RETURN ret = None;
	static http::httpClient client;
	static http::HTTP_STATUS stat;
	static char header[256];
	static unsigned int state = 0;
	
	switch(state)
	{
		default:
			state = 0;
		case 0:
			ret = Working;
			// compose the header for GET /firmware
			sprintf(header, "GET /api/%s/configure HTTP/1.1\r\n"
					"Host: %s\r\n"
					"Connection: Keep-Alive\r\n\r\n", deviceId, host);
			// configure the HTTP client
			client = http::httpClient();
			client.socketNumber = GET_COMMANDS_SOCKET;
			client.requestHeader = header;
			client.requestBody = NULL;
			client.requestBodySize = 0;
			client.cbGetRequestData = NULL;
			client.parser_settings.on_body = cbOnBody;
			client.cbPutResponseData = NULL;
			client.sendSocketData = &openxc::telitHE910::writeSocket;
			client.isReceiveDataAvailable = &openxc::telitHE910::isSocketDataAvailable;
			client.receiveSocketData = &openxc::telitHE910::readSocket;
			state = 1;
			break;
			
		case 1:
			// run the HTTP client
			switch(client.execute())
			{
				case http::HTTP_READY:
				case http::HTTP_SENDING_REQUEST_HEADER:
				case http::HTTP_SENDING_REQUEST_BODY:
				case http::HTTP_RECEIVING_RESPONSE:
					// nothing to do while client is in progress
					break;
				case http::HTTP_COMPLETE:
					ret = Success;
					state = 0;
					break;
				case http::HTTP_FAILED:
					ret = Failed;
					state = 0;
					break;
			}
			break;
	}
	
	return ret;
	
}

/*EXTERNAL TASK CALLS*/

#define GET_FIRMWARE_INTERVAL	600000
#define GET_COMMANDS_INTERVAL	10000
#define POST_DATA_MAX_INTERVAL	5000

void openxc::telitHE910::firmwareCheck(TelitDevice* device) {
	
	static unsigned int state = 0;
	static bool first = true;
	static unsigned long timer = 0xFFFF;

	switch(state)
	{
		default:
			state = 0;
		case 0:
			// check interval if it's not our first time
			if(!first)
			{
				// request upgrade on interval
				if(uptimeMs() - timer > GET_FIRMWARE_INTERVAL)
				{
					timer = uptimeMs();
					state = 1;
				}
			}
			else
			{
				first = false;
				timer = uptimeMs();
				state = 1;
			}
			break;
			
		case 1:
			if(!isSocketOpen(GET_FIRMWARE_SOCKET))
			{
				if(!openSocket(GET_FIRMWARE_SOCKET, device->config.serverConnectSettings))
				{
					state = 0;
				}
				else
				{
					state = 2;
				}
			}
			else
			{
				state = 2;
			}
			break;
			
		case 2:
			// call the GETfirmware API
			switch(serverGETfirmware(device->deviceId, device->config.serverConnectSettings.host))
			{
				case None:
				case Working:
					// if we are working, do nothing
					break;
				default:
				case Success:
				case Failed:
					// whether we succeeded or failed (or got lost), there's nothing we can do except go around again
					// close the socket
					// either we got a 200 OK, in which case we went for reset
					// or we got a 204/error, in which case we have a dangling transaction
					closeSocket(GET_FIRMWARE_SOCKET);
					//timer = uptimeMs();
					state = 0;
					break;
			}
			break;
	}

	return;
	
}

void openxc::telitHE910::flushDataBuffer(TelitDevice* device) {

	static bool first = true;
	static unsigned int state = 0;
	static unsigned int lastFlushTime = 0;
	static const unsigned int flushSize = 2048;
	static char postBuffer[sendBufferSize + 64]; // extra space needed for root record
	static unsigned int byteCount = 0;
	unsigned int i = 0;
	
	switch(state)
	{
		default:
			state = 0;
		case 0:
			// conditions to flush the outgoing data buffer
				// a) buffer has reached the flush size
				// b) buffer has not been flushed for the time period POST_DATA_MAX_INTERVAL (and there is something in there)
				// c) minimum amount of time has passed since lastFlushTime (depends on socket status) - not yet implemented
			if(!first)
			{
				if( (pSendBuffer - sendBuffer) >= flushSize || 
					((uptimeMs() - lastFlushTime >= POST_DATA_MAX_INTERVAL) && (pSendBuffer != sendBuffer)) )
				{
					lastFlushTime = uptimeMs();
					state = 1;
				}
			}
			else
			{
				first = false;
				lastFlushTime = uptimeMs();
				state = 1;
			}
			break;
			
		case 1:
			// ensure we have an open TCP/IP socket
			if(!isSocketOpen(POST_DATA_SOCKET))
			{
				if(!openSocket(POST_DATA_SOCKET, device->config.serverConnectSettings))
				{
					state = 0;
				}
				else
				{
					state = 2;
				}
			}
			else
			{
				state = 2;
			}
			break;
			
		case 2:
			
			switch(getConfiguration()->payloadFormat)
			{
				case PayloadFormat::JSON:
				
					// pre-populate the send buffer with root record
					memcpy(postBuffer, "{\"records\":[", 12);
					byteCount = 12;
					
					// get all bytes from the send buffer (so we have room to fill it again as we POST)
					memcpy(postBuffer+byteCount, sendBuffer, pSendBuffer - sendBuffer);
					byteCount += pSendBuffer - sendBuffer;
					pSendBuffer = sendBuffer;
					
					// replace the nulls with commas to create a JSON array
					for(i = 0; i < byteCount; ++i)
					{
						if(postBuffer[i] == '\0')
							postBuffer[i] = ',';
					}
					
					// back over the trailing comma
					if(postBuffer[byteCount-1] == ',')
						byteCount--;
					
					// end the array
					postBuffer[byteCount++] = ']';
					postBuffer[byteCount++] = '}';
				
					break;
					
				case PayloadFormat::PROTOBUF:
				
					// get all bytes from the send buffer (so we have room to fill it again as we POST)
					byteCount = 0;
					memcpy(postBuffer+byteCount, sendBuffer, pSendBuffer - sendBuffer);
					byteCount += pSendBuffer - sendBuffer;
					pSendBuffer = sendBuffer;
				
					break;
			}
			
			state = 3;
			
			break;
			
		case 3:
		
			// call the POSTdata API
			switch(serverPOSTdata(device->deviceId, device->config.serverConnectSettings.host, postBuffer, byteCount))
			{
				case None:
				case Working:
					// if we are working, do nothing
					break;
				default:
				case Success:
					state = 0;
					break;
				case Failed:
					//lastFlushTime = uptimeMs();
					state = 0;
					closeSocket(POST_DATA_SOCKET);
					break;
			}
			break;
	}
	
	return;

}

void openxc::telitHE910::commandCheck(TelitDevice* device) {

	static unsigned int state = 0;
	static bool first = true;
	static unsigned long timer = 0xFFFF;

	switch(state)
	{
		default:
			state = 0;
		case 0:
			// check interval if it's not our first time
			if(!first)
			{
				// request commands on interval
				if(uptimeMs() - timer > GET_COMMANDS_INTERVAL)
				{
					timer = uptimeMs();
					state = 1;
				}
			}
			else
			{
				first = false;
				timer = uptimeMs();
				state = 1;
			}
			break;
			
		case 1:
			if(!isSocketOpen(GET_COMMANDS_SOCKET))
			{
				if(!openSocket(GET_COMMANDS_SOCKET, device->config.serverConnectSettings))
				{
					state = 0;
				}
				else
				{
					state = 2;
				}
			}
			else
			{
				state = 2;
			}
			break;
			
		case 2:
			// call the GETconfigure API
			switch(serverGETcommands(device->deviceId, device->config.serverConnectSettings.host))
			{
				case None:
				case Working:
					// if we are working, do nothing
					break;
				default:
				case Success:
					// send the contents of commandBuffer to the command handler
					#warning "MG hack a null onto the complete command until we can have the server do it"
					*pCommandBuffer++ = '\0'; // not just null-delimited but null-terminated...hack only works for a single command
					commands::handleIncomingMessage(commandBuffer, pCommandBuffer - commandBuffer, &(telitDevice->descriptor));
					pCommandBuffer = commandBuffer;
					state = 0;
					break;
				case Failed:
					pCommandBuffer = commandBuffer;
					closeSocket(GET_COMMANDS_SOCKET);
					state = 0;
					break;
			}
			break;
	}

	return;

}

/*PIPELINE*/

/*
 * Public:
 *
 * During pipeline::process(), this function will be called. The TelitDevice contains a QUEUE 
 * that already has unprocessed data from previous calls to pipeline::publish. Here we simply need
 * to flush the send QUEUE through a TCP/IP socket by constructing an HTTP POST. We can do this on 
  * every call, or only when the QUEUE reaches a certain size and/or age.
 */
void openxc::telitHE910::processSendQueue(TelitDevice* device) {

	// The pipeline is made up of a limited 256 byte queue_uint8_t defined in bytebuffer and emqueue
	// The job of a processSendQueue device method is to empty the pipeline
	// Normally we would just dump that queue straight out of the device (UART, USB)
	// Right now, we are reformatting the pipeline data into a JSON array and POSTing via httpClient
	// However, it might become necessary to instead feed a larger buffer that flushes more slowly, 
	 // so as to minimize the overhead (both time and data) incurred by the HTTP transactions
	// Thus the QUEUE is buffering data between successive iterations of firmwareLoop(), and 
	// our "sendBuffer" will buffer up multiple QUEUEs before flushing on a time and/or data watermark.

	// pop bytes from the device send queue (stop short of sendBuffer overflow)
	while(!QUEUE_EMPTY(uint8_t, &device->sendQueue) && (pSendBuffer - sendBuffer) < sendBufferSize)
	{
        *pSendBuffer++ = QUEUE_POP(uint8_t, &device->sendQueue);
    }

	return;

}