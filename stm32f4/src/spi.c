/*
This file is part of Hackflight.

Hackflight is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

Hackflight is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
Hackflight. If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <core/constrain.h>

#include "platform.h"

#include "atomic.h"
#include "spi.h"
#include "dma_reqmap.h"
#include "exti.h"
#include "flash.h"
#include "io.h"
#include "rcc.h"
#include "nvic.h"
#include "resource.h"
#include "systemdev.h"

#define SPI_PREINIT_COUNT 16

#define MAX_SPI_PIN_SEL 2

#define BUS_SPI_FREE   0x0
#define BUS_SPI_LOCKED 0x4

#define SPI_IO_AF_CFG      IO_CONFIG(GPIO_Mode_AF,  GPIO_Speed_50MHz, \
        GPIO_OType_PP, GPIO_PuPd_NOPULL)
#define SPI_IO_AF_SCK_CFG  IO_CONFIG(GPIO_Mode_AF,  GPIO_Speed_50MHz, \
        GPIO_OType_PP, GPIO_PuPd_DOWN)
#define SPI_IO_AF_MISO_CFG IO_CONFIG(GPIO_Mode_AF,  GPIO_Speed_50MHz, \
        GPIO_OType_PP, GPIO_PuPd_UP)

#define SPIDEV_COUNT 3

// Macros to convert between CLI bus number and SPIDevice.
#define SPI_CFG_TO_DEV(x)   ((x) - 1)
#define SPI_DEV_TO_CFG(x)   ((x) + 1)

// Work around different check routines in the libraries for different MCU types
#define CHECK_SPI_RX_DATA_AVAILABLE(instance) LL_SPI_IsActiveFlag_RXNE(instance)
#define SPI_RX_DATA_REGISTER(base) ((base)->DR)

typedef struct spiPinConfig_s {
    ioTag_t ioTagSck;
    ioTag_t ioTagMiso;
    ioTag_t ioTagMosi;
    int8_t txDmaopt;
    int8_t rxDmaopt;
} spiPinConfig_t;

// De facto standard mode
// See https://en.wikipedia.org/wiki/Serial_Peripheral_Interface
//
// Mode CPOL CPHA
//  0    0    0
//  1    0    1
//  2    1    0
//  3    1    1
typedef enum {
    SPI_MODE0_POL_LOW_EDGE_1ST = 0,
    SPI_MODE1_POL_LOW_EDGE_2ND,
    SPI_MODE2_POL_HIGH_EDGE_1ST,
    SPI_MODE3_POL_HIGH_EDGE_2ND
} SPIMode_e;

typedef struct spiPinDef_s {
    ioTag_t pin;
} spiPinDef_t;

typedef struct spiHardware_s {
    SPIDevice device;
    SPI_TypeDef *reg;
    spiPinDef_t sckPins[MAX_SPI_PIN_SEL];
    spiPinDef_t misoPins[MAX_SPI_PIN_SEL];
    spiPinDef_t mosiPins[MAX_SPI_PIN_SEL];
    uint8_t af;
    rccPeriphTag_t rcc;
    uint8_t dmaIrqHandler;
} spiHardware_t;

typedef struct SPIDevice_s {
    SPI_TypeDef *dev;
    ioTag_t sck;
    ioTag_t miso;
    ioTag_t mosi;
    uint8_t af;
    rccPeriphTag_t rcc;
    volatile uint16_t errorCount;
    bool leadingEdge;
    uint8_t dmaIrqHandler;
} spiDevice_t;


static uint8_t spiRegisteredDeviceCount = 0;

spiDevice_t spiDevice[SPIDEV_COUNT];
busDevice_t spiBusDevice[SPIDEV_COUNT];

typedef struct spiPreinit_s {
    ioTag_t iotag;
    uint8_t iocfg;
    bool init;
} spiPreinit_t;

static spiPreinit_t spiPreinitArray[SPI_PREINIT_COUNT];
static int spiPreinitCount;

SPIDevice spiDeviceByInstance(SPI_TypeDef *instance)
{
    if (instance == SPI1) {
        return SPIDEV_1;
    }

    if (instance == SPI2) {
        return SPIDEV_2;
    }

    if (instance == SPI3) {
        return SPIDEV_3;
    }

    return SPIINVALID;
}

SPI_TypeDef *spiInstanceByDevice(SPIDevice device)
{
    if (device == SPIINVALID || device >= SPIDEV_COUNT) {
        return NULL;
    }

    return spiDevice[device].dev;
}

static void initmask(uint8_t mask, uint8_t k)
{
    if ((mask>>k) & 0x01) {
        spiInitDevice(k);
    }
}

void spiInit(uint8_t mask)
{
    initmask(mask, 0);
    initmask(mask, 1);
    initmask(mask, 2);
}

// Return true if DMA engine is busy
bool spiIsBusy(const extDevice_t *dev)
{
    return (dev->bus->curSegment != (busSegment_t *)BUS_SPI_FREE);
}

// Indicate that the bus on which this device resides may initiate DMA transfers from interrupt context
void spiSetAtomicWait(const extDevice_t *dev)
{
    dev->bus->useAtomicWait = true;
}

// Wait for DMA completion and claim the bus driver
void spiWaitClaim(const extDevice_t *dev)
{
    // If there is a device on the bus whose driver might call spiSequence from an ISR then an
    // atomic access is required to claim the bus, however if not, then interrupts need not be
    // disabled as this can result in edge triggered interrupts being missed

    if (dev->bus->useAtomicWait) {
        // Prevent race condition where the bus appears free, but a gyro interrupt starts a transfer
        do {
            ATOMIC_BLOCK(NVIC_PRIO_MAX) {
                if (dev->bus->curSegment == (busSegment_t *)BUS_SPI_FREE) {
                    dev->bus->curSegment = (busSegment_t *)BUS_SPI_LOCKED;
                }
            }
        } while (dev->bus->curSegment != (busSegment_t *)BUS_SPI_LOCKED);
    } else {
        // Wait for completion
        while (dev->bus->curSegment != (busSegment_t *)BUS_SPI_FREE);
    }
}

// Wait for DMA completion
void spiWait(const extDevice_t *dev)
{
    // Wait for completion
    while (dev->bus->curSegment != (busSegment_t *)BUS_SPI_FREE);
}

// Wait for bus to become free, then read/write block of data
void spiReadWriteBuf(const extDevice_t *dev, uint8_t *txData, uint8_t *rxData, int len)
{
    // This routine blocks so no need to use static data
    busSegment_t segments[] = {
            {txData, rxData, len, true, NULL},
            {NULL, NULL, 0, true, NULL},
    };

    // Ensure any prior DMA has completed before continuing
    spiWaitClaim(dev);

    spiSequence(dev, &segments[0]);

    spiWait(dev);
}

// Read/Write a block of data, returning false if the bus is busy
bool spiReadWriteBufRB(const extDevice_t *dev, uint8_t *txData, uint8_t *rxData, int length)
{
    // Ensure any prior DMA has completed before continuing
    if (spiIsBusy(dev)) {
        return false;
    }

    spiReadWriteBuf(dev, txData, rxData, length);

    return true;
}

// Wait for bus to become free, then read/write a single byte
uint8_t spiReadWrite(const extDevice_t *dev, uint8_t data)
{
    uint8_t retval;

    // This routine blocks so no need to use static data
    busSegment_t segments[] = {
            {&data, &retval, sizeof(data), true, NULL},
            {NULL, NULL, 0, true, NULL},
    };

    // Ensure any prior DMA has completed before continuing
    spiWaitClaim(dev);

    spiSequence(dev, &segments[0]);

    spiWait(dev);

    return retval;
}

// Wait for bus to become free, then read/write a single byte from a register
uint8_t spiReadWriteReg(const extDevice_t *dev, uint8_t reg, uint8_t data)
{
    uint8_t retval;

    // This routine blocks so no need to use static data
    busSegment_t segments[] = {
            {&reg, NULL, sizeof(reg), false, NULL},
            {&data, &retval, sizeof(data), true, NULL},
            {NULL, NULL, 0, true, NULL},
    };

    // Ensure any prior DMA has completed before continuing
    spiWaitClaim(dev);

    spiSequence(dev, &segments[0]);

    spiWait(dev);

    return retval;
}

// Wait for bus to become free, then write a single byte
void spiWrite(const extDevice_t *dev, uint8_t data)
{
    // This routine blocks so no need to use static data
    busSegment_t segments[] = {
            {&data, NULL, sizeof(data), true, NULL},
            {NULL, NULL, 0, true, NULL},
    };

    // Ensure any prior DMA has completed before continuing
    spiWaitClaim(dev);

    spiSequence(dev, &segments[0]);

    spiWait(dev);
}

// Write data to a register
void spiWriteReg(const extDevice_t *dev, uint8_t reg, uint8_t data)
{
    // This routine blocks so no need to use static data
    busSegment_t segments[] = {
            {&reg, NULL, sizeof(reg), false, NULL},
            {&data, NULL, sizeof(data), true, NULL},
            {NULL, NULL, 0, true, NULL},
    };

    // Ensure any prior DMA has completed before continuing
    spiWaitClaim(dev);

    spiSequence(dev, &segments[0]);

    spiWait(dev);
}

// Write data to a register, returning false if the bus is busy
bool spiWriteRegRB(const extDevice_t *dev, uint8_t reg, uint8_t data)
{
    // Ensure any prior DMA has completed before continuing
    if (spiIsBusy(dev)) {
        return false;
    }

    spiWriteReg(dev, reg, data);

    return true;
}

// Read a block of data from a register
void spiReadRegBuf(const extDevice_t *dev, uint8_t reg, uint8_t *data, uint8_t length)
{
    // This routine blocks so no need to use static data
    busSegment_t segments[] = {
            {&reg, NULL, sizeof(reg), false, NULL},
            {NULL, data, length, true, NULL},
            {NULL, NULL, 0, true, NULL},
    };

    // Ensure any prior DMA has completed before continuing
    spiWaitClaim(dev);

    spiSequence(dev, &segments[0]);

    spiWait(dev);
}

// Read a block of data from a register, returning false if the bus is busy
bool spiReadRegBufRB(const extDevice_t *dev, uint8_t reg, uint8_t *data, uint8_t length)
{
    // Ensure any prior DMA has completed before continuing
    if (spiIsBusy(dev)) {
        return false;
    }

    spiReadRegBuf(dev, reg, data, length);

    return true;
}

// Read a block of data where the register is ORed with 0x80, returning false if the bus is busy
bool spiReadRegMskBufRB(const extDevice_t *dev, uint8_t reg, uint8_t *data, uint8_t length)
{
    return spiReadRegBufRB(dev, reg | 0x80, data, length);
}

// Wait for bus to become free, then write a block of data to a register
void spiWriteRegBuf(const extDevice_t *dev, uint8_t reg, uint8_t *data, uint32_t length)
{
    // This routine blocks so no need to use static data
    busSegment_t segments[] = {
            {&reg, NULL, sizeof(reg), false, NULL},
            {data, NULL, length, true, NULL},
            {NULL, NULL, 0, true, NULL},
    };

    // Ensure any prior DMA has completed before continuing
    spiWaitClaim(dev);

    spiSequence(dev, &segments[0]);

    spiWait(dev);
}

// Wait for bus to become free, then read a byte from a register
uint8_t spiReadReg(const extDevice_t *dev, uint8_t reg)
{
    uint8_t data;
    // This routine blocks so no need to use static data
    busSegment_t segments[] = {
            {&reg, NULL, sizeof(reg), false, NULL},
            {NULL, &data, sizeof(data), true, NULL},
            {NULL, NULL, 0, true, NULL},
    };

    // Ensure any prior DMA has completed before continuing
    spiWaitClaim(dev);

    spiSequence(dev, &segments[0]);

    spiWait(dev);

    return data;
}

// Wait for bus to become free, then read a byte of data where the register is ORed with 0x80
uint8_t spiReadRegMsk(const extDevice_t *dev, uint8_t reg)
{
    return spiReadReg(dev, reg | 0x80);
}

uint16_t spiCalculateDivider(uint32_t freq)
{
    uint32_t spiClk = SystemCoreClock / 2;

    uint16_t divisor = 2;

    spiClk >>= 1;

    for (; (spiClk > freq) && (divisor < 256); divisor <<= 1, spiClk >>= 1);

    return divisor;
}

// Interrupt handler for SPI receive DMA completion
static void spiIrqHandler(const extDevice_t *dev)
{
    busDevice_t *bus = dev->bus;
    busSegment_t *nextSegment;

    if (bus->curSegment->callback) {
        switch(bus->curSegment->callback(dev->callbackArg)) {
        case BUS_BUSY:
            // Repeat the last DMA segment
            bus->curSegment--;
            // Reinitialise the cached init values as segment is not progressing
            spiInternalInitStream(dev, true);
            break;

        case BUS_ABORT:
            bus->curSegment = (busSegment_t *)BUS_SPI_FREE;
            return;

        case BUS_READY:
        default:
            // Advance to the next DMA segment
            break;
        }
    }

    // Advance through the segment list
    nextSegment = bus->curSegment + 1;

    if (nextSegment->len == 0) {
        // If a following transaction has been linked, start it
        if (nextSegment->txData) {
            const extDevice_t *nextDev = (const extDevice_t *)nextSegment->txData;
            busSegment_t *nextSegments = (busSegment_t *)nextSegment->rxData;
            nextSegment->txData = NULL;
            // The end of the segment list has been reached
            spiSequenceStart(nextDev, nextSegments);
        } else {
            // The end of the segment list has been reached, so mark transactions as complete
            bus->curSegment = (busSegment_t *)BUS_SPI_FREE;
        }
    } else {
        bus->curSegment = nextSegment;

        // After the completion of the first segment setup the init structure for the subsequent segment
        if (bus->initSegment) {
            spiInternalInitStream(dev, false);
            bus->initSegment = false;
        }

        // Launch the next transfer
        spiInternalStartDMA(dev);

        // Prepare the init structures ready for the next segment to reduce inter-segment time
        spiInternalInitStream(dev, true);
    }
}

// Interrupt handler for SPI receive DMA completion
static void spiRxIrqHandler(dmaChannelDescriptor_t* descriptor)
{
    const extDevice_t *dev = (const extDevice_t *)descriptor->userParam;

    if (!dev) {
        return;
    }

    busDevice_t *bus = dev->bus;

    if (bus->curSegment->negateCS) {
        // Negate Chip Select
        IOHi(dev->busType_u.spi.csnPin);
    }

    spiInternalStopDMA(dev);

    spiIrqHandler(dev);
}

// Interrupt handler for SPI transmit DMA completion
static void spiTxIrqHandler(dmaChannelDescriptor_t* descriptor)
{
    const extDevice_t *dev = (const extDevice_t *)descriptor->userParam;

    if (!dev) {
        return;
    }

    busDevice_t *bus = dev->bus;

    spiInternalStopDMA(dev);

    if (bus->curSegment->negateCS) {
        // Negate Chip Select
        IOHi(dev->busType_u.spi.csnPin);
    }

    spiIrqHandler(dev);
}

// Mark this bus as being SPI and record the first owner to use it
bool spiSetBusInstance(extDevice_t *dev, uint32_t device)
{
    if ((device == 0) || (device > SPIDEV_COUNT)) {
        return false;
    }

    dev->bus = &spiBusDevice[SPI_CFG_TO_DEV(device)];

    // By default each device should use SPI DMA if the bus supports it
    dev->useDMA = true;

    if (dev->bus->busType == BUS_TYPE_SPI) {
        // This bus has already been initialised
        dev->bus->deviceCount++;
        return true;
    }

    busDevice_t *bus = dev->bus;

    bus->busType_u.spi.instance = spiInstanceByDevice(SPI_CFG_TO_DEV(device));

    if (bus->busType_u.spi.instance == NULL) {
        return false;
    }

    bus->busType = BUS_TYPE_SPI;
    bus->useDMA = false;
    bus->useAtomicWait = false;
    bus->deviceCount = 1;
    bus->initTx = &dev->initTx;
    bus->initRx = &dev->initRx;

    return true;
}

void spiInitBusDMA()
{
    uint32_t device;
    /* Check
     * https://www.st.com/resource/en/errata_sheet/dm00037591-stm32f405407xx-and-stm32f415417xx-device-limitations-stmicroelectronics.pdf
     * section 2.1.10 which reports an errata that corruption may occurs on
     * DMA2 if AHB peripherals (eg GPIO ports) are access concurrently with APB
     * peripherals (eg SPI busses). Bitbang DSHOT uses DMA2 to write to GPIO
     * ports. If this is enabled, then don't enable DMA on an SPI bus using
     * DMA2
     */

    spiPinConfig_t spiPinConfig;

    spiPinConfig.ioTagSck  = 21;
    spiPinConfig.ioTagMiso = 22;
    spiPinConfig.ioTagMosi = 23;
    spiPinConfig.txDmaopt  = -1;
    spiPinConfig.rxDmaopt  = -1;

    const bool dshotBitbangActive = true;

    for (device = 0; device < SPIDEV_COUNT; device++) {
        busDevice_t *bus = &spiBusDevice[device];

        if (bus->busType != BUS_TYPE_SPI) {
            // This bus is not in use
            continue;
        }

        dmaIdentifier_e dmaTxIdentifier = DMA_NONE;
        dmaIdentifier_e dmaRxIdentifier = DMA_NONE;

        int8_t txDmaopt = spiPinConfig.txDmaopt;
        uint8_t txDmaoptMin = 0;
        uint8_t txDmaoptMax = MAX_PERIPHERAL_DMA_OPTIONS - 1;

        if (txDmaopt != -1) {
            txDmaoptMin = txDmaopt;
            txDmaoptMax = txDmaopt;
        }

        for (uint8_t opt = txDmaoptMin; opt <= txDmaoptMax; opt++) {
            const dmaChannelSpec_t *dmaTxChannelSpec = dmaGetChannelSpecByPeripheral(DMA_PERIPH_SPI_MOSI, device, opt);

            if (dmaTxChannelSpec) {
                dmaTxIdentifier = dmaGetIdentifier(dmaTxChannelSpec->ref);
                if (!dmaAllocate(dmaTxIdentifier, OWNER_SPI_MOSI, device + 1)) {
                    dmaTxIdentifier = DMA_NONE;
                    continue;
                }
                if (dshotBitbangActive && (DMA_DEVICE_NO(dmaTxIdentifier) == 2)) {
                    dmaTxIdentifier = DMA_NONE;
                    break;
                }
                bus->dmaTx = dmaGetDescriptorByIdentifier(dmaTxIdentifier);
                bus->dmaTx->stream = DMA_DEVICE_INDEX(dmaTxIdentifier);
                bus->dmaTx->channel = dmaTxChannelSpec->channel;

                dmaEnable(dmaTxIdentifier);

                break;
            }
        }

        int8_t rxDmaopt = spiPinConfig.rxDmaopt;
        uint8_t rxDmaoptMin = 0;
        uint8_t rxDmaoptMax = MAX_PERIPHERAL_DMA_OPTIONS - 1;

        if (rxDmaopt != -1) {
            rxDmaoptMin = rxDmaopt;
            rxDmaoptMax = rxDmaopt;
        }

        for (uint8_t opt = rxDmaoptMin; opt <= rxDmaoptMax; opt++) {
            const dmaChannelSpec_t *dmaRxChannelSpec = dmaGetChannelSpecByPeripheral(DMA_PERIPH_SPI_MISO, device, opt);

            if (dmaRxChannelSpec) {
                dmaRxIdentifier = dmaGetIdentifier(dmaRxChannelSpec->ref);
                if (!dmaAllocate(dmaRxIdentifier, OWNER_SPI_MISO, device + 1)) {
                    dmaRxIdentifier = DMA_NONE;
                    continue;
                }
                if (dshotBitbangActive && (DMA_DEVICE_NO(dmaRxIdentifier) == 2)) {
                    dmaRxIdentifier = DMA_NONE;
                    break;
                }
                bus->dmaRx = dmaGetDescriptorByIdentifier(dmaRxIdentifier);
                bus->dmaRx->stream = DMA_DEVICE_INDEX(dmaRxIdentifier);
                bus->dmaRx->channel = dmaRxChannelSpec->channel;

                dmaEnable(dmaRxIdentifier);

                break;
            }
        }

        if (dmaTxIdentifier && dmaRxIdentifier) {
            // Ensure streams are disabled
            spiInternalResetStream(bus->dmaRx);
            spiInternalResetStream(bus->dmaTx);

            spiInternalResetDescriptors(bus);

            /* Note that this driver may be called both from the normal thread of execution, or from USB interrupt
             * handlers, so the DMA completion interrupt must be at a higher priority
             */
            dmaSetHandler(dmaRxIdentifier, spiRxIrqHandler, NVIC_PRIO_SPI_DMA, 0);

            bus->useDMA = true;
        } else if (dmaTxIdentifier) {
            // Transmit on DMA is adequate for OSD so worth having
            bus->dmaTx = dmaGetDescriptorByIdentifier(dmaTxIdentifier);
            bus->dmaRx = (dmaChannelDescriptor_t *)NULL;

            // Ensure streams are disabled
            spiInternalResetStream(bus->dmaTx);

            spiInternalResetDescriptors(bus);

            dmaSetHandler(dmaTxIdentifier, spiTxIrqHandler, NVIC_PRIO_SPI_DMA, 0);

            bus->useDMA = true;
        } else {
            // Disassociate channels from bus
            bus->dmaRx = (dmaChannelDescriptor_t *)NULL;
            bus->dmaTx = (dmaChannelDescriptor_t *)NULL;
        }
    }
}

