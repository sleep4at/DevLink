/********************************** (C) COPYRIGHT *******************************
 * File Name          : rf_basic.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2024/08/15
 * Description        : ÎÞÏß´®¿Ú-½ÓÊÕ¶Ë
 *
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *******************************************************************************/

/******************************************************************************/
/* Í·ÎÄ¼þ°üº¬ */
#include "rf.h"
#include "rf_uart_rx.h"
#include "usb_uart.h"


uint8_t volatile RF_bound_Flag;

/*********************************************************************
 * GLOBAL TYPEDEFS
 */
static uint8_t rf_buf[RF_BUF_LEN];
static struct simple_buf *pRfBuf = NULL;
static struct simple_buf rf_buffer;
uint32_t gBaudRate;

/* Runtime diagnostics; these do not alter the on-air packet format. */
volatile uint32_t gRfBackpressureCount;
volatile uint32_t gRfCreditZeroCount;

rfTxBuf_t gTxBuf;
uint32_t gSysClock;
uint32_t gRfTxCount;
uint32_t gIntervalTimer;
uint16_t gServerData; // ÐèÒªµôµç±£´æ£¬Ðè±£´æÖÁflash
uint16_t gTimeoutMax;
uint16_t gTimeout;

uint8_t gDataSeq;
uint8_t gRfStatus;
uint8_t gBoundStatus;
uint8_t gRxDataStatus;

static void rfProcessRx( rfPackage_t *pPkt );
static void rfProcessTx( void );
static void rfProcessCrcError( void );
static  void rfProcessTimeout( void );
// GAP Service Callbacks
rfStatusCBs_t rfCBs =
{
    rfProcessRx,
    rfProcessTx,
    rfProcessCrcError,
    rfProcessTimeout,
};

/*********************************************************************
 * @fn      uart_buffer_create
 *
 * @brief   Create a file called uart_buffer simple buffer of the buffer
 *          and assign its address to the variable pointed to by the buffer pointer.
 *
 * @param   buf    -   a parameter buf pointing to a pointer.
 *
 * @return  none
 */
static void rf_buffer_create(struct simple_buf **buf)
{
    *buf = simple_buf_create(&rf_buffer, rf_buf, sizeof(rf_buf) );
}

/* Convert the Device's 16-byte credit units to a legal RF payload length. */
static typeBufSize Host_CreditLength( uint8_t credit )
{
    uint32_t len = (uint32_t)credit * RF_CREDIT_UNIT;
    return (len > DATA_LEN_MAX_TX) ? DATA_LEN_MAX_TX : (typeBufSize)len;
}

/*******************************************************************************
 * @fn      rf_rand
 *
 * @brief   Ëæ»úÊýÉú³Éº¯Êý
 *
 * @return  None.
 */
__HIGH_CODE
uint32_t rf_rand16( uint32_t seed )
{
    static uint32_t holdrand;
    uint64_t tmp;

    holdrand += seed;
    holdrand = holdrand * 1664525 + 1013904223;
    tmp = holdrand;
    tmp = (tmp*0xFFFF)>>32;
    return tmp;
}

/*******************************************************************************
 * @fn      rf_rand
 *
 * @brief   Ëæ»úÊýÉú³Éº¯Êý
 *
 * @return  None.
 */
__HIGH_CODE
uint32_t rf_rand_aa( uint16_t rand )
{
    uint32_t aa = rand;

    aa = (aa<<8)|0x6E0000B6;
    return aa;
}

/*******************************************************************************
 * @fn      rf_disconnect
 *
 * @brief   ¶Ï¿ªÁ¬½Ó
 *
 * @param   None.
 *
 * @return  None.
 */
static void rf_disconnect( void )
{
    RF_bound_Flag = 0;
    PRINT("disconnect.\n" );
    RFRole_Shut( );
    gBoundStatus = BOUND_STATUS_IDLE;
    gRxDataStatus = DATA_STATUS_START;
    TMR_ITCfg(DISABLE, TMR_IT_CYC_END); // ¹Ø±ÕÖÐ¶Ï

    rf_tx_set_sync_word( AA );
    rf_tx_set_frequency( DEF_FREQUENCY );
    rf_rx_set_sync_word( AA );
    rf_rx_set_frequency( DEF_FREQUENCY );
    gRfStatus = RF_STATUS_WAIT;
    gServerData = 0;
}

