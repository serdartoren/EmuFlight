/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

#ifdef USE_SERIALRX_CRSF

#include "build/build_config.h"
#include "build/debug.h"

#include "common/crc.h"
#include "common/maths.h"
#include "common/utils.h"

#include "pg/rx.h"

#include "drivers/serial.h"
#include "drivers/serial_uart.h"
#include "drivers/system.h"
#include "drivers/time.h"

#include "io/serial.h"

#include "rx/rx.h"
#include "rx/crsf.h"

#include "telemetry/crsf.h"

#define CRSF_TIME_NEEDED_PER_FRAME_US   1100 // 700 ms + 400 ms for potential ad-hoc request
#define CRSF_TIME_BETWEEN_FRAMES_US     6667 // At fastest, frames are sent by the transmitter every 6.667 milliseconds, 150 Hz

#define CRSF_DIGITAL_CHANNEL_MIN 172
#define CRSF_DIGITAL_CHANNEL_MAX 1811

#define CRSF_PAYLOAD_OFFSET offsetof(crsfFrameDef_t, type)

#define CRSF_LINK_STATUS_UPDATE_TIMEOUT_US  250000 // 250ms, 4 Hz mode 1 telemetry

#define CRSF_FRAME_ERROR_COUNT_THRESHOLD    100

STATIC_UNIT_TESTED bool crsfFrameDone = false;
STATIC_UNIT_TESTED crsfFrame_t crsfFrame;
STATIC_UNIT_TESTED crsfFrame_t crsfChannelDataFrame;
STATIC_UNIT_TESTED uint32_t crsfChannelData[CRSF_MAX_CHANNEL];

static serialPort_t *serialPort;
static uint32_t crsfFrameStartAtUs = 0;
static uint8_t telemetryBuf[CRSF_FRAME_SIZE_MAX];
static uint8_t telemetryBufLen = 0;

/*
 * CRSF protocol
 *
 * CRSF protocol uses a single wire half duplex uart connection.
 * The master sends one frame every 4ms and the slave replies between two frames from the master.
 *
 * 420000 baud
 * not inverted
 * 8 Bit
 * 1 Stop bit
 * Big endian
 * 420000 bit/s = 46667 byte/s (including stop bit) = 21.43us per byte
 * Max frame size is 64 bytes
 * A 64 byte frame plus 1 sync byte can be transmitted in 1393 microseconds.
 *
 * CRSF_TIME_NEEDED_PER_FRAME_US is set conservatively at 1500 microseconds
 *
 * Every frame has the structure:
 * <Device address><Frame length><Type><Payload><CRC>
 *
 * Device address: (uint8_t)
 * Frame length:   length in  bytes including Type (uint8_t)
 * Type:           (uint8_t)
 * CRC:            (uint8_t)
 *
 */

struct crsfPayloadRcChannelsPacked_s {
    // 176 bits of data (11 bits per channel * 16 channels) = 22 bytes.
    unsigned int chan0 : 11;
    unsigned int chan1 : 11;
    unsigned int chan2 : 11;
    unsigned int chan3 : 11;
    unsigned int chan4 : 11;
    unsigned int chan5 : 11;
    unsigned int chan6 : 11;
    unsigned int chan7 : 11;
    unsigned int chan8 : 11;
    unsigned int chan9 : 11;
    unsigned int chan10 : 11;
    unsigned int chan11 : 11;
    unsigned int chan12 : 11;
    unsigned int chan13 : 11;
    unsigned int chan14 : 11;
    unsigned int chan15 : 11;
} __attribute__ ((__packed__));

typedef struct crsfPayloadRcChannelsPacked_s crsfPayloadRcChannelsPacked_t;

// first 5 bits in the first byte hold the first channel packed
// remaining bits hold the channel data in 11-bit format
#define CRSF_SUBSET_RC_CHANNELS_PACKED_RESOLUTION                  11
#define CRSF_SUBSET_RC_CHANNELS_PACKED_MASK                        0x07FF
#define CRSF_SUBSET_RC_CHANNELS_PACKED_STARTING_CHANNEL_RESOLUTION 5
#define CRSF_SUBSET_RC_CHANNELS_PACKED_STARTING_CHANNEL_MASK       0x1F