void spiSetClkDivisor(const extDevice_t *dev, uint16_t divisor)
{
    ((extDevice_t *)dev)->busType_u.spi.speed = divisor;
}

// Set the clock phase/polarity to be used for accesses by the given device
void spiSetClkPhasePolarity(const extDevice_t *dev, bool leadingEdge)
{
    ((extDevice_t *)dev)->busType_u.spi.leadingEdge = leadingEdge;
}

// Enable/disable DMA on a specific device. Enabled by default.
void spiDmaEnable(const extDevice_t *dev, bool enable)
{
    ((extDevice_t *)dev)->useDMA = enable;
}

bool spiUseDMA(const extDevice_t *dev)
{
    // Full DMA only requires both transmit and receive}
    return dev->bus->useDMA && dev->bus->dmaRx && dev->useDMA;
}

bool spiUseMOSI_DMA(const extDevice_t *dev)
{
    return dev->bus->useDMA && dev->useDMA;
}

void spiBusDeviceRegister(const extDevice_t *dev)
{
    UNUSED(dev);

    spiRegisteredDeviceCount++;
}

uint8_t spiGetRegisteredDeviceCount(void)
{
    return spiRegisteredDeviceCount;
}

uint8_t spiGetExtDeviceCount(const extDevice_t *dev)
{
    return dev->bus->deviceCount;
}

