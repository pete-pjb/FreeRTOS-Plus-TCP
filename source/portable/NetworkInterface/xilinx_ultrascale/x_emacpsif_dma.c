/*
 * FreeRTOS+TCP <DEVELOPMENT BRANCH>
 * Copyright (C) 2022 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://aws.amazon.com/freertos
 * http://www.FreeRTOS.org
 */

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_IP_Private.h"
#include "NetworkBufferManagement.h"

#include "x_emacpsif.h"
#include "x_topology.h"
#include "xstatus.h"

#include "xparameters.h"
#include "xparameters_ps.h"
#include "xil_exception.h"
#include "xil_mmu.h"

#include "uncached_memory.h"

/*
 * XPAR_SCUGIC_0_CPU_BASEADDR  0xF9020000U
 * XPAR_SCUGIC_0_DIST_BASEADDR 0xF9010000U
 */
#define INTC_BASE_ADDR         XPAR_SCUGIC_0_CPU_BASEADDR
#define INTC_DIST_BASE_ADDR    XPAR_SCUGIC_0_DIST_BASEADDR

#if ( ipconfigPACKET_FILLER_SIZE != 2 )
    #error Please define ipconfigPACKET_FILLER_SIZE as the value '2'
#endif
#define TX_OFFSET    ipconfigPACKET_FILLER_SIZE

#if ( ipconfigNETWORK_MTU > 1526 )
    #if ( ipconfigPORT_SUPPRESS_WARNING == 0 )
        #warning the use of Jumbo Frames has not been tested sufficiently yet.
    #endif

    #define USE_JUMBO_FRAMES    1
#endif /* ( ipconfigNETWORK_MTU > 1526 ) */

#if ( USE_JUMBO_FRAMES == 1 )
    #define dmaRX_TX_BUFFER_SIZE    ( 10240 - ipBUFFER_PADDING )
#else
    #define dmaRX_TX_BUFFER_SIZE    ( 1536 - ipBUFFER_PADDING )
#endif /* ( USE_JUMBO_FRAMES == 1 ) */

extern XScuGic xInterruptController;

/* Defined in NetworkInterface.c */
extern TaskHandle_t xEMACTaskHandles[ XPAR_XEMACPS_NUM_INSTANCES ];

/*
 *  pxDMA_tx_buffers: these are character arrays, each one is big enough to hold 1 MTU.
 *  The actual TX buffers are located in uncached RAM.
 */
static unsigned char * pxDMA_tx_buffers[ XPAR_XEMACPS_NUM_INSTANCES ][ ipconfigNIC_N_TX_DESC ];

/*
 *  pxDMA_rx_buffers: these are pointers to 'NetworkBufferDescriptor_t'.
 *  Once a message has been received by the EMAC, the descriptor can be passed
 *  immediately to the IP-task.
 */
static NetworkBufferDescriptor_t * pxDMA_rx_buffers[ XPAR_XEMACPS_NUM_INSTANCES ][ ipconfigNIC_N_RX_DESC ];

/*
 *  The FreeRTOS+TCP port is using a fixed 'topology', which is declared in
 *  ./portable/NetworkInterface/Zynq/NetworkInterface.c
 */
extern struct xtopology_t xXTopologies[ XPAR_XEMACPS_NUM_INSTANCES ];

static SemaphoreHandle_t xTXDescriptorSemaphore[ XPAR_XEMACPS_NUM_INSTANCES ];

/*
 *  The FreeRTOS+TCP port does not make use of "src/xemacps_bdring.c".
 *  In stead 'struct xemacpsif_s' has a "head" and a "tail" index.
 *  "head" is the next index to be written, used.
 *  "tail" is the next index to be read, freed.
 */

int is_tx_space_available( xemacpsif_s * xemacpsif )
{
    size_t uxCount;
    BaseType_t xEMACIndex = xemacpsif->emacps.Config.DeviceId;

    if( xTXDescriptorSemaphore[ xEMACIndex ] != NULL )
    {
        uxCount = ( ( UBaseType_t ) ipconfigNIC_N_TX_DESC ) - uxSemaphoreGetCount( xTXDescriptorSemaphore[ xEMACIndex ] );
    }
    else
    {
        uxCount = ( UBaseType_t ) 0u;
    }

    return uxCount;
}