#if defined(USE_CRSF_LINK_STATISTICS)
/*
 * 0x14 Link statistics
 * Payload:
 *
 * uint8_t Uplink RSSI Ant. 1 ( dBm * -1 )
 * uint8_t Uplink RSSI Ant. 2 ( dBm * -1 )
 * uint8_t Uplink Package success rate / Link quality ( % )
 * int8_t Uplink SNR ( db )
 * uint8_t Diversity active antenna ( enum ant. 1 = 0, ant. 2 )
 * uint8_t RF Mode ( enum 4fps = 0 , 50fps, 150hz)
 * uint8_t Uplink TX Power ( enum 0mW = 0, 10mW, 25 mW, 100 mW, 500 mW, 1000 mW, 2000mW )
 * uint8_t Downlink RSSI ( dBm * -1 )
 * uint8_t Downlink package success rate / Link quality ( % )
 * int8_t Downlink SNR ( db )
 * Uplink is the connection from the ground to the UAV and downlink the opposite direction.
 */

/*
 * 0x1C Link statistics RX
 * Payload:
 *
 * uint8_t Downlink RSSI ( dBm * -1 )
 * uint8_t Downlink RSSI ( % )
 * uint8_t Downlink Package success rate / Link quality ( % )
 * int8_t Downlink SNR ( db )
 * uint8_t Uplink RF Power ( db )
 */

/*
 * 0x1D Link statistics TX
 * Payload:
 *
 * uint8_t Uplink RSSI ( dBm * -1 )
 * uint8_t Uplink RSSI ( % )
 * uint8_t Uplink Package success rate / Link quality ( % )
 * int8_t Uplink SNR ( db )
 * uint8_t Downlink RF Power ( db )
 * uint8_t Uplink FPS ( FPS / 10 )
 */

typedef struct crsfPayloadLinkstatistics_s {
    uint8_t uplink_RSSI_1;
    uint8_t uplink_RSSI_2;
    uint8_t uplink_Link_quality;
    int8_t uplink_SNR;
    uint8_t active_antenna;
    uint8_t rf_Mode;
    uint8_t uplink_TX_Power;
    uint8_t downlink_RSSI;
    uint8_t downlink_Link_quality;
    int8_t downlink_SNR;
} crsfLinkStatistics_t;

#if defined(USE_CRSF_V3)
typedef struct crsfPayloadLinkstatisticsRx_s {
    uint8_t downlink_RSSI_1;
    uint8_t downlink_RSSI_1_percentage;
    uint8_t downlink_Link_quality;
    int8_t downlink_SNR;
    uint8_t uplink_power;
} crsfLinkStatisticsRx_t;

typedef struct crsfPayloadLinkstatisticsTx_s {
    uint8_t uplink_RSSI;
    uint8_t uplink_RSSI_percentage;
    uint8_t uplink_Link_quality;
    int8_t uplink_SNR;
    uint8_t downlink_power;
    uint8_t uplink_FPS;
} crsfLinkStatisticsTx_t;
#endif

static timeUs_t lastLinkStatisticsFrameUs;

static void handleCrsfLinkStatisticsFrame(const crsfLinkStatistics_t* statsPtr, timeUs_t currentTimeUs) {
    const crsfLinkStatistics_t stats = *statsPtr;
    CRSFsetLQ(stats.uplink_Link_quality);
    CRSFsetRFMode(stats.rf_Mode);
    CRSFsetSnR(stats.downlink_SNR);
    CRSFsetTXPower(stats.uplink_TX_Power);
    if(stats.uplink_RSSI_1 == 0)
        CRSFsetRSSI(stats.uplink_RSSI_2);
    else if(stats.uplink_RSSI_2 == 0)
        CRSFsetRSSI(stats.uplink_RSSI_1);
    else {
        uint8_t rssimin = MIN(stats.uplink_RSSI_1, stats.uplink_RSSI_2) * -1;
        CRSFsetRSSI(rssimin);
    }
}

#if defined(USE_CRSF_V3)
static void handleCrsfLinkStatisticsTxFrame(const crsfLinkStatisticsTx_t* statsPtr, timeUs_t currentTimeUs)
{
    const crsfLinkStatisticsTx_t stats = *statsPtr;
    lastLinkStatisticsFrameUs = currentTimeUs;
    int16_t rssiDbm = -1 * stats.uplink_RSSI;
    if (rssiSource == RSSI_SOURCE_RX_PROTOCOL_CRSF) {
        const uint16_t rssiPercentScaled = stats.uplink_RSSI_percentage;
        setRssi(rssiPercentScaled, RSSI_SOURCE_RX_PROTOCOL_CRSF);
    }
#ifdef USE_RX_RSSI_DBM
    if (rxConfig()->crsf_use_rx_snr) {
        rssiDbm = stats.uplink_SNR;
    }
    setRssiDbm(rssiDbm, RSSI_SOURCE_RX_PROTOCOL_CRSF);
#endif

#ifdef USE_RX_LINK_QUALITY_INFO
    if (linkQualitySource == LQ_SOURCE_RX_PROTOCOL_CRSF) {
        setLinkQualityDirect(stats.uplink_Link_quality);
    }
#endif
}
#endif
#endif