// DMA transfer setup and start
void spiSequence(const extDevice_t *dev, busSegment_t *segments)
{
    busDevice_t *bus = dev->bus;

    ATOMIC_BLOCK(NVIC_PRIO_MAX) {
        if ((bus->curSegment != (busSegment_t *)BUS_SPI_LOCKED) && spiIsBusy(dev)) {
            /* Defer this transfer to be triggered upon completion of the current transfer. Blocking calls
             * and those from non-interrupt context will have already called spiWaitClaim() so this will
             * only happen for non-blocking calls called from an ISR.
             */
            busSegment_t *endSegment = bus->curSegment;

            if (endSegment) {
                // Find the last segment of the current transfer
                for (; endSegment->len; endSegment++);

                // Record the dev and segments parameters in the terminating segment entry
                endSegment->txData = (uint8_t *)dev;
                endSegment->rxData = (uint8_t *)segments;

                return;
            }
        }
    }

    spiSequenceStart(dev, segments);
}


void spiPreinitRegister(ioTag_t iotag, uint8_t iocfg, bool init)
{
    if (!iotag) {
        return;
    }

    if (spiPreinitCount == SPI_PREINIT_COUNT) {
        systemIndicateFailure(FAILURE_DEVELOPER, 5);
        return;
    }

    spiPreinitArray[spiPreinitCount].iotag = iotag;
    spiPreinitArray[spiPreinitCount].iocfg = iocfg;
    spiPreinitArray[spiPreinitCount].init = init;
    ++spiPreinitCount;
}