/*******************************************************************************
 * @fn      rf_bound
 *
 * @brief   ¶Ï¿ªÁ¬½Ó
 *
 * @param   None.
 *
 * @return  None.
 */
static void rf_bound( bound_rsp_t *rsp )
{
    gTimeout = 0;
    gBoundStatus = BOUND_STATUS_WAIT;
    rf_tx_set_sync_word( rsp->accessaddr );
    rf_tx_set_frequency( rsp->channel );
    rf_tx_set_phy_type( rsp->phy );

    rf_rx_set_sync_word( rsp->accessaddr );
    rf_rx_set_frequency( rsp->channel );
    rf_rx_set_phy_type( rsp->phy );
    RF_bound_Flag = 1;
    PRINT("bound success.%X %x\n",rsp->accessaddr,rsp->channel );
}

/*******************************************************************************
 * @fn      RF_ProcessRx
 *
 * @brief
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
static void rfProcessRx( rfPackage_t *pPkt )
{
    rfPackage_t *pPkt_t = (rfPackage_t *)gTxBuf.TxBuf;

    if( gBoundStatus == BOUND_STATUS_IDLE )
    {
        if( pPkt->type == PKT_CMD_BOUND_REQ )
        {
            BOOL reg=0;
            bound_req_t  *pReq_t =  ( bound_req_t  *)(pPkt+1);
            if( pPkt->length == sizeof(bound_req_t) + 2 )
            {
                int8_t rssi  = *(uint8_t* )((uint8_t* )pPkt + pPkt->length + 2 +2 );

                // ·ÇµÚÒ»´ÎÁ¬½Ó£¬µØÖ·Æ¥Åä¿ÉÁ¬
                if( pReq_t->severData )
                {
                    // ÐÅÏ¢Æ¥Åä£¬»òÕßdongleÖØÐÂÉÏµçÁË
                    if( !gServerData || pReq_t->severData == gServerData )
                    {
                        reg = 1;
                    }
                }
                else
                {
                    // µÚÒ»´ÎÁ¬½Ó£¬Ðè¿¿½üÁ¬½Ó
                    if( rssi > -35 )
                    {
                        reg = 1;
                    }
                }
                if( reg )
                {
                    bound_rsp_t *pRsp_t = (bound_rsp_t *)&gTxBuf.TxBuf[PKT_HEAD_LEN];

                    gRfStatus = RF_STATUS_TXRSP;
                    // Éú³ÉÏÂ´Î»ØÁ¬µÄËæ»úÐÅÏ¢
                    gDataSeq = 0;
                    gServerData = rf_rand16( rssi );
                    pPkt_t->type = PKT_CMD_BOUND_RSP;
                    pPkt_t->length = PKT_DATA_OFFSET+sizeof(bound_rsp_t);
                    pPkt_t->seq = gDataSeq;
                    pPkt_t->resv = 0;
                    pRsp_t->accessaddr = rf_rand_aa( gServerData );
                    pRsp_t->channel = gServerData&0x3F;
                    pRsp_t->phy = CONN_PHY_TYPE; // 2M
                    pRsp_t->severData = gServerData;
                    pRsp_t->interval = CONN_INTERVAL;
                    pRsp_t->timeout = CONN_TIMEOUT;
                    rf_tx_start( pPkt_t, 20 );
                    gDataSeq++;
                    gIntervalTimer = pReq_t->interval*1000;
                    gTimeoutMax = BOUND_EST_COUNT;
                    return ;
                }
                else
                {
                    PRINT(" reject..\n");
                }
                PRINT( "rssi=%d \n",rssi);
            }
        }
        gRfStatus = RF_STATUS_WAIT;
    }
    else
    {
        rfRsp_t *pRsp_t = (rfRsp_t *)&gTxBuf.TxBuf[PKT_HEAD_LEN];
        if( pPkt->type == PKT_CMD_GET_STATUS )
        {
            gRfStatus = RF_STATUS_TX;
            if( pPkt->seq == gDataSeq )
            {
                uint8_t s;
                typeBufSize len = DATA_LEN_MAX_TX;
                len = Host_CreditLength( pPkt->resv );
                if( !len )
                {
                    gRfCreditZeroCount++;
                }
                pPkt_t->type = PKT_CMD_RSP_STATUS;
                if( gBoundStatus < BOUND_STATUS_EST  )
                {
                    // ·¢ËÍ´®¿Ú²¨ÌØÂÊ
                    pPkt_t->length = PKT_DATA_OFFSET+1+sizeof(Uart0Para);
                    pRsp_t->opcode = OPCODE_BSP;
                    pRsp_t->buad_t.BaudRate = Uart0Para.BaudRate;
                    pRsp_t->buad_t.StopBits = Uart0Para.StopBits;
                    pRsp_t->buad_t.ParityType = Uart0Para.ParityType;
                    pRsp_t->buad_t.DataBits = Uart0Para.DataBits;
                    pRsp_t->buad_t.ioStaus = Uart0Para.ioStaus;
                    pPkt_t->seq = gDataSeq;
                    pPkt_t->resv = 0;
                }
                else
                {
                    s = USB_RxQuery( pRsp_t->other.rspData, &len );
                    if( s == 0 )
                    {
                        pPkt_t->length = PKT_DATA_OFFSET+1+len;
                        pRsp_t->opcode = OPCODE_DATA;
                        pPkt_t->seq = gDataSeq;
                        pPkt_t->resv = 0;
                        gRfTxCount += len;
                    }
                    else if( s == 0x80 )
                    {
                        // ·¢ËÍ´®¿Ú²¨ÌØÂÊ
                        pPkt_t->length = PKT_DATA_OFFSET+1+sizeof(Uart0Para);
                        pRsp_t->opcode = OPCODE_BSP;
                        pRsp_t->buad_t.BaudRate = Uart0Para.BaudRate;
                        pRsp_t->buad_t.StopBits = Uart0Para.StopBits;
                        pRsp_t->buad_t.ParityType = Uart0Para.ParityType;
                        pRsp_t->buad_t.DataBits = Uart0Para.DataBits;
                        pRsp_t->buad_t.ioStaus = Uart0Para.ioStaus;
                        pPkt_t->seq = gDataSeq;
                        pPkt_t->resv = 0;
                    }
                    else
                    {
                        pPkt_t->length = PKT_DATA_OFFSET;
                        pRsp_t->opcode = OPCODE_ACK;
                        pPkt_t->seq = gDataSeq;
                        pPkt_t->resv = 0;
                    }
                }
                /* Carry the PA2 key event in the status response. */
                pPkt_t->resv = Host_BootRequestTake() ? RF_CTRL_ENTER_BOOT : 0;
                gDataSeq++;
            }
            rf_tx_start( pPkt_t, 20 );
            if( pRsp_t->opcode == OPCODE_BSP )
            {
                PRINT("uart param.\n");
            }
        }
        else if( pPkt->type == PKT_DATA_FLAG )
        {

            gRfStatus = RF_STATUS_TX;
            if( pPkt->seq == gDataSeq )
            {
                typeBufSize payload_len = pPkt->length - PKT_DATA_OFFSET;
                typeBufSize accepted_len = payload_len;
                if( payload_len ) HostLedActivity();
                write_buf( pRfBuf, (pPkt+1), &accepted_len );
                if( accepted_len != payload_len )
                {
                    /* Do not ACK a packet that did not fit. The Device TX
                     * side will time out and retransmit the same sequence. */
                    gRfBackpressureCount++;
                    gRfStatus = RF_STATUS_WAIT;
                    rf_rx_start( gIntervalTimer );
                    return;
                }
                gRxDataStatus = DATA_STATUS_RCV;
                /* The packet was accepted into pRfBuf.  Reverse USB data is
                 * handled below using the Device-provided credit. */
                /* Limit reverse data in this ACK by the credit attached to
                 * the Device data packet.  A zero-credit ACK is intentional:
                 * it acknowledges the received bytes without consuming more
                 * Device-side UART buffer space. */
                typeBufSize len = Host_CreditLength( pPkt->resv );
                if( !len )
                {
                    gRfCreditZeroCount++;
                }
                pPkt_t->type = PKT_DATA_RSP_ACK;
                
                if( gBoundStatus == BOUND_STATUS_EST && !USB_RxQuery( pRsp_t->other.rspData, &len ) )
                {
                    pPkt_t->length = PKT_DATA_OFFSET+1+len;
                    pRsp_t->opcode = OPCODE_DATA;
                    pPkt_t->seq = gDataSeq;
                    pPkt_t->resv = 0;
                    gRfTxCount += len;
                }
                else
                {
                    pPkt_t->length = PKT_DATA_OFFSET;
                    pPkt_t->seq = gDataSeq;
                    pPkt_t->resv = 0;
                }
                gDataSeq++;
            }
            else
            {
                // ÖØ´«
            }
            rf_tx_start( pPkt_t, 20 );
            
        }
        else
        {
            PRINT("error data.\n");
            if( ++gTimeout > gTimeoutMax )
            {
                gRxDataStatus = DATA_STATUS_TIMEOUT;
                PRINT("connect timeout.\n");
            }
            else
            {
                gRfStatus = RF_STATUS_WAIT;
            }
            return ;
        }
        if( gBoundStatus == BOUND_STATUS_WAIT )
        {
            // Á¬½ÓÈ·ÈÏ£¬Æô¶¯Á¬½ÓÍ¨ÐÅ³¬Ê±
            gBoundStatus = BOUND_STATUS_EST;
            gIntervalTimer = CONN_INTERVAL*1000;
            gTimeoutMax = (CONN_TIMEOUT*10/CONN_INTERVAL);
       }
       gTimeout = 0;
    }
    
}

