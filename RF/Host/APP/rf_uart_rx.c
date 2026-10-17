/********************************** (C) COPYRIGHT *******************************
 * File Name          : rf_basic.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2024/08/15
 * Description        : 无线串口-接收端
 *
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *******************************************************************************/

/******************************************************************************/
/* 头文件包含 */
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
uint16_t gServerData; // 需要掉电保存，需保存至flash
uint16_t gTimeoutMax;
uint16_t gTimeout;

volatile uint8_t gDataSeq;
volatile uint8_t gRfStatus;
volatile uint8_t gBoundStatus;
volatile uint8_t gRxDataStatus;
/* The last ACK is kept in gTxBuf.  Remember which received DATA packet it
 * acknowledges so a lost ACK can be retransmitted without delivering the
 * same UART bytes twice. */
static uint8_t gLastDataAckSeq;
static uint8_t gLastDataAckValid;
static uint8_t gLastStatusRspSeq;
static uint8_t gLastStatusRspValid;

static uint8_t rf_packet_valid( const rfPackage_t *pPkt )
{
    if( !pPkt || pPkt->length < PKT_DATA_OFFSET ||
        pPkt->length > DATA_LEN_MAX_RX )
        return 0;
    switch( pPkt->type )
    {
        case PKT_CMD_BOUND_REQ:
            return pPkt->length == PKT_DATA_OFFSET + sizeof(bound_req_t);
        case PKT_CMD_BOUND_RSP:
            return pPkt->length == PKT_DATA_OFFSET + sizeof(bound_rsp_t);
        case PKT_CMD_GET_STATUS:
            return pPkt->length == PKT_DATA_OFFSET;
        case PKT_CMD_RSP_STATUS:
            if( pPkt->length == PKT_DATA_OFFSET ) return 1;
            if( pPkt->length < PKT_DATA_OFFSET + 1 ) return 0;
            {
                const rfRsp_t *rsp = (const rfRsp_t *)(pPkt + 1);
                if( rsp->opcode == OPCODE_BSP )
                    return pPkt->length == PKT_DATA_OFFSET + 1 + sizeof(Uart0Para);
                if( rsp->opcode == OPCODE_DATA )
                    return pPkt->length > PKT_DATA_OFFSET + 1;
                if( rsp->opcode == OPCODE_ACK )
                    return pPkt->length == PKT_DATA_OFFSET + 1;
                return 0;
            }
        case PKT_DATA_FLAG:
            return pPkt->length <= PKT_DATA_OFFSET + DATA_LEN_MAX_TX;
        case PKT_DATA_RSP_ACK:
            if( pPkt->length == PKT_DATA_OFFSET ) return 1;
            if( pPkt->length < PKT_DATA_OFFSET + 2 ) return 0;
            return ((const rfRsp_t *)(pPkt + 1))->opcode == OPCODE_DATA;
        default:
            return 0;
    }
}

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

/* Status responses share resv with the existing one-bit boot control.  Encode
 * Host receive capacity in 32-byte units in bits 1..6 and mark it valid in
 * bit 7; ACK packets use 16-byte units plus the same validity marker. */
static uint8_t Host_StatusCredit( void )
{
    uint32_t free_len = 0;
    uint32_t credit;

    if( pRfBuf )
    {
        uint32_t irq_state = __risc_v_disable_irq();
        free_len = pRfBuf->buf_len - pRfBuf->data_len;
        __risc_v_enable_irq( irq_state );
    }
    credit = free_len / (RF_CREDIT_UNIT * 2u);
    if( credit > 0x3Fu ) credit = 0x3Fu;
    return (uint8_t)(RF_CREDIT_VALID | (credit << 1));
}

static uint8_t Host_AckCredit( void )
{
    uint32_t free_len = 0;

    if( pRfBuf )
    {
        uint32_t irq_state = __risc_v_disable_irq();
        free_len = pRfBuf->buf_len - pRfBuf->data_len;
        __risc_v_enable_irq( irq_state );
    }
    free_len /= RF_CREDIT_UNIT;
    if( free_len > 0x7Fu ) free_len = 0x7Fu;
    return (uint8_t)(RF_CREDIT_VALID | free_len);
}

/*******************************************************************************
 * @fn      rf_rand
 *
 * @brief   随机数生成函数
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
 * @brief   随机数生成函数
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
 * @brief   断开连接
 *
 * @param   None.
 *
 * @return  None.
 */