static void spiPreinitPin(spiPreinit_t *preinit, int index)
{
    IO_t io = IOGetByTag(preinit->iotag);
    IOInit(io, OWNER_PREINIT, RESOURCE_INDEX(index));
    IOConfigGPIO(io, preinit->iocfg);
    if (preinit->init) {
        IOHi(io);
    } else {
        IOLo(io);
    }
}

void spiPreInit(void)
{
    flashPreInit();

    for (int i = 0; i < spiPreinitCount; i++) {
        spiPreinitPin(&spiPreinitArray[i], i);
    }
}

void spiPreinitByIO(IO_t io)
{
    for (int i = 0; i < spiPreinitCount; i++) {
        if (io == IOGetByTag(spiPreinitArray[i].iotag)) {
            spiPreinitPin(&spiPreinitArray[i], i);
            return;
        }
    }
}

void spiPreinitByTag(ioTag_t tag)
{
    spiPreinitByIO(IOGetByTag(tag));
}

const spiHardware_t spiHardware[] = {
    {
        .device = SPIDEV_1,
        .reg = SPI1,
        .sckPins = {
            { DEFIO_TAG_E(PA5) },
            { DEFIO_TAG_E(PB3) },
        },
        .misoPins = {
            { DEFIO_TAG_E(PA6) },
            { DEFIO_TAG_E(PB4) },
        },
        .mosiPins = {
            { DEFIO_TAG_E(PA7) },
            { DEFIO_TAG_E(PB5) },
        },
        .af = GPIO_AF_SPI1,
        .rcc = RCC_APB2(SPI1),
    },
    {
        .device = SPIDEV_2,
        .reg = SPI2,
        .sckPins = {
            { DEFIO_TAG_E(PB10) },
            { DEFIO_TAG_E(PB13) },
        },
        .misoPins = {
            { DEFIO_TAG_E(PB14) },
            { DEFIO_TAG_E(PC2) },
        },
        .mosiPins = {
            { DEFIO_TAG_E(PB15) },
            { DEFIO_TAG_E(PC3) },
        },
        .af = GPIO_AF_SPI2,
        .rcc = RCC_APB1(SPI2),
    },
    {
        .device = SPIDEV_3,
        .reg = SPI3,
        .sckPins = {
            { DEFIO_TAG_E(PB3) },
            { DEFIO_TAG_E(PC10) },
        },
        .misoPins = {
            { DEFIO_TAG_E(PB4) },
            { DEFIO_TAG_E(PC11) },
        },
        .mosiPins = {
            { DEFIO_TAG_E(PB5) },
            { DEFIO_TAG_E(PC12) },
        },
        .af = GPIO_AF_SPI3,
        .rcc = RCC_APB1(SPI3),
    },
};