void emacps_check_tx( xemacpsif_s * xemacpsif )
{
    int tail = xemacpsif->txTail;
    int head = xemacpsif->txHead;
    BaseType_t xEMACIndex = xemacpsif->emacps.Config.DeviceId;
    size_t uxCount = ( ( UBaseType_t ) ipconfigNIC_N_TX_DESC ) - uxSemaphoreGetCount( xTXDescriptorSemaphore[ xEMACIndex ] );

    /* uxCount is the number of TX descriptors that are in use by the DMA. */
    /* When done, "TXBUF_USED" will be set. */

    while( ( uxCount > 0 ) && ( ( xemacpsif->txSegments[ tail ].flags & XEMACPS_TXBUF_USED_MASK ) != 0 ) )
    {
        if( ( tail == head ) && ( uxCount != ipconfigNIC_N_TX_DESC ) )
        {
            break;
        }

        {
            void * pvBuffer = pxDMA_tx_buffers[ xEMACIndex ][ tail ];
            NetworkBufferDescriptor_t * pxBuffer;

            if( pvBuffer != NULL )
            {
                pxDMA_tx_buffers[ xEMACIndex ][ tail ] = NULL;
                pxBuffer = pxPacketBuffer_to_NetworkBuffer( pvBuffer );

                if( pxBuffer != NULL )
                {
                    vReleaseNetworkBufferAndDescriptor( pxBuffer );
                }
                else
                {
                    FreeRTOS_printf( ( "emacps_check_tx: Can not find network buffer\n" ) );
                }
            }
        }

        /* Clear all but the "used" and "wrap" bits. */
        if( tail < ipconfigNIC_N_TX_DESC - 1 )
        {
            xemacpsif->txSegments[ tail ].flags = XEMACPS_TXBUF_USED_MASK;
        }
        else
        {
            xemacpsif->txSegments[ tail ].flags = XEMACPS_TXBUF_USED_MASK | XEMACPS_TXBUF_WRAP_MASK;
        }

        uxCount--;
        /* Tell the counting semaphore that one more TX descriptor is available. */
        xSemaphoreGive( xTXDescriptorSemaphore[ xEMACIndex ] );

        if( ++tail == ipconfigNIC_N_TX_DESC )
        {
            tail = 0;
        }

        xemacpsif->txTail = tail;
    }
}

void emacps_send_handler( void * arg )
{
    xemacpsif_s * xemacpsif;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    BaseType_t xEMACIndex;

    xemacpsif = ( xemacpsif_s * ) arg;
    xEMACIndex = xemacpsif->emacps.Config.DeviceId;

    /* This function is called from an ISR. The Xilinx ISR-handler has already
     * cleared the TXCOMPL and TXSR_USEDREAD status bits in the XEMACPS_TXSR register.
     * But it forgets to do a read-back. Do so now to avoid ever-returning ISR's. */
    ( void ) XEmacPs_ReadReg( xemacpsif->emacps.Config.BaseAddress, XEMACPS_TXSR_OFFSET );

    /* In this port for FreeRTOS+TCP, the EMAC interrupts will only set a bit in
     * "isr_events". The task in NetworkInterface will wake-up and do the necessary work.
     */
    xemacpsif->isr_events |= EMAC_IF_TX_EVENT;
    xemacpsif->txBusy = pdFALSE;

    if( xEMACTaskHandles[ xEMACIndex ] != NULL )
    {
        vTaskNotifyGiveFromISR( xEMACTaskHandles[ xEMACIndex ], &xHigherPriorityTaskWoken );
    }

    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
}

static BaseType_t xValidLength( BaseType_t xLength )
{
    BaseType_t xReturn;

    if( ( xLength >= ( BaseType_t ) sizeof( struct xARP_PACKET ) ) && ( ( ( uint32_t ) xLength ) <= dmaRX_TX_BUFFER_SIZE ) )
    {
        xReturn = pdTRUE;
    }
    else
    {
        xReturn = pdFALSE;
    }

    return xReturn;
}