/*******************************************************************************
 * @fn      RF_ProcessRx
 *
 * @brief
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
static void rfProcessTx( void )
{
    if( gRfStatus == RF_STATUS_TXRSP )
    {
         rf_bound( (bound_rsp_t *)&gTxBuf.TxBuf[PKT_HEAD_LEN] );
    }
    gRfStatus = RF_STATUS_RX;
    rf_rx_start( gIntervalTimer );
}

/*******************************************************************************
 * @fn      RF_ProcessRxError
 *
 * @brief
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
static void rfProcessCrcError( void )
{
    gRfStatus = RF_STATUS_WAIT;
}

/*******************************************************************************
 * @fn      RF_ProcessTimeout
 *
 * @brief
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
static void rfProcessTimeout( void )
{
    // PRINT("r -timeout %x\n",gRfStatus);
    if( gBoundStatus && ++gTimeout > gTimeoutMax )
    {
        gRxDataStatus = DATA_STATUS_TIMEOUT;
        PRINT("connect timeout.\n");
    }
    else
    {
        gRfStatus = RF_STATUS_WAIT;
    }
}

/*******************************************************************************
 * @fn      RF_RxQuery
 *
 * @brief
 *
 * @return  None.
 */
__HIGH_CODE
uint8_t RF_RxQuery( void *buf, typeBufSize *len )
{

    uint8_t *p;

    if( gRxDataStatus == DATA_STATUS_RCV )
    {
        if( read_buf( pRfBuf, buf, len ) == 0 )
        {
            PFIC_DisableIRQ(BLEL_IRQn);
            gRxDataStatus = DATA_STATUS_START;
            PFIC_EnableIRQ(BLEL_IRQn);
        }
    }
    else
    {
        *len = 0;
        RF_RxService( );
    }
    return *len;
}

