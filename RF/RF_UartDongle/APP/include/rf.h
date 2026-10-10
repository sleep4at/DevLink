/********************************** (C) COPYRIGHT *******************************
* File Name          : rf_basic.h
* Author             : WCH
* Version            : V1.0
* Date               : 2022/03/10
* Description        : 
*******************************************************************************/

#ifndef __RF_BASIC_H
#define __RF_BASIC_H

#ifdef __cplusplus
extern "C"
{
#endif
#include <CH572rf.h>
#include "CH57x_common.h"
#include "buf.h"

#define  DEF_FREQUENCY   17              // Í¨ÐÅÆµµã
#define  TEST_PHY_MODE   PHY_MODE_PHY_2M

#if(TEST_PHY_MODE == PHY_MODE_2G4 )

#if(PHY_2G4_MODE == 0 )
#define   AA           0x94826E8E
#define   AA_EX        0
#define   AA_LEN       1
#define   PRE_LEN      1
#define   CRC_INIT     0xFFFF
#define   CRC_POLY     0x8810
#define   CRC_LEN      2
#define   CTL_FILED    0
#define   DPL_EN       0
#define   DATA_ORDER   0
#define   MODE_2G4     PHY_2G4_1M
#define   CRC_XOR_EN   0

#elif(PHY_2G4_MODE == 1 )
#define PKT_DET_CFG4( var )          { (*((PUINT32V)0x4000C120))= var; } // Demodulation parameter
#define   AA           0x94826E8E
#define   AA_EX        0
#define   AA_LEN       1
#define   PRE_LEN      1
#define   CRC_INIT     0xFFFF
#define   CRC_POLY     0x8810
#define   CRC_LEN      2
#define   CTL_FILED    0
#define   DPL_EN       0
#define   DATA_ORDER   0
#define   MODE_2G4     PHY_2G4_1M
#define   CRC_XOR_EN   0

#elif(PHY_2G4_MODE == 2 )
#define   AA           0x94826E8E
#define   AA_EX        0
#define   AA_LEN       1
#define   PRE_LEN      1
#define   CRC_INIT     0xFFFF
#define   CRC_POLY     0x8810
#define   CRC_LEN      2
#define   CTL_FILED    0
#define   DPL_EN       0
#define   DATA_ORDER   1
#define   MODE_2G4     PHY_2G4_1M
#define   CRC_XOR_EN   1

#endif

#else

#define   AA           0x57250425
#define   AA_EX        0
#define   CRC_INIT     0X555555
#define   CRC_POLY     0x80032d

#endif

/* package type */
typedef struct
{
    uint8_t type;                 //!< package type
    uint8_t length;               //!< data length
    uint8_t seq;                  //!< seq
    uint8_t resv;                 //!< reserved
} rfPackage_t;

/* package type */
#define  PKT_HEAD_LEN          sizeof(rfPackage_t)
#define  PKT_DATA_OFFSET      (PKT_HEAD_LEN-2)

#define  DATA_LEN_MAX_TX    (251)  //!< the max length of rf tx. [251]
#define  BUF_LEN_TX         (DATA_LEN_MAX_TX+PKT_HEAD_LEN)

#define  DATA_LEN_MAX_RX     BUF_LEN_TX  //!< the max length of rf rx.

#define  BOUND_EST_COUNT    6

enum data_status
{
    DATA_STATUS_IDLE,
    DATA_STATUS_START,
    DATA_STATUS_RCV,
    DATA_STATUS_TIMEOUT,
};

enum bound_status
{
    BOUND_STATUS_IDLE,
    BOUND_STATUS_WAIT,
    BOUND_STATUS_EST,
};

/* rf status */
enum rf_status
{
    RF_STATUS_IDLE,
    RF_STATUS_REQ,
    RF_STATUS_GETS,
    RF_STATUS_WAIT,
    RF_STATUS_RX,
    RF_STATUS_TX,
    RF_STATUS_WAITRSP,
    RF_STATUS_TXRSP,
    RF_STATUS_TXACK,
    RF_STATUS_RETX,
    RF_STATUS_REWAIT,

};
typedef struct
{
    uint8_t TxBuf[BUF_LEN_TX];
    typeBufSize len;
    uint8_t status;
    uint8_t resendCount;
} rfTxBuf_t;

typedef struct  __attribute__((packed))
{
    uint16_t interval; // ¹ã²¥ºÍÁ¬½ÓÈ·ÈÏÆÚ¼ä£¬·¢ËÍ¼ä¸ô£¬µ¥Î»£ºms
    uint16_t severData; // »ØÁ¬ÅäÖÃÎªÉÏ´ÎÁ¬½ÓµÄÐÅÏ¢£¬Ê×´ÎÁ¬½ÓÐ´0
} bound_req_t;

typedef struct  __attribute__((packed))
{
    uint32_t accessaddr;// Á¬½ÓÍ¨ÐÅ£¬½ÓÈëµØÖ·
    uint8_t channel; // Á¬½ÓÍ¨ÐÅ£¬Í¨ÐÅÆµÂÊ
    uint8_t phy;     // Á¬½ÓÍ¨ÐÅ£¬PHYÀàÐÍ
    uint16_t severData; // ÓÃÓÚÊ¶±ð»ØÁ¬µÄÐÅÏ¢
    uint16_t interval; // Á¬½ÓÍ¨ÐÅ£¬¿ÕÏÐÊ±·¢ËÍ»ñÈ¡×´Ì¬ÃüÁîµÄ¼ä¸ô£¬µ¥Î»£ºms
    uint16_t timeout;  // Á¬½ÓÍ¨ÐÅ£¬¶Ï¿ªÊ±¼ä£¬µ¥Î»£º10ms
} bound_rsp_t;


typedef struct __attribute__((packed))
{
    uint8_t opcode;
    union{

        struct {
            uint32_t  BaudRate;   /* ²¨ÌØÂÊ */
            uint8_t StopBits;   /* Í£Ö¹Î»¼ÆÊý£¬0£º1Í£Ö¹Î»£¬1£º1.5Í£Ö¹Î»£¬2£º2Í£Ö¹Î» */
            uint8_t ParityType;   /* Ð£ÑéÎ»£¬0£ºNone£¬1£ºOdd£¬2£ºEven£¬3£ºMark£¬4£ºSpace */
            uint8_t DataBits;   /* Êý¾ÝÎ»¼ÆÊý£º5£¬6£¬7£¬8£¬16 */
            uint8_t ioStaus;
        } buad_t;

        struct {
            uint8_t rspData[BUF_LEN_TX];
        }other;
    };
} rfRsp_t;

/* pkt type */
#define  PKT_CMD_BOUND_REQ     0x01
#define  PKT_CMD_GET_STATUS    0x02
#define  PKT_DATA_FLAG         0x7E

#define  PKT_CMD_BOUND_RSP     (0X80|PKT_CMD_BOUND_REQ)
#define  PKT_CMD_RSP_STATUS    (0X80|PKT_CMD_GET_STATUS)
#define  PKT_DATA_RSP_ACK      (0X80|PKT_DATA_FLAG)

typedef void (*pfnRfRxCB_t)( rfPackage_t *pPkt );
typedef void (*pfnRfTxCB_t)( void );
typedef void (*pfnRfRxCrcCB_t)( void  );
typedef void (*pfnRfTimeoutCB_t)( void );

typedef struct
{
    pfnRfRxCB_t pfnRxCB;
    pfnRfTxCB_t pfnTxCB;
    pfnRfRxCrcCB_t pfnCrcErrCB;
    pfnRfTimeoutCB_t pfnTimeoutCB;
} rfStatusCBs_t;

typedef struct
{
    uint16_t head;
    uint16_t serverData;
} rfBoundInfo_t;

#define  BOUND_INFO_HEAD         0X55AA


/* respond opcode define */
#define  OPCODE_DATA           0x00
#define  OPCODE_BSP            0x01
#define  OPCODE_ACK            0xF0

/* Host status response request: Device shall generate the BOOT/RESET pulse. */
#define  RF_CTRL_ENTER_BOOT    0x01u
#define  RF_CREDIT_UNIT        16u

void rf_tx_set_frequency( uint32_t f );
void rf_tx_set_sync_word( uint32_t sync_word );
void rf_tx_start( void *pBuf, uint16_t rfon_us );
void rf_tx_set_phy_type( uint8_t phy );

void rf_rx_set_sync_word( uint32_t sync_word );
void rf_rx_set_frequency( uint32_t f );
void rf_rx_start( uint32_t rx_us );
void rf_rx_set_phy_type( uint8_t phy );

void RFRole_Init(void);
void RFRole_RegisterStatusCbs( rfStatusCBs_t * p );

#ifdef __cplusplus
}
#endif

#endif