XStatus emacps_send_message( xemacpsif_s * xemacpsif,
                             NetworkBufferDescriptor_t * pxBuffer,
                             int iReleaseAfterSend )
{
    int head = xemacpsif->txHead;
    int iHasSent = 0;
    uint32_t ulBaseAddress = xemacpsif->emacps.Config.BaseAddress;
    BaseType_t xEMACIndex = xemacpsif->emacps.Config.DeviceId;
    TickType_t xBlockTimeTicks = pdMS_TO_TICKS( 5000U );

    /* This driver wants to own all network buffers which are to be transmitted. */
    configASSERT( iReleaseAfterSend != pdFALSE );

    /* Open a do {} while ( 0 ) loop to be able to call break. */
    do
    {
        uint32_t ulFlags = 0;

        if( xValidLength( pxBuffer->xDataLength ) != pdTRUE )
        {
            break;
        }

        if( xTXDescriptorSemaphore[ xEMACIndex ] == NULL )
        {
            break;
        }

        if( xSemaphoreTake( xTXDescriptorSemaphore[ xEMACIndex ], xBlockTimeTicks ) != pdPASS )
        {
            FreeRTOS_printf( ( "emacps_send_message: Time-out waiting for TX buffer\n" ) );
            break;
        }

        /* Pass the pointer (and its ownership) directly to DMA. */
        pxDMA_tx_buffers[ xEMACIndex ][ head ] = pxBuffer->pucEthernetBuffer;

        if( ucIsCachedMemory( pxBuffer->pucEthernetBuffer ) != 0 )
        {
            Xil_DCacheFlushRange( ( INTPTR ) pxBuffer->pucEthernetBuffer, ( INTPTR ) pxBuffer->xDataLength );
        }

        /* Buffer has been transferred, do not release it. */
        iReleaseAfterSend = pdFALSE;

        /* Packets will be sent one-by-one, so for each packet
         * the TXBUF_LAST bit will be set. */
        ulFlags |= XEMACPS_TXBUF_LAST_MASK;
        ulFlags |= ( pxBuffer->xDataLength & XEMACPS_TXBUF_LEN_MASK );

        if( head == ( ipconfigNIC_N_TX_DESC - 1 ) )
        {
            ulFlags |= XEMACPS_TXBUF_WRAP_MASK;
        }

        /* Copy the address of the buffer and set the flags. */
        xemacpsif->txSegments[ head ].address = ( uintptr_t ) pxDMA_tx_buffers[ xEMACIndex ][ head ];
        xemacpsif->txSegments[ head ].flags = ulFlags;

        iHasSent = pdTRUE;

        if( ++head == ipconfigNIC_N_TX_DESC )
        {
            head = 0;
        }

        /* Update the TX-head index. These variable are declared volatile so they will be
         * accessed as little as possible.	*/
        xemacpsif->txHead = head;
    } while( pdFALSE );

    if( iReleaseAfterSend != pdFALSE )
    {
        vReleaseNetworkBufferAndDescriptor( pxBuffer );
        pxBuffer = NULL;
    }

    /* Data Synchronization Barrier */
    dsb();

    if( iHasSent != pdFALSE )
    {
        /* Make STARTTX high */
        uint32_t ulValue = XEmacPs_ReadReg( ulBaseAddress, XEMACPS_NWCTRL_OFFSET );
        /* Start transmit */
        xemacpsif->txBusy = pdTRUE;
        XEmacPs_WriteReg( ulBaseAddress, XEMACPS_NWCTRL_OFFSET, ( ulValue | XEMACPS_NWCTRL_STARTTX_MASK ) );
        /* Read back the register to make sure the data is flushed. */
        ( void ) XEmacPs_ReadReg( ulBaseAddress, XEMACPS_NWCTRL_OFFSET );
    }

    dsb();

    return 0;
}

void emacps_recv_handler( void * arg )
{
    xemacpsif_s * xemacpsif;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    xemacpsif = ( xemacpsif_s * ) arg;
    xemacpsif->isr_events |= EMAC_IF_RX_EVENT;
    BaseType_t xEMACIndex = xemacpsif->emacps.Config.DeviceId;

    /* The driver has already cleared the FRAMERX, BUFFNA and error bits
     * in the XEMACPS_RXSR register,
     * But it forgets to do a read-back. Do so now. */
    ( void ) XEmacPs_ReadReg( xemacpsif->emacps.Config.BaseAddress, XEMACPS_RXSR_OFFSET );

    if( xEMACTaskHandles[ xEMACIndex ] != NULL )
    {
        vTaskNotifyGiveFromISR( xEMACTaskHandles[ xEMACIndex ], &xHigherPriorityTaskWoken );
    }

    portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
}