STATIC_UNIT_TESTED uint8_t crsfFrameCRC(void) {
    // CRC includes type and payload
    uint8_t crc = crc8_dvb_s2(0, crsfFrame.frame.type);
    for (int ii = 0; ii < crsfFrame.frame.frameLength - CRSF_FRAME_LENGTH_TYPE_CRC; ++ii) {
        crc = crc8_dvb_s2(crc, crsfFrame.frame.payload[ii]);
    }
    return crc;
}

STATIC_UNIT_TESTED uint8_t crsfFrameCmdCRC(void)
{
    // CRC includes type and payload
    uint8_t crc = crc8_poly_0xba(0, crsfFrame.frame.type);
    for (int ii = 0; ii < crsfFrame.frame.frameLength - CRSF_FRAME_LENGTH_TYPE_CRC - 1; ++ii) {
        crc = crc8_poly_0xba(crc, crsfFrame.frame.payload[ii]);
    }
    return crc;
}

// Receive ISR callback, called back from serial port
STATIC_UNIT_TESTED void crsfDataReceive(uint16_t c, void *data) {
    UNUSED(data);
    static uint8_t crsfFramePosition = 0;
#if defined(USE_CRSF_V3)
    static uint32_t crsfFrameErrorCnt = 0;
#endif
    const uint32_t currentTimeUs = micros();
#ifdef DEBUG_CRSF_PACKETS
    debug[2] = currentTimeUs - crsfFrameStartAtUs;
#endif
    if (currentTimeUs > crsfFrameStartAtUs + CRSF_TIME_NEEDED_PER_FRAME_US) {
        // We've received a character after max time needed to complete a frame,
        // so this must be the start of a new frame.
        crsfFramePosition = 0;
    }
    if (crsfFramePosition == 0) {
        crsfFrameStartAtUs = currentTimeUs;
    }
    // assume frame is 5 bytes long until we have received the frame length
    // full frame length includes the length of the address and framelength fields
    const int fullFrameLength = crsfFramePosition < 3 ? 5 : crsfFrame.frame.frameLength + CRSF_FRAME_LENGTH_ADDRESS + CRSF_FRAME_LENGTH_FRAMELENGTH;
    if (crsfFramePosition < fullFrameLength) {
        crsfFrame.bytes[crsfFramePosition++] = (uint8_t)c;
        if (crsfFramePosition >= fullFrameLength) {
            crsfFramePosition = 0;
            const uint8_t crc = crsfFrameCRC();
            if (crc == crsfFrame.bytes[fullFrameLength - 1]) {
#if defined(USE_CRSF_V3)
                crsfFrameErrorCnt = 0;
#endif
                switch (crsfFrame.frame.type) {
                    case CRSF_FRAMETYPE_RC_CHANNELS_PACKED:
                    case CRSF_FRAMETYPE_SUBSET_RC_CHANNELS_PACKED:
                        if (crsfFrame.frame.deviceAddress == CRSF_ADDRESS_FLIGHT_CONTROLLER) {
                            crsfFrameDone = true;
                            memcpy(&crsfChannelDataFrame, &crsfFrame, sizeof(crsfFrame));
                        }
                        break;
#if defined(USE_TELEMETRY_CRSF) && defined(USE_MSP_OVER_TELEMETRY)
                    case CRSF_FRAMETYPE_MSP_REQ:
                    case CRSF_FRAMETYPE_MSP_WRITE: {
                        uint8_t *frameStart = (uint8_t *)&crsfFrame.frame.payload + CRSF_FRAME_ORIGIN_DEST_SIZE;
                        if (bufferCrsfMspFrame(frameStart, CRSF_FRAME_RX_MSP_FRAME_SIZE)) {
                            crsfScheduleMspResponse();
                        }
                        break;
                    }
#endif
#if defined(USE_CRSF_CMS_TELEMETRY)
                    case CRSF_FRAMETYPE_DEVICE_PING:
                        crsfScheduleDeviceInfoResponse();
                        break;
                    case CRSF_FRAMETYPE_DISPLAYPORT_CMD: {
                        uint8_t *frameStart = (uint8_t *)&crsfFrame.frame.payload + CRSF_FRAME_ORIGIN_DEST_SIZE;
                        crsfProcessDisplayPortCmd(frameStart);
                        break;
                    }
#endif
#if defined(USE_TELEMETRY_CRSF)
                    case CRSF_FRAMETYPE_LINK_STATISTICS: {
                         // if to FC and 10 bytes + CRSF_FRAME_ORIGIN_DEST_SIZE
                         if ((rssiSource == RSSI_SOURCE_RX_PROTOCOL_CRSF) &&
                             (crsfFrame.frame.frameLength == CRSF_FRAME_ORIGIN_DEST_SIZE + CRSF_FRAME_LINK_STATISTICS_PAYLOAD_SIZE)) {
                             const crsfLinkStatistics_t* statsFrame = (const crsfLinkStatistics_t*)&crsfFrame.frame.payload;
                             handleCrsfLinkStatisticsFrame(statsFrame, currentTimeUs);
                         }
                        break;
                    }
#if defined(USE_CRSF_V3)
                    case CRSF_FRAMETYPE_LINK_STATISTICS_RX: {
                        break;
                    }
                    case CRSF_FRAMETYPE_LINK_STATISTICS_TX: {
                         if ((rssiSource == RSSI_SOURCE_RX_PROTOCOL_CRSF) &&
                             (crsfFrame.frame.deviceAddress == CRSF_ADDRESS_FLIGHT_CONTROLLER) &&
                             (crsfFrame.frame.frameLength == CRSF_FRAME_ORIGIN_DEST_SIZE + CRSF_FRAME_LINK_STATISTICS_PAYLOAD_SIZE)) {
                             const crsfLinkStatisticsTx_t* statsFrame = (const crsfLinkStatisticsTx_t*)&crsfFrame.frame.payload;
                             handleCrsfLinkStatisticsTxFrame(statsFrame, currentTimeUs);
                         }
                        break;
                    }
#endif
#endif
#if defined(USE_CRSF_V3)
                    case CRSF_FRAMETYPE_COMMAND:
                        if ((crsfFrame.bytes[fullFrameLength - 2] == crsfFrameCmdCRC()) &&
                        (crsfFrame.bytes[3] == CRSF_ADDRESS_FLIGHT_CONTROLLER)) {
                        crsfProcessCommand(crsfFrame.frame.payload + CRSF_FRAME_ORIGIN_DEST_SIZE);
                        }
                        break;
#endif
                    default:
                        break;
                }
            }
            else {
#if defined(USE_CRSF_V3)
                if (crsfFrameErrorCnt < CRSF_FRAME_ERROR_COUNT_THRESHOLD)
                    crsfFrameErrorCnt++;
#endif
            }
        }
        else {
#if defined(USE_CRSF_V3)
            if (crsfFrameErrorCnt < CRSF_FRAME_ERROR_COUNT_THRESHOLD)
                crsfFrameErrorCnt++;
#endif
        }
#if defined(USE_CRSF_V3)
        if (crsfFrameErrorCnt >= CRSF_FRAME_ERROR_COUNT_THRESHOLD) {
            // fall back to default speed if speed mismatch detected
            setCrsfDefaultSpeed();
            crsfFrameErrorCnt = 0;
        }
#endif
    }
}