void spiPinConfigure(void)
{
    spiPinConfig_t spiPinConfig;

    spiPinConfig.ioTagSck = 21;   // 21 & 0x07 = 5; so, PA5
    spiPinConfig.ioTagMiso = 22;  //                    PA6
    spiPinConfig.ioTagMosi = 23;  //                    PA7
    spiPinConfig.txDmaopt = -1;
    spiPinConfig.rxDmaopt = -1;

    spiPinConfig_t * pConfig = &spiPinConfig;

    for (size_t hwindex = 0 ; hwindex < ARRAYLEN(spiHardware) ; hwindex++) {
        const spiHardware_t *hw = &spiHardware[hwindex];

        if (!hw->reg) {
            continue;
        }

        SPIDevice device = hw->device;
        spiDevice_t *pDev = &spiDevice[device];

        for (int pindex = 0 ; pindex < MAX_SPI_PIN_SEL ; pindex++) {
            if (pConfig[device].ioTagSck == hw->sckPins[pindex].pin) {
                pDev->sck = hw->sckPins[pindex].pin;
            }
            if (pConfig[device].ioTagMiso == hw->misoPins[pindex].pin) {
                pDev->miso = hw->misoPins[pindex].pin;
            }
            if (pConfig[device].ioTagMosi == hw->mosiPins[pindex].pin) {
                pDev->mosi = hw->mosiPins[pindex].pin;
            }
        }

        if (pDev->sck && pDev->miso && pDev->mosi) {
            pDev->dev = hw->reg;
            pDev->af = hw->af;
            pDev->rcc = hw->rcc;
            pDev->leadingEdge = false; // XXX Should be part of transfer context
            pDev->dmaIrqHandler = hw->dmaIrqHandler;
        }
    }
}