static void prvPassEthMessages( NetworkBufferDescriptor_t * pxDescriptor )
{
    IPStackEvent_t xRxEvent;

    xRxEvent.eEventType = eNetworkRxEvent;
    xRxEvent.pvData = ( void * ) pxDescriptor;

    if( xSendEventStructToIPTask( &xRxEvent, ( TickType_t ) 1000U ) != pdPASS )
    {
        /* The buffer could not be sent to the stack so	must be released again.
         * This is a deferred handler taskr, not a real interrupt, so it is ok to
         * use the task level function here. */
        #if ( ipconfigUSE_LINKED_RX_MESSAGES != 0 )
        {
            do
            {
                NetworkBufferDescriptor_t * pxNext = pxDescriptor->pxNextBuffer;
                vReleaseNetworkBufferAndDescriptor( pxDescriptor );
                pxDescriptor = pxNext;
            } while( pxDescriptor != NULL );
        }
        #else
        {
            vReleaseNetworkBufferAndDescriptor( pxDescriptor );
        }
        #endif /* ipconfigUSE_LINKED_RX_MESSAGES */
        iptraceETHERNET_RX_EVENT_LOST();
        FreeRTOS_printf( ( "prvPassEthMessages: Can not queue return packet!\n" ) );
    }
}

BaseType_t xMayAcceptPacket( uint8_t * pucEthernetBuffer )
{
    const ProtocolPacket_t * pxProtPacket = ( const ProtocolPacket_t * ) pucEthernetBuffer;

    switch( pxProtPacket->xTCPPacket.xEthernetHeader.usFrameType )
    {
        case ipARP_FRAME_TYPE:
            /* Check it later. */
            return pdTRUE;

        case ipIPv6_FRAME_TYPE:
            /* Check it later. */
            return pdTRUE;

        case ipIPv4_FRAME_TYPE:
            /* Check it here. */
            break;

        default:
            /* Refuse the packet. */
            return pdFALSE;
    }

    #if ( ipconfigETHERNET_DRIVER_FILTERS_PACKETS == 1 )
    {
        const IPHeader_t * pxIPHeader = &( pxProtPacket->xTCPPacket.xIPHeader );

        /* Ensure that the incoming packet is not fragmented (only outgoing packets
         * can be fragmented) as these are the only handled IP frames currently. */
        if( ( pxIPHeader->usFragmentOffset & ipFRAGMENT_OFFSET_BIT_MASK ) != 0U )
        {
            return pdFALSE;
        }

        /* HT: Might want to make the following configurable because
         * most IP messages have a standard length of 20 bytes */

        /* 0x45 means: IPv4 with an IP header of 5 x 4 = 20 bytes
         * 0x47 means: IPv4 with an IP header of 7 x 4 = 28 bytes */
        if( ( pxIPHeader->ucVersionHeaderLength < 0x45 ) || ( pxIPHeader->ucVersionHeaderLength > 0x4F ) )
        {
            return pdFALSE;
        }

        if( pxIPHeader->ucProtocol == ipPROTOCOL_UDP )
        {
            uint16_t usSourcePort = FreeRTOS_ntohs( pxProtPacket->xUDPPacket.xUDPHeader.usSourcePort );
            uint16_t usDestinationPort = FreeRTOS_ntohs( pxProtPacket->xUDPPacket.xUDPHeader.usDestinationPort );

            if( ( xPortHasUDPSocket( pxProtPacket->xUDPPacket.xUDPHeader.usDestinationPort ) == pdFALSE )
                #if ipconfigUSE_LLMNR == 1
                    && ( usDestinationPort != ipLLMNR_PORT ) &&
                    ( usSourcePort != ipLLMNR_PORT )
                #endif
                #if ipconfigUSE_NBNS == 1
                    && ( usDestinationPort != ipNBNS_PORT ) &&
                    ( usSourcePort != ipNBNS_PORT )
                #endif
                #if ipconfigUSE_DNS == 1
                    && ( usSourcePort != ipDNS_PORT )
                #endif
                )
            {
                /* Drop this packet, not for this device. */
                /* FreeRTOS_printf( ( "Drop: UDP port %d -> %d\n", usSourcePort, usDestinationPort ) ); */
                return pdFALSE;
            }
        }
    }
    #endif /* ipconfigETHERNET_DRIVER_FILTERS_PACKETS */
    return pdTRUE;
}