static void rf_disconnect( void )
{
    /* Drop bytes and cached acknowledgements from the old RF session.
     * Otherwise a reconnect can replay stale UART data or wait forever for
     * a sequence number belonging to the previous session. */
    simple_buf_reset( pRfBuf );
    gTxBuf.status = 0;
    gTxBuf.resendCount = 0;
    gDataSeq = 0;
    RF_bound_Flag = 0;
    gLastDataAckValid = 0;
    gLastStatusRspValid = 0;
    PRINT("disconnect.\n" );
    RFRole_Shut( );
    gBoundStatus = BOUND_STATUS_IDLE;
    gRxDataStatus = DATA_STATUS_START;
    TMR_ITCfg(DISABLE, TMR_IT_CYC_END); // 关闭中断

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
 * @brief   断开连接
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
    if( !rf_packet_valid( pPkt ) )
    {
        /* Never use a malformed RF length in pointer arithmetic or buffer
         * accounting.  Resume the receive state and let the peer retry. */
        gRfStatus = RF_STATUS_WAIT;
        return;
    }
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

                // 非第一次连接，地址匹配可连
                if( pReq_t->severData )
                {
                    // 信息匹配，或者dongle重新上电了
                    if( !gServerData || pReq_t->severData == gServerData )
                    {
                        reg = 1;
                    }
                }
                else
                {
                    // 第一次连接，需靠近连接
                    if( rssi > -35 )
                    {
                        reg = 1;
                    }
                }
                if( reg )
                {
                    bound_rsp_t *pRsp_t = (bound_rsp_t *)&gTxBuf.TxBuf[PKT_HEAD_LEN];

                    gRfStatus = RF_STATUS_TXRSP;
                    // 生成下次回连的随机信息
                    gDataSeq = 0;
                    gLastDataAckValid = 0;
                    gLastStatusRspValid = 0;
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
                /* A new in-order request supersedes the cached ACK for the
                 * previous DATA packet.  Keeping this flag set would let a
                 * delayed packet (or an 8-bit sequence wrap) cause us to
                 * replay an unrelated gTxBuf response. */
                gLastDataAckValid = 0;
                uint8_t s;
                typeBufSize len = DATA_LEN_MAX_TX;
                len = Host_CreditLength( pPkt->resv );
                if( !len )
                {
                    gRfCreditZeroCount++;
                }
                pPkt_t->type = PKT_CMD_RSP_STATUS;
                pPkt_t->resv = Host_StatusCredit();
                if( gBoundStatus < BOUND_STATUS_EST  )
                {
                    // 发送串口波特率
                    pPkt_t->length = PKT_DATA_OFFSET+1+sizeof(Uart0Para);
                    pRsp_t->opcode = OPCODE_BSP;
                    pRsp_t->buad_t.BaudRate = Uart0Para.BaudRate;
                    pRsp_t->buad_t.StopBits = Uart0Para.StopBits;
                    pRsp_t->buad_t.ParityType = Uart0Para.ParityType;
                    pRsp_t->buad_t.DataBits = Uart0Para.DataBits;
                    pRsp_t->buad_t.ioStaus = Uart0Para.ioStaus;
                    pPkt_t->seq = gDataSeq;
                }
                else
                {
                    s = USB_RxQuery( pRsp_t->other.rspData, &len );
                    if( s == 0 )
                    {
                        pPkt_t->length = PKT_DATA_OFFSET+1+len;
                        pRsp_t->opcode = OPCODE_DATA;
                        pPkt_t->seq = gDataSeq;
                        gRfTxCount += len;
                    }
                    else if( s == 0x80 )
                    {
                        // 发送串口波特率
                        pPkt_t->length = PKT_DATA_OFFSET+1+sizeof(Uart0Para);
                        pRsp_t->opcode = OPCODE_BSP;
                        pRsp_t->buad_t.BaudRate = Uart0Para.BaudRate;
                        pRsp_t->buad_t.StopBits = Uart0Para.StopBits;
                        pRsp_t->buad_t.ParityType = Uart0Para.ParityType;
                        pRsp_t->buad_t.DataBits = Uart0Para.DataBits;
                        pRsp_t->buad_t.ioStaus = Uart0Para.ioStaus;
                        pPkt_t->seq = gDataSeq;
                    }
                    else
                    {
                        pPkt_t->length = PKT_DATA_OFFSET;
                        pRsp_t->opcode = OPCODE_ACK;
                        pPkt_t->seq = gDataSeq;
                    }
                }
                /* Carry the PA7 remote-download key event in the status response. */
                pPkt_t->resv |= Host_BootRequestTake() ? RF_CTRL_ENTER_BOOT : 0;
                gLastStatusRspSeq = gDataSeq;
                gLastStatusRspValid = 1;
                gDataSeq++;
            }
            else if( gLastStatusRspValid && pPkt->seq == gLastStatusRspSeq )
            {
                /* Re-send the cached response without consuming USB data a
                 * second time. */
                gRfStatus = RF_STATUS_TX;
                gTimeout = 0;
                rf_tx_start( pPkt_t, 20 );
                return;
            }
            else
            {
                /* Never transmit a stale response when sequence state has
                 * diverged.  Rebinding is safer than acknowledging a packet
                 * whose payload may not have been accepted. */
                gRfStatus = RF_STATUS_WAIT;
                rf_rx_start( gIntervalTimer );
                return;
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
                /* This new DATA response replaces any cached GET_STATUS
                 * response; an old delayed request must never receive the
                 * current DATA ACK from gTxBuf. */
                gLastStatusRspValid = 0;
                typeBufSize payload_len = pPkt->length - PKT_DATA_OFFSET;
                typeBufSize accepted_len = payload_len;
                if( payload_len ) HostLedActivity();
                write_buf( pRfBuf, (pPkt+1), &accepted_len );
                if( accepted_len != payload_len )
                {
                    /* Do not ACK a packet that did not fit. The Device TX
                     * side will time out and retransmit the same sequence. */
                    gRfBackpressureCount++;
                    /* Device retries after its short ACK timeout, not after
                     * the 10 ms connection poll. Keep this receiver armed for
                     * that retry; RF_RxService only replaces RF_STATUS_WAIT. */
                    gRfStatus = RF_STATUS_RX;
                    rf_rx_start( 150 );
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
                    gRfTxCount += len;
                }
                else
                {
                    pPkt_t->length = PKT_DATA_OFFSET;
                    pPkt_t->seq = gDataSeq;
                    pPkt_t->resv = 0;
                }
                /* Advertise free Host RF->USB space to the Device. */
                pPkt_t->resv = Host_AckCredit();
                gDataSeq++;
                gLastDataAckSeq = (uint8_t)(gDataSeq - 1u);
                gLastDataAckValid = 1;
            }
            else if( gLastDataAckValid && pPkt->seq == gLastDataAckSeq )
            {
                /* The peer did not receive the previous ACK.  gTxBuf still
                 * contains that ACK (including any piggybacked USB data);
                 * resend it verbatim and do not append the duplicate bytes. */
                gRfStatus = RF_STATUS_TX;
                gTimeout = 0;
                rf_tx_start( pPkt_t, 20 );
                return;
            }
            else
            {
                /* An old/out-of-window packet must not be acknowledged with
                 * an unrelated response.  Resume receive and wait for the
                 * peer's current packet. */
                gRfStatus = RF_STATUS_WAIT;
                rf_rx_start( gIntervalTimer );
                return;
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
            // 连接确认，启动连接通信超时
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
    uint32_t irq_state = __risc_v_disable_irq();
    /* Queue occupancy is the authority. Update its wakeup flag while the
     * producer is still excluded, not after read_buf() restores IRQs. */
    if( pRfBuf && pRfBuf->data_len )
    {
        if( read_buf( pRfBuf, buf, len ) == 0 )
        {
            if( gRxDataStatus != DATA_STATUS_TIMEOUT )
                gRxDataStatus = DATA_STATUS_START;
        }
    }
    else
    {
        *len = 0;
    }
    __risc_v_enable_irq( irq_state );
    return *len;
}

/* Keep the radio receive state machine running independently of USB mode.
 * CDC mode does not call RF_RxQuery() through the vendor bulk-IN path, but
 * pairing and RF timeouts still need to be serviced. */
__attribute__((noinline))
void RF_RxService( void )
{
    uint32_t irq_state = __risc_v_disable_irq();
    if( gRxDataStatus == DATA_STATUS_TIMEOUT )
    {
        rf_disconnect( );
    }
    else if( gRfStatus == RF_STATUS_WAIT )
    {
        gRfStatus = RF_STATUS_RX;
        rf_rx_start( gIntervalTimer );
    }
    __risc_v_enable_irq( irq_state );
}

/*******************************************************************************
 * @fn      RF_UartTxInit
 *
 * @brief   RF uart发送应用初始化
 *
 * @param   None.
 *
 * @return  None.
 */
void RF_UartRxInit( void )
{
    PRINT("----------------- rf uart rx mode -----------------\n");
    gRxDataStatus = DATA_STATUS_IDLE;
    rf_buffer_create(&pRfBuf);
    gDataSeq = 0;
    gLastDataAckSeq = 0;
    gLastDataAckValid = 0;
    gLastStatusRspSeq = 0;
    gLastStatusRspValid = 0;
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