// STM32F405 can't DMA to/from FASTRAM (CCM SRAM)
#define IS_CCM(p) (((uint32_t)p & 0xffff0000) == 0x10000000)

static SPI_InitTypeDef defaultInit = {
    .SPI_Mode = SPI_Mode_Master,
    .SPI_Direction = SPI_Direction_2Lines_FullDuplex,
    .SPI_DataSize = SPI_DataSize_8b,
    .SPI_NSS = SPI_NSS_Soft,
    .SPI_FirstBit = SPI_FirstBit_MSB,
    .SPI_CRCPolynomial = 7,
    .SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8,
    .SPI_CPOL = SPI_CPOL_High,
    .SPI_CPHA = SPI_CPHA_2Edge
};

static uint16_t spiDivisorToBRbits(SPI_TypeDef *instance, uint16_t divisor)
{
    // SPI2 and SPI3 are on APB1/AHB1 which PCLK is half that of APB2/AHB2.
#if defined(STM32F410xx) || defined(STM32F411xE)
    UNUSED(instance);
#else
     if (instance == SPI2 || instance == SPI3) {
        divisor /= 2; // Safe for divisor == 0 or 1
    }
#endif

    divisor = constrain_u16(divisor, 2, 256);

    return (ffs(divisor) - 2) << 3; // SPI_CR1_BR_Pos
}

static void spiSetDivisorBRreg(SPI_TypeDef *instance, uint16_t divisor)
{
#define BR_BITS ((BIT(5) | BIT(4) | BIT(3)))
    const uint16_t tempRegister = (instance->CR1 & ~BR_BITS);
    instance->CR1 = tempRegister | spiDivisorToBRbits(instance, divisor);
#undef BR_BITS
}


void spiInitDevice(SPIDevice device)
{
    spiDevice_t *spi = &(spiDevice[device]);

    if (!spi->dev) {
        return;
    }

    // Enable SPI clock
    RCC_ClockCmd(spi->rcc, ENABLE);
    RCC_ResetCmd(spi->rcc, ENABLE);

    IOInit(IOGetByTag(spi->sck),  OWNER_SPI_SCK,  RESOURCE_INDEX(device));
    IOInit(IOGetByTag(spi->miso), OWNER_SPI_MISO, RESOURCE_INDEX(device));
    IOInit(IOGetByTag(spi->mosi), OWNER_SPI_MOSI, RESOURCE_INDEX(device));

    IOConfigGPIOAF(IOGetByTag(spi->sck),  SPI_IO_AF_SCK_CFG, spi->af);
    IOConfigGPIOAF(IOGetByTag(spi->miso), SPI_IO_AF_MISO_CFG, spi->af);
    IOConfigGPIOAF(IOGetByTag(spi->mosi), SPI_IO_AF_CFG, spi->af);

    // Init SPI hardware
    SPI_I2S_DeInit(spi->dev);

    SPI_I2S_DMACmd(spi->dev, SPI_I2S_DMAReq_Tx | SPI_I2S_DMAReq_Rx, DISABLE);
    SPI_Init(spi->dev, &defaultInit);
    SPI_Cmd(spi->dev, ENABLE);
}

void spiInternalResetDescriptors(busDevice_t *bus)
{
    DMA_InitTypeDef *initTx = bus->initTx;

    DMA_StructInit(initTx);
    initTx->DMA_Channel = bus->dmaTx->channel;
    initTx->DMA_DIR = DMA_DIR_MemoryToPeripheral;
    initTx->DMA_Mode = DMA_Mode_Normal;
    initTx->DMA_PeripheralBaseAddr = (uint32_t)&bus->busType_u.spi.instance->DR;
    initTx->DMA_Priority = DMA_Priority_Low;
    initTx->DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    initTx->DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    initTx->DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;

    if (bus->dmaRx) {
        DMA_InitTypeDef *initRx = bus->initRx;

        DMA_StructInit(initRx);
        initRx->DMA_Channel = bus->dmaRx->channel;
        initRx->DMA_DIR = DMA_DIR_PeripheralToMemory;
        initRx->DMA_Mode = DMA_Mode_Normal;
        initRx->DMA_PeripheralBaseAddr = (uint32_t)&bus->busType_u.spi.instance->DR;
        initRx->DMA_Priority = DMA_Priority_Low;
        initRx->DMA_PeripheralInc = DMA_PeripheralInc_Disable;
        initRx->DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    }
}