int emacps_check_rx( xemacpsif_s * xemacpsif,
                     NetworkInterface_t * pxInterface )
{
    NetworkBufferDescriptor_t * pxBuffer, * pxNewBuffer;
    int rx_bytes;
    volatile int msgCount = 0;
    int head = xemacpsif->rxHead;
    BaseType_t xEMACIndex = xemacpsif->emacps.Config.DeviceId;
    BaseType_t xAccepted;

    #if ( ipconfigUSE_LINKED_RX_MESSAGES != 0 )
        NetworkBufferDescriptor_t * pxFirstDescriptor = NULL;
        NetworkBufferDescriptor_t * pxLastDescriptor = NULL;
    #endif /* ipconfigUSE_LINKED_RX_MESSAGES */

    /* There seems to be an issue (SI# 692601), see comments below. */
    resetrx_on_no_rxdata( xemacpsif );

    /* This FreeRTOS+TCP driver shall be compiled with the option
     * "ipconfigUSE_LINKED_RX_MESSAGES" enabled.  It allows the driver to send a
     * chain of RX messages within one message to the IP-task.	*/
    for( ; ; )
    {
        if( ( ( xemacpsif->rxSegments[ head ].address & XEMACPS_RXBUF_NEW_MASK ) == 0 ) ||
            ( pxDMA_rx_buffers[ xEMACIndex ][ head ] == NULL ) )
        {
            break;
        }

        pxBuffer = ( NetworkBufferDescriptor_t * ) pxDMA_rx_buffers[ xEMACIndex ][ head ];
        xAccepted = xMayAcceptPacket( pxBuffer->pucEthernetBuffer );

        if( xAccepted == pdFALSE )
        {
            pxNewBuffer = NULL;
        }
        else
        {
            pxNewBuffer = pxGetNetworkBufferWithDescriptor( dmaRX_TX_BUFFER_SIZE, ( TickType_t ) 0 );

            if( pxNewBuffer == NULL )
            {
                /* A packet has been received, but there is no replacement for this Network Buffer.
                 * The packet will be dropped, and it Network Buffer will stay in place. */
                FreeRTOS_printf( ( "emacps_check_rx: unable to allocate a Network Buffer\n" ) );
            }
        }

        if( pxNewBuffer == NULL )
        {
            pxNewBuffer = ( NetworkBufferDescriptor_t * ) pxDMA_rx_buffers[ xEMACIndex ][ head ];
        }
        else
        {
            pxBuffer->pxInterface = pxInterface;
            pxBuffer->pxEndPoint = FreeRTOS_MatchingEndpoint( pxInterface, pxBuffer->pucEthernetBuffer );
            /* Just avoiding to use or refer to the same buffer again */
            pxDMA_rx_buffers[ xEMACIndex ][ head ] = pxNewBuffer;

            /*
             * Adjust the buffer size to the actual number of bytes received.
             * If port is built with Jumbo Frame support, then the XEMACPS_RXBUF_LEN_JUMBO_MASK
             * should be used to obtain the size of the buffer. Otherwise the mask
             * XEMACPS_RXBUF_LEN_MASK can be used.
             */
            #if ( USE_JUMBO_FRAMES == 1 )
            {
                rx_bytes = xemacpsif->rxSegments[ head ].flags & XEMACPS_RXBUF_LEN_JUMBO_MASK;
            }
            #else
            {
                rx_bytes = xemacpsif->rxSegments[ head ].flags & XEMACPS_RXBUF_LEN_MASK;
            }
            #endif /* ( USE_JUMBO_FRAMES == 1 ) */

            pxBuffer->xDataLength = rx_bytes;

            if( ucIsCachedMemory( pxBuffer->pucEthernetBuffer ) != 0 )
            {
                Xil_DCacheInvalidateRange( ( ( uintptr_t ) pxBuffer->pucEthernetBuffer ) - ipconfigPACKET_FILLER_SIZE, ( unsigned ) rx_bytes );
            }

            /* store it in the receive queue, where it'll be processed by a
             * different handler. */
            iptraceNETWORK_INTERFACE_RECEIVE();
            #if ( ipconfigUSE_LINKED_RX_MESSAGES != 0 )
            {
                pxBuffer->pxNextBuffer = NULL;

                if( pxFirstDescriptor == NULL )
                {
                    /* Becomes the first message */
                    pxFirstDescriptor = pxBuffer;
                }
                else if( pxLastDescriptor != NULL )
                {
                    /* Add to the tail */
                    pxLastDescriptor->pxNextBuffer = pxBuffer;
                }

                pxLastDescriptor = pxBuffer;
            }
            #else /* if ( ipconfigUSE_LINKED_RX_MESSAGES != 0 ) */
            {
                prvPassEthMessages( pxBuffer );
            }
            #endif /* ipconfigUSE_LINKED_RX_MESSAGES */

            msgCount++;
        }

        {
            if( ucIsCachedMemory( pxNewBuffer->pucEthernetBuffer ) != 0 )
            {
                Xil_DCacheInvalidateRange( ( ( uintptr_t ) pxNewBuffer->pucEthernetBuffer ) - ipconfigPACKET_FILLER_SIZE, ( uint32_t ) dmaRX_TX_BUFFER_SIZE );
            }

            {
                uintptr_t addr = ( ( uintptr_t ) pxNewBuffer->pucEthernetBuffer ) & XEMACPS_RXBUF_ADD_MASK;

                if( head == ( ipconfigNIC_N_RX_DESC - 1 ) )
                {
                    addr |= XEMACPS_RXBUF_WRAP_MASK;
                }

                /* Clearing 'XEMACPS_RXBUF_NEW_MASK'       0x00000001 *< Used bit.. */
                xemacpsif->rxSegments[ head ].flags = 0;
                xemacpsif->rxSegments[ head ].address = addr;
                /* Make sure that the value has reached the peripheral by reading it back. */
                ( void ) xemacpsif->rxSegments[ head ].address;
            }
        }

        if( ++head == ipconfigNIC_N_RX_DESC )
        {
            head = 0;
        }

        xemacpsif->rxHead = head;
    }

    #if ( ipconfigUSE_LINKED_RX_MESSAGES != 0 )
    {
        if( pxFirstDescriptor != NULL )
        {
            prvPassEthMessages( pxFirstDescriptor );
        }
    }
    #endif /* ipconfigUSE_LINKED_RX_MESSAGES */

    return msgCount;
}

