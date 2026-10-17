/********************************** (C) COPYRIGHT *******************************
 * File Name          : buf.c
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2022/06/30
 * Description        : 
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

#include "buf.h"
#include "CH57x_common.h"


/*********************************************************************
 * @fn      simple_buf_create
 *
 * @brief   Create a simple_buf structure object.
 *
 * @param   buf    -   defined simple_buf structural object.
 *          buf_pool  - a pointer to the buffer where data is stored.
 *          pool_size - the size of the buffer.
 *
 * @return  none
 */
struct simple_buf *simple_buf_create(struct simple_buf *buf,
            uint8_t *buf_pool, uint16_t pool_size)
{
    buf->start = buf_pool;
    buf->end = buf->start+pool_size;
    buf->buf_len = pool_size;
    buf->data_len = 0;
    buf->read = buf->start;
    buf->write = buf->start;
    return buf;
}

__HIGH_CODE
void simple_buf_reset(struct simple_buf *buf)
{
    uint32_t irq_state;

    if( !buf ) return;
    irq_state = __risc_v_disable_irq();
    buf->read = buf->start;
    buf->write = buf->start;
    buf->data_len = 0;
    __risc_v_enable_irq( irq_state );
}

/*********************************************************************
 * @fn      write_buf
 *
 * @brief   Add data to the buf object.
 *
 * @param   buf  -  defined simple_buf structural object.
 *          src  -  write data.
 *          len  -  length of the data to be added.
 *
 * @return  typeBufSize
 */
__HIGH_CODE
typeBufSize write_buf(struct simple_buf *buf, void *src, typeBufSize *len )
{
    uint32_t free_len,data_len;
    uint32_t irq_state;
    typeBufSize tmp_len;

    /* Serialize reservation, copy and commit.  pUsbBuf can be written by
     * both the USB ISR and the foreground retry path; splitting this critical
     * section corrupts data_len/write and eventually freezes the link. */
    irq_state = __risc_v_disable_irq();
    free_len = buf->buf_len - buf->data_len;
    if( free_len < *len )
    {
        /* Full-buffer backpressure is handled by the caller.  This function
         * can run from RF/USB interrupt context, so never block here on the
         * debug UART. */
        *len = 0;
        __risc_v_enable_irq( irq_state );
        return buf->data_len;
    }
    tmp_len = (buf->end - buf->write);
    if( *len <= tmp_len )
    {
        __MCPY( buf->write, src, (uint8_t *)((uint8_t *)src+*len) );
        buf->write += *len;
    }
    else
    {
        __MCPY( buf->write, src, (uint8_t *)((uint8_t *)src+tmp_len) );
        buf->write = buf->start;
        src = (uint8_t *)((uint8_t *)src+tmp_len);
        tmp_len = *len - tmp_len;
        __MCPY( buf->write, src, (uint8_t *)((uint8_t *)src+tmp_len) );
        buf->write += tmp_len;
    }
    data_len = buf->data_len + *len;
    buf->data_len = data_len;
    __risc_v_enable_irq( irq_state );
    return  data_len;
}

/*********************************************************************
 * @fn      simple_buf_pull
 *
 * @brief   pull data from the buf object.
 *
 * @param   buf  -  defined simple_buf structural object.
 *          len  -  The length of data to be retrieved.
 *
 * @return  typeBufSize
 */
__HIGH_CODE
typeBufSize read_buf( struct simple_buf *buf, void *dst, typeBufSize *len )
{
    uint32_t writeAddr;
    uint32_t data_len;
    uint32_t irq_state;

    /* See write_buf(): preserve the caller's interrupt state and make the
     * copy plus data_len decrement one indivisible operation. */
    irq_state = __risc_v_disable_irq();
    writeAddr = (uint32_t)buf->write;
    data_len = buf->data_len;

    if( !data_len )
    {
        *len = 0;
        __risc_v_enable_irq( irq_state );
        return 0;
    }
    if( data_len < *len )
    {
        *len = data_len;
    }

    if( writeAddr > (uint32_t)buf->read  )
    {
        __MCPY( dst, buf->read, buf->read+*len );
        buf->read += *len;
    }
    else
    {
        typeBufSize  tmp_len;

        tmp_len = (buf->end-buf->read);
        if( *len <= tmp_len )
        {
            __MCPY( dst, buf->read, buf->read+*len );
            buf->read += *len;
        }
        else
        {
            __MCPY( dst, buf->read, buf->end );
            dst  = (uint8_t *)dst + tmp_len;
            tmp_len = *len - tmp_len;
            buf->read = buf->start;
            __MCPY( dst, buf->read, buf->read + tmp_len );
            buf->read += tmp_len;
        }
    }
    data_len = buf->data_len - *len;
    buf->data_len = data_len;
    __risc_v_enable_irq( irq_state );
    return data_len;
}