void spiInternalResetStream(dmaChannelDescriptor_t *descriptor)
{
    DMA_Stream_TypeDef *streamRegs = (DMA_Stream_TypeDef *)descriptor->ref;

    // Disable the stream
    streamRegs->CR = 0U;

    // Clear any pending interrupt flags
    DMA_CLEAR_FLAG(descriptor, DMA_IT_HTIF | DMA_IT_TEIF | DMA_IT_TCIF);
}

static bool spiInternalReadWriteBufPolled(SPI_TypeDef *instance, const uint8_t *txData, uint8_t *rxData, int len)
{
    uint8_t b;

    while (len--) {
        b = txData ? *(txData++) : 0xFF;
        while (SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_TXE) == RESET);
        SPI_I2S_SendData(instance, b);

        while (SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_RXNE) == RESET);
        b = SPI_I2S_ReceiveData(instance);
        if (rxData) {
            *(rxData++) = b;
        }
    }

    return true;
}

void spiInternalInitStream(const extDevice_t *dev, bool preInit)
{
    static uint8_t dummyTxByte = 0xff;
    static uint8_t dummyRxByte;
    busDevice_t *bus = dev->bus;

    volatile busSegment_t *segment = bus->curSegment;

    if (preInit) {
        // Prepare the init structure for the next segment to reduce inter-segment interval
        segment++;
        if(segment->len == 0) {
            // There's no following segment
            return;
        }
    }

    int len = segment->len;

    uint8_t *txData = segment->txData;
    DMA_InitTypeDef *initTx = bus->initTx;

    if (txData) {
        initTx->DMA_Memory0BaseAddr = (uint32_t)txData;
        initTx->DMA_MemoryInc = DMA_MemoryInc_Enable;
    } else {
        dummyTxByte = 0xff;
        initTx->DMA_Memory0BaseAddr = (uint32_t)&dummyTxByte;
        initTx->DMA_MemoryInc = DMA_MemoryInc_Disable;
    }
    initTx->DMA_BufferSize = len;

    if (dev->bus->dmaRx) {
        uint8_t *rxData = segment->rxData;
        DMA_InitTypeDef *initRx = bus->initRx;

        if (rxData) {
            initRx->DMA_Memory0BaseAddr = (uint32_t)rxData;
            initRx->DMA_MemoryInc = DMA_MemoryInc_Enable;
        } else {
            initRx->DMA_Memory0BaseAddr = (uint32_t)&dummyRxByte;
            initRx->DMA_MemoryInc = DMA_MemoryInc_Disable;
        }
        // If possible use 16 bit memory writes to prevent atomic access issues on gyro data
        if ((initRx->DMA_Memory0BaseAddr & 0x1) || (len & 0x1)) {
            initRx->DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
        } else {
            initRx->DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
        }
        initRx->DMA_BufferSize = len;
    }
}

void spiInternalStartDMA(const extDevice_t *dev)
{
    // Assert Chip Select
    IOLo(dev->busType_u.spi.csnPin);

    dmaChannelDescriptor_t *dmaTx = dev->bus->dmaTx;
    dmaChannelDescriptor_t *dmaRx = dev->bus->dmaRx;
    DMA_Stream_TypeDef *streamRegsTx = (DMA_Stream_TypeDef *)dmaTx->ref;
    if (dmaRx) {
        DMA_Stream_TypeDef *streamRegsRx = (DMA_Stream_TypeDef *)dmaRx->ref;

        // Use the correct callback argument
        dmaRx->userParam = (uint32_t)dev;

        // Clear transfer flags
        DMA_CLEAR_FLAG(dmaTx, DMA_IT_HTIF | DMA_IT_TEIF | DMA_IT_TCIF);
        DMA_CLEAR_FLAG(dmaRx, DMA_IT_HTIF | DMA_IT_TEIF | DMA_IT_TCIF);

        // Disable streams to enable update
        streamRegsTx->CR = 0U;
        streamRegsRx->CR = 0U;

        /* Use the Rx interrupt as this occurs once the SPI operation is complete whereas the Tx interrupt
         * occurs earlier when the Tx FIFO is empty, but the SPI operation is still in progress
         */
        DMA_ITConfig(streamRegsRx, DMA_IT_TC, ENABLE);

        // Update streams
        DMA_Init(streamRegsTx, dev->bus->initTx);
        DMA_Init(streamRegsRx, dev->bus->initRx);

        /* Note from AN4031
         *
         * If the user enables the used peripheral before the corresponding DMA stream, a “FEIF”
         * (FIFO Error Interrupt Flag) may be set due to the fact the DMA is not ready to provide
         * the first required data to the peripheral (in case of memory-to-peripheral transfer).
         */

        // Enable streams
        DMA_Cmd(streamRegsTx, ENABLE);
        DMA_Cmd(streamRegsRx, ENABLE);

        /* Enable the SPI DMA Tx & Rx requests */
        SPI_I2S_DMACmd(dev->bus->busType_u.spi.instance, SPI_I2S_DMAReq_Tx | SPI_I2S_DMAReq_Rx, ENABLE);
    } else {
        // Use the correct callback argument
        dmaTx->userParam = (uint32_t)dev;

        // Clear transfer flags
        DMA_CLEAR_FLAG(dmaTx, DMA_IT_HTIF | DMA_IT_TEIF | DMA_IT_TCIF);

        // Disable stream to enable update
        streamRegsTx->CR = 0U;

        DMA_ITConfig(streamRegsTx, DMA_IT_TC, ENABLE);

        // Update stream
        DMA_Init(streamRegsTx, dev->bus->initTx);

        /* Note from AN4031
         *
         * If the user enables the used peripheral before the corresponding DMA stream, a “FEIF”
         * (FIFO Error Interrupt Flag) may be set due to the fact the DMA is not ready to provide
         * the first required data to the peripheral (in case of memory-to-peripheral transfer).
         */

        // Enable stream
        DMA_Cmd(streamRegsTx, ENABLE);

        /* Enable the SPI DMA Tx request */
        SPI_I2S_DMACmd(dev->bus->busType_u.spi.instance, SPI_I2S_DMAReq_Tx, ENABLE);
    }
}