void clean_dma_txdescs( xemacpsif_s * xemacpsif )
{
    int index;
    BaseType_t xEMACIndex = xemacpsif->emacps.Config.DeviceId;

    for( index = 0; index < ipconfigNIC_N_TX_DESC; index++ )
    {
        xemacpsif->txSegments[ index ].address = ( uintptr_t ) NULL;
        xemacpsif->txSegments[ index ].flags = XEMACPS_TXBUF_USED_MASK;
        pxDMA_tx_buffers[ xEMACIndex ][ index ] = ( unsigned char * ) NULL;
    }

    xemacpsif->txSegments[ ipconfigNIC_N_TX_DESC - 1 ].flags =
        XEMACPS_TXBUF_USED_MASK | XEMACPS_TXBUF_WRAP_MASK;
}

XStatus init_dma( xemacpsif_s * xemacpsif )
{
    NetworkBufferDescriptor_t * pxBuffer;
    BaseType_t xEMACIndex = xemacpsif->emacps.Config.DeviceId;

    int iIndex;
    UBaseType_t xRxSize;
    UBaseType_t xTxSize;
    struct xtopology_t * xtopologyp = &xXTopologies[ xEMACIndex ];
    XEmacPs_BdRing * rxRing;
    XEmacPs * emac = &( xemacpsif->emacps );
    XEmacPs_Bd bdTemplate;
    XEmacPs_Bd * dmaBdPtr = NULL;
    uint32_t status;

    xRxSize = ipconfigNIC_N_RX_DESC * sizeof( xemacpsif->rxSegments[ 0 ] );

    xTxSize = ipconfigNIC_N_TX_DESC * sizeof( xemacpsif->txSegments[ 0 ] );

    xemacpsif->uTxUnitSize = ( dmaRX_TX_BUFFER_SIZE + 0x1000UL ) & ~0xfffUL;

    /*
     * We allocate 65536 bytes for RX BDs which can accommodate a
     * maximum of 8192 BDs which is much more than any application
     * will ever need.
     */
    xemacpsif->rxSegments = ( struct xBD_TYPE * ) ( pucGetUncachedMemory( xRxSize ) );
    xemacpsif->txSegments = ( struct xBD_TYPE * ) ( pucGetUncachedMemory( xTxSize ) );

    configASSERT( xemacpsif->rxSegments );
    configASSERT( xemacpsif->txSegments );
    configASSERT( ( ( ( uintptr_t ) xemacpsif->rxSegments ) % XEMACPS_DMABD_MINIMUM_ALIGNMENT ) == 0 );
    configASSERT( ( ( ( uintptr_t ) xemacpsif->txSegments ) % XEMACPS_DMABD_MINIMUM_ALIGNMENT ) == 0 );


    rxRing = &( XEmacPs_GetRxRing( emac ) );
    XEmacPs_BdClear( bdTemplate );

    status = XEmacPs_BdRingCreate( rxRing, ( UINTPTR ) xemacpsif->rxSegments,
                                   ( UINTPTR ) xemacpsif->rxSegments, XEMACPS_DMABD_MINIMUM_ALIGNMENT,
                                   ipconfigNIC_N_RX_DESC );

    if( status != 0 )
    {
        return status;
    }

    status = XEmacPs_BdRingClone( rxRing, &bdTemplate, XEMACPS_RECV );

    if( status != 0 )
    {
        return status;
    }

    if( xTXDescriptorSemaphore[ xEMACIndex ] == NULL )
    {
        xTXDescriptorSemaphore[ xEMACIndex ] = xSemaphoreCreateCounting( ( UBaseType_t ) ipconfigNIC_N_TX_DESC, ( UBaseType_t ) ipconfigNIC_N_TX_DESC );
        configASSERT( xTXDescriptorSemaphore[ xEMACIndex ] != NULL );
    }

    /*
     * Allocate RX descriptors, 1 RxBD at a time.
     */
    for( iIndex = 0; iIndex < ipconfigNIC_N_RX_DESC; iIndex++ )
    {
        pxBuffer = pxDMA_rx_buffers[ xEMACIndex ][ iIndex ];

        if( pxBuffer == NULL )
        {
            pxBuffer = pxGetNetworkBufferWithDescriptor( dmaRX_TX_BUFFER_SIZE, ( TickType_t ) 0 );

            if( pxBuffer == NULL )
            {
                FreeRTOS_printf( ( "Unable to allocate a network buffer in recv_handler\n" ) );
                return -1;
            }
        }

        status = XEmacPs_BdRingAlloc( rxRing, 1, &dmaBdPtr );

        if( status != 0 )
        {
            return status;
        }

        XEmacPs_BdSetAddressRx( dmaBdPtr,
                                ( ( uintptr_t ) pxBuffer->pucEthernetBuffer ) & XEMACPS_RXBUF_ADD_MASK );

        status = XEmacPs_BdRingToHw( rxRing, 1, dmaBdPtr );

        if( status != 0 )
        {
            return status;
        }

        /* Writing for debug - can look at it in RX processing */
        #ifdef __aarch64__
            xemacpsif->rxSegments[ iIndex ].reserved = iIndex;
        #endif

        pxDMA_rx_buffers[ xEMACIndex ][ iIndex ] = pxBuffer;

        /* Make sure this memory is not in cache for now. */
        if( ucIsCachedMemory( pxBuffer->pucEthernetBuffer ) != 0 )
        {
            Xil_DCacheInvalidateRange( ( ( uintptr_t ) pxBuffer->pucEthernetBuffer ) - ipconfigPACKET_FILLER_SIZE,
                                       ( unsigned ) dmaRX_TX_BUFFER_SIZE );
        }
    }

    xemacpsif->rxSegments[ ipconfigNIC_N_RX_DESC - 1 ].address |= XEMACPS_RXBUF_WRAP_MASK;

    clean_dma_txdescs( xemacpsif );

    {
        uint32_t value;
        value = XEmacPs_ReadReg( xemacpsif->emacps.Config.BaseAddress, XEMACPS_DMACR_OFFSET );

        /* 1xxxx: Attempt to use INCR16 AHB bursts */
        value = ( value & ~( XEMACPS_DMACR_BLENGTH_MASK ) ) | XEMACPS_DMACR_INCR16_AHB_BURST;
        #if ( ipconfigDRIVER_INCLUDED_TX_IP_CHECKSUM != 0 )
            value |= XEMACPS_DMACR_TCPCKSUM_MASK;
        #else
        #if ( ipconfigPORT_SUPPRESS_WARNING == 0 )
            {
                #warning Are you sure the EMAC should not calculate outgoing checksums?
            }
            #endif

            value &= ~XEMACPS_DMACR_TCPCKSUM_MASK;
        #endif
        XEmacPs_WriteReg( xemacpsif->emacps.Config.BaseAddress, XEMACPS_DMACR_OFFSET, value );
    }
    {
        uint32_t value;
        value = XEmacPs_ReadReg( xemacpsif->emacps.Config.BaseAddress, XEMACPS_NWCFG_OFFSET );

        /* Network buffers are 32-bit aligned + 2 bytes (because ipconfigPACKET_FILLER_SIZE = 2 ).
         * Now tell the EMAC that received messages should be stored at "address + 2". */
        value = ( value & ~XEMACPS_NWCFG_RXOFFS_MASK ) | 0x8000;

        #if ( ipconfigDRIVER_INCLUDED_RX_IP_CHECKSUM != 0 )
            value |= XEMACPS_NWCFG_RXCHKSUMEN_MASK;
        #else
        #if ( ipconfigPORT_SUPPRESS_WARNING == 0 )
            {
                #warning Are you sure the EMAC should not calculate incoming checksums?
            }
            #endif

            value &= ~( ( uint32_t ) XEMACPS_NWCFG_RXCHKSUMEN_MASK );
        #endif
        XEmacPs_WriteReg( xemacpsif->emacps.Config.BaseAddress, XEMACPS_NWCFG_OFFSET, value );
    }

    /* Set terminating BDs for US+ GEM */
    xemacpsif->rxBdTerminator = ( struct xBD_TYPE * ) pucGetUncachedMemory( sizeof( *xemacpsif->rxBdTerminator ) );
    xemacpsif->txBdTerminator = ( struct xBD_TYPE * ) pucGetUncachedMemory( sizeof( *xemacpsif->txBdTerminator ) );

    XEmacPs_BdClear( xemacpsif->rxBdTerminator );
    XEmacPs_BdSetAddressRx( xemacpsif->rxBdTerminator, ( XEMACPS_RXBUF_NEW_MASK |
                                                         XEMACPS_RXBUF_WRAP_MASK ) );
    XEmacPs_Out32( ( emac->Config.BaseAddress + XEMACPS_RXQ1BASE_OFFSET ),
                   ( UINTPTR ) xemacpsif->rxBdTerminator );

    XEmacPs_BdClear( xemacpsif->txBdTerminator );
    XEmacPs_BdSetStatus( xemacpsif->txBdTerminator,
                         ( XEMACPS_TXBUF_USED_MASK | XEMACPS_TXBUF_WRAP_MASK ) );
    XEmacPs_Out32( ( emac->Config.BaseAddress + XEMACPS_TXQBASE_OFFSET ),
                   ( UINTPTR ) xemacpsif->txBdTerminator );

    /* These variables will be used in XEmacPs_Start (see src/xemacps.c). */
    XEmacPs_SetQueuePtr( emac, ( uintptr_t ) xemacpsif->rxSegments, 0, XEMACPS_RECV );

    XEmacPs_SetQueuePtr( emac, ( uintptr_t ) xemacpsif->txSegments, 1, XEMACPS_SEND );

    XScuGic_Connect( &xInterruptController, xtopologyp->scugic_emac_intr, XEmacPs_IntrHandler, emac );

    /*
     * Enable the interrupt for emacps.
     */
    EmacEnableIntr( xEMACIndex );

    return 0;
}