STATIC_UNIT_TESTED uint8_t crsfFrameStatus(rxRuntimeConfig_t *rxRuntimeConfig) {
    UNUSED(rxRuntimeConfig);
    if (crsfFrameDone) {
        crsfFrameDone = false;

        // unpack the RC channels
        if( crsfChannelDataFrame.frame.type == CRSF_FRAMETYPE_RC_CHANNELS_PACKED ) {
             // use ordinary RC frame structure (0x16)
             const crsfPayloadRcChannelsPacked_t* const rcChannels = (crsfPayloadRcChannelsPacked_t*)&crsfChannelDataFrame.frame.payload;
             crsfChannelData[0] = rcChannels->chan0;
             crsfChannelData[1] = rcChannels->chan1;
             crsfChannelData[2] = rcChannels->chan2;
             crsfChannelData[3] = rcChannels->chan3;
             crsfChannelData[4] = rcChannels->chan4;
             crsfChannelData[5] = rcChannels->chan5;
             crsfChannelData[6] = rcChannels->chan6;
             crsfChannelData[7] = rcChannels->chan7;
             crsfChannelData[8] = rcChannels->chan8;
             crsfChannelData[9] = rcChannels->chan9;
             crsfChannelData[10] = rcChannels->chan10;
             crsfChannelData[11] = rcChannels->chan11;
             crsfChannelData[12] = rcChannels->chan12;
             crsfChannelData[13] = rcChannels->chan13;
             crsfChannelData[14] = rcChannels->chan14;
             crsfChannelData[15] = rcChannels->chan15;
         }
 #if defined(USE_CRSF_V3)
         else {
             // use subset RC frame structure (0x17)
             uint8_t n;
             uint8_t readByte;
             uint8_t readByteIndex = 0;
             uint8_t bitsMerged = 0;
             uint32_t readValue = 0;
             uint8_t startChannel = 0xFF;
             const uint8_t *payload = crsfChannelDataFrame.frame.payload;
             uint8_t numOfChannels = ((crsfChannelDataFrame.frame.frameLength - CRSF_FRAME_LENGTH_TYPE_CRC) * 8 - CRSF_SUBSET_RC_CHANNELS_PACKED_STARTING_CHANNEL_RESOLUTION) / CRSF_SUBSET_RC_CHANNELS_PACKED_RESOLUTION;
             for (n = 0; n < numOfChannels; n++) {
                 while(bitsMerged < CRSF_SUBSET_RC_CHANNELS_PACKED_RESOLUTION) {
                     readByte = payload[readByteIndex++];
                     if(startChannel == 0xFF) {
                         // get the startChannel
                         startChannel = readByte & CRSF_SUBSET_RC_CHANNELS_PACKED_STARTING_CHANNEL_MASK;
                         readByte >>= CRSF_SUBSET_RC_CHANNELS_PACKED_STARTING_CHANNEL_RESOLUTION;
                         readValue |= ((uint32_t) readByte) << bitsMerged;
                         bitsMerged += 8 - CRSF_SUBSET_RC_CHANNELS_PACKED_STARTING_CHANNEL_RESOLUTION;
                     }
                     else {
                         readValue |= ((uint32_t) readByte) << bitsMerged;
                         bitsMerged += 8;
                     }
                 }
                 crsfChannelData[startChannel + n] = readValue & CRSF_SUBSET_RC_CHANNELS_PACKED_MASK;
                 readValue >>= CRSF_SUBSET_RC_CHANNELS_PACKED_RESOLUTION;
                 bitsMerged -= CRSF_SUBSET_RC_CHANNELS_PACKED_RESOLUTION;
             }
         }
 #endif
        return RX_FRAME_COMPLETE;
    }
    return RX_FRAME_PENDING;
}