void spiInternalStopDMA (const extDevice_t *dev)
{
    dmaChannelDescriptor_t *dmaTx = dev->bus->dmaTx;
    dmaChannelDescriptor_t *dmaRx = dev->bus->dmaRx;
    SPI_TypeDef *instance = dev->bus->busType_u.spi.instance;
    DMA_Stream_TypeDef *streamRegsTx = (DMA_Stream_TypeDef *)dmaTx->ref;

    if (dmaRx) {
        DMA_Stream_TypeDef *streamRegsRx = (DMA_Stream_TypeDef *)dmaRx->ref;

        // Disable streams
        streamRegsTx->CR = 0U;
        streamRegsRx->CR = 0U;

        SPI_I2S_DMACmd(instance, SPI_I2S_DMAReq_Tx | SPI_I2S_DMAReq_Rx, DISABLE);
    } else {
        // Ensure the current transmission is complete
        while (SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_BSY));

        // Drain the RX buffer
        while (SPI_I2S_GetFlagStatus(instance, SPI_I2S_FLAG_RXNE)) {
            instance->DR;
        }

        // Disable stream
        streamRegsTx->CR = 0U;

        SPI_I2S_DMACmd(instance, SPI_I2S_DMAReq_Tx, DISABLE);
    }
}

// DMA transfer setup and start
void spiSequenceStart(const extDevice_t *dev, busSegment_t *segments)
{
    busDevice_t *bus = dev->bus;
    SPI_TypeDef *instance = bus->busType_u.spi.instance;
    bool dmaSafe = dev->useDMA;
    uint32_t xferLen = 0;
    uint32_t segmentCount = 0;

    dev->bus->initSegment = true;
    dev->bus->curSegment = segments;

    SPI_Cmd(instance, DISABLE);

    // Switch bus speed
    if (dev->busType_u.spi.speed != bus->busType_u.spi.speed) {
        spiSetDivisorBRreg(bus->busType_u.spi.instance, dev->busType_u.spi.speed);
        bus->busType_u.spi.speed = dev->busType_u.spi.speed;
    }

    if (dev->busType_u.spi.leadingEdge != bus->busType_u.spi.leadingEdge) {
        // Switch SPI clock polarity/phase
        instance->CR1 &= ~(SPI_CPOL_High | SPI_CPHA_2Edge);

        // Apply setting
        if (dev->busType_u.spi.leadingEdge) {
            instance->CR1 |= SPI_CPOL_Low | SPI_CPHA_1Edge;
        } else
        {
            instance->CR1 |= SPI_CPOL_High | SPI_CPHA_2Edge;
        }
        bus->busType_u.spi.leadingEdge = dev->busType_u.spi.leadingEdge;
    }

    SPI_Cmd(instance, ENABLE);

    // Check that any there are no attempts to DMA to/from CCD SRAM
    for (busSegment_t *checkSegment = bus->curSegment; checkSegment->len; checkSegment++) {
        // Check there is no receive data as only transmit DMA is available
        if (((checkSegment->rxData) && (IS_CCM(checkSegment->rxData) || (bus->dmaRx == (dmaChannelDescriptor_t *)NULL))) ||
            ((checkSegment->txData) && IS_CCM(checkSegment->txData))) {
            dmaSafe = false;
            break;
        }
        // Note that these counts are only valid if dmaSafe is true
        segmentCount++;
        xferLen += checkSegment->len;
    }
    // Use DMA if possible
    if (bus->useDMA && dmaSafe && ((segmentCount > 1) || (xferLen > 8))) {
        // Intialise the init structures for the first transfer
        spiInternalInitStream(dev, false);

        // Start the transfers
        spiInternalStartDMA(dev);
    } else {
        // Manually work through the segment list performing a transfer for each
        while (bus->curSegment->len) {
            // Assert Chip Select
            IOLo(dev->busType_u.spi.csnPin);

            spiInternalReadWriteBufPolled(
                    bus->busType_u.spi.instance,
                    bus->curSegment->txData,
                    bus->curSegment->rxData,
                    bus->curSegment->len);

            if (bus->curSegment->negateCS) {
                // Negate Chip Select
                IOHi(dev->busType_u.spi.csnPin);
            }

            if (bus->curSegment->callback) {
                switch(bus->curSegment->callback(dev->callbackArg)) {
                case BUS_BUSY:
                    // Repeat the last DMA segment
                    bus->curSegment--;
                    break;

                case BUS_ABORT:
                    bus->curSegment = (busSegment_t *)BUS_SPI_FREE;
                    return;

                case BUS_READY:
                default:
                    // Advance to the next DMA segment
                    break;
                }
            }
            bus->curSegment++;
        }

        // If a following transaction has been linked, start it
        if (bus->curSegment->txData) {
            const extDevice_t *nextDev = (const extDevice_t *)bus->curSegment->txData;
            busSegment_t *nextSegments = (busSegment_t *)bus->curSegment->rxData;
            bus->curSegment->txData = NULL;
            spiSequenceStart(nextDev, nextSegments);
        } else {
            // The end of the segment list has been reached, so mark transactions as complete
            bus->curSegment = (busSegment_t *)BUS_SPI_FREE;
        }
    }
}