/*
 * resetrx_on_no_rxdata():
 *
 * It is called at regular intervals through the API xemacpsif_resetrx_on_no_rxdata
 * called by the user.
 * The EmacPs has a HW bug (SI# 692601) on the Rx path for heavy Rx traffic.
 * Under heavy Rx traffic because of the HW bug there are times when the Rx path
 * becomes unresponsive. The workaround for it is to check for the Rx path for
 * traffic (by reading the stats registers regularly). If the stats register
 * does not increment for sometime (proving no Rx traffic), the function resets
 * the Rx data path.
 *
 */

void resetrx_on_no_rxdata( xemacpsif_s * xemacpsif )
{
    uint32_t regctrl;
    uint32_t tempcntr;

    tempcntr = XEmacPs_ReadReg( xemacpsif->emacps.Config.BaseAddress, XEMACPS_RXCNT_OFFSET );

    if( ( tempcntr == 0 ) && ( xemacpsif->last_rx_frms_cntr == 0 ) )
    {
        regctrl = XEmacPs_ReadReg( xemacpsif->emacps.Config.BaseAddress,
                                   XEMACPS_NWCTRL_OFFSET );
        regctrl &= ( ~XEMACPS_NWCTRL_RXEN_MASK );
        XEmacPs_WriteReg( xemacpsif->emacps.Config.BaseAddress,
                          XEMACPS_NWCTRL_OFFSET, regctrl );
        regctrl = XEmacPs_ReadReg( xemacpsif->emacps.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET );
        regctrl |= ( XEMACPS_NWCTRL_RXEN_MASK );
        XEmacPs_WriteReg( xemacpsif->emacps.Config.BaseAddress, XEMACPS_NWCTRL_OFFSET, regctrl );
    }

    xemacpsif->last_rx_frms_cntr = tempcntr;
}

void EmacDisableIntr( int xEMACIndex )
{
    XScuGic_Disable( &xInterruptController, xXTopologies[ xEMACIndex ].scugic_emac_intr );
}

void EmacEnableIntr( int xEMACIndex )
{
    XScuGic_Enable( &xInterruptController, xXTopologies[ xEMACIndex ].scugic_emac_intr );
}