STATIC_UNIT_TESTED uint16_t crsfReadRawRC(const rxRuntimeConfig_t *rxRuntimeConfig, uint8_t chan) {
    UNUSED(rxRuntimeConfig);
    return (crsfChannelData[chan] - 992) * 5 / 8 + 1500;
}

void crsfRxWriteTelemetryData(const void *data, int len) {
    len = MIN(len, (int)sizeof(telemetryBuf));
    memcpy(telemetryBuf, data, len);
    telemetryBufLen = len;
}

void crsfRxSendTelemetryData(void) {
    // if there is telemetry data to write
    if (telemetryBufLen > 0) {
        serialWriteBuf(serialPort, telemetryBuf, telemetryBufLen);
        telemetryBufLen = 0; // reset telemetry buffer
    }
}

bool crsfRxInit(const rxConfig_t *rxConfig, rxRuntimeConfig_t *rxRuntimeConfig) {
    for (int ii = 0; ii < CRSF_MAX_CHANNEL; ++ii) {
        crsfChannelData[ii] = (16 * rxConfig->midrc) / 10 - 1408;
    }
    rxRuntimeConfig->channelCount = CRSF_MAX_CHANNEL;
    rxRuntimeConfig->rxRefreshRate = CRSF_TIME_BETWEEN_FRAMES_US; //!!TODO this needs checking
    rxRuntimeConfig->rcReadRawFn = crsfReadRawRC;
    rxRuntimeConfig->rcFrameStatusFn = crsfFrameStatus;
    const serialPortConfig_t *portConfig = findSerialPortConfig(FUNCTION_RX_SERIAL);
    if (!portConfig) {
        return false;
    }
    serialPort = openSerialPort(portConfig->identifier,
                                FUNCTION_RX_SERIAL,
                                crsfDataReceive,
                                NULL,
                                CRSF_BAUDRATE,
                                CRSF_PORT_MODE,
                                CRSF_PORT_OPTIONS | (rxConfig->serialrx_inverted ? SERIAL_INVERTED : 0)
                               );
    return serialPort != NULL;
}

#if defined(USE_CRSF_V3)
void crsfRxUpdateBaudrate(uint32_t baudrate)
{
    serialSetBaudRate(serialPort, baudrate);
}
#endif

bool crsfRxIsActive(void) {
    return serialPort != NULL;
}
#endif