/* Keep the radio receive state machine running independently of USB mode.
 * CDC mode does not call RF_RxQuery() through the vendor bulk-IN path, but
 * pairing and RF timeouts still need to be serviced. */
__HIGH_CODE
void RF_RxService( void )
{
    if( gRxDataStatus == DATA_STATUS_TIMEOUT )
    {
        rf_disconnect( );
    }
    else if( gRxDataStatus != DATA_STATUS_RCV && gRfStatus == RF_STATUS_WAIT )
    {
        gRfStatus = RF_STATUS_RX;
        rf_rx_start( gIntervalTimer );
    }
}

/*******************************************************************************
 * @fn      RF_UartTxInit
 *
 * @brief   RF uart·¢ËÍÓ¦ÓÃ³õÊ¼»¯
 *
 * @param   None.
 *
 * @return  None.
 */
__HIGH_CODE
void RF_UartRxInit( void )
{
    PRINT("----------------- rf uart rx mode -----------------\n");
    gRxDataStatus = DATA_STATUS_IDLE;
    rf_buffer_create(&pRfBuf);
    gDataSeq = 0;
    gRfBackpressureCount = 0;
    gRfCreditZeroCount = 0;
    gServerData = 0;
    gBoundStatus = BOUND_STATUS_IDLE;
    gTxBuf.status = 0;
    gIntervalTimer = CONN_INTERVAL*1000;
    gSysClock = GetSysClock( );
    RFRole_RegisterStatusCbs( &rfCBs );
    gRfStatus = RF_STATUS_WAIT;
}

/******************************** endfile @rf ******************************/
