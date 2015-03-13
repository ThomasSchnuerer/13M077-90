/*********************  P r o g r a m  -  M o d u l e ***********************/
/*!  
 *        \file  serial_m77.c
 *
 *      \author  norbert wiedmann.
 *        $Date: 2007/08/14 10:36:31 $
 *    $Revision: 1.2 $
 * 
 *       \brief  Serial Linux driver for M77 modules 
 * 
 * Features
 * - All standard baudrates up to 115200 Baud
 * - up to 8 M77 modules
 * - XON/XOFFhandshake
 *
 *
 *     Switches: MAC_BYTESWAP, _LITTLE_ENDIAN_, _BIG_ENDIAN_
 */
/*-------------------------------[ History ]---------------------------------
 *
 * $Log: serial_m77_24.c,v $
 * Revision 1.2  2007/08/14 10:36:31  ts
 * Initial Revision, renamed from serial_m77.c so mak file uses
 * appropriate sourcefile for the target kernel (2.4 vs. 2.6)
 *
 * Revision 1.1  2003/06/11 10:06:07  kp
 * Initial Revision
 *
 *---------------------------------------------------------------------------
 * (c) Copyright 2003 by MEN mikro elektronik GmbH, Nuernberg, Germany 
 ****************************************************************************/
#define EXPORT_SYMTAB

#include <linux/config.h>
#include <linux/version.h>

#define M77_MAJOR    224     /* CYCLADES_MAJOR  19 */
#define M77AUX_MAJOR 225     /* CYCLADESAUX_MAJOR  20 */

#define OUR_BH MACSERIAL_BH  /* 13 */

#undef SERIAL_PARANOIA_CHECK

#define RS_ISR_PASS_LIMIT 256

/* Set of debugging defines */
#undef SERIAL_DEBUG_MCOUNT  /* module count */


/* Set of debugging defines */
#undef SERIAL_DEBUG_INTR
#undef SERIAL_DEBUG_OPEN
#undef SERIAL_DEBUG_FLOW
#undef SERIAL_DEBUG_RS_WAIT_UNTIL_SENT
#undef SERIAL_DEBUG_PCI
#undef SERIAL_DEBUG_AUTOCONF


#include <linux/module.h>

#include <linux/types.h>
#include <linux/serial.h>
#include "serial_m77.h"
#include "serialP_m77.h"
#include <linux/serial_reg.h>

#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/major.h>
#include <linux/string.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <asm/uaccess.h>

#include <linux/delay.h>
#include <linux/pci.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/bitops.h>

/* M77-special */
#ifndef MAC_MEM_MAPPED
# define MAC_MEM_MAPPED
#endif

#ifndef LINUX
# define LINUX 
#endif

#include <MEN/maccess.h>
#include <MEN/mk_nonmdisif.h>
#include <MEN/mdis_com.h>
#include <MEN/men_typs.h>
#include <MEN/oss.h>
#include <MEN/ll_defs.h>

#include "defsfile.h"

static char *serial_version = "0.9";
static char *serial_revdate = "2003-04-02";
static char *serial_name    = "M77-Module serial driver";

#ifdef SERIAL_INLINE
#define _INLINE_ inline
#else
#define _INLINE_
#endif

static DECLARE_TASK_QUEUE(tq_serial);

static struct tty_driver serial_driver, callout_driver;
static int serial_refcount;

/* serial subtype definitions */
#ifndef SERIAL_TYPE_NORMAL
#define SERIAL_TYPE_NORMAL      1
#define SERIAL_TYPE_CALLOUT     2
#endif

/* number of characters left in xmit buffer before we ask for more */
#define WAKEUP_CHARS 256


static void autoconfig(struct serial_state * state);
static void change_speed(struct async_struct *info, struct termios *old);
static void rs_wait_until_sent(struct tty_struct *tty, int timeout);

#define M77_MAX_MODULE   8  /* maximum of M77-Moduls */
#define M77_PORT_COUNT   4  /* number of serial ports on module*/ 
#define NR_PORTS        32  /* maximum of supported port */


/* modul parameter */
int     minor[M77_MAX_MODULE];
char*   brdName[M77_MAX_MODULE];
int     slotNo[M77_MAX_MODULE];
int     mode[M77_MAX_MODULE];
int     echo[M77_MAX_MODULE];

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MEN M77-module-driver for serial ports RS232/422/423/485");
MODULE_PARM(minor, "1-" __MODULE_STRING(M77_MAX_MODULE) "i");
MODULE_PARM_DESC(minor, "minornummer of first channel on Modul");
MODULE_PARM(brdName,  "1-" __MODULE_STRING(M77_MAX_MODULE) "s");
MODULE_PARM_DESC(brdName, "device name of carrier board, e. g. 'D201_1'");
MODULE_PARM(slotNo,  "1-" __MODULE_STRING(M77_MAX_MODULE) "i");
MODULE_PARM_DESC(slotNo, "slot number on carrier board [0..n]");
MODULE_PARM(mode,  "1-" __MODULE_STRING(M77_MAX_MODULE) "i");
MODULE_PARM_DESC(mode, "interface mode of channel [0..7]");
MODULE_PARM(echo,  "1-" __MODULE_STRING(M77_MAX_MODULE) "i");
MODULE_PARM_DESC(echo, "disable/enable receive line of channel  [0,1]");

MODULE_AUTHOR("MEN mikro elektronic GmbH");

static struct serial_state rs_table[NR_PORTS];
static struct tty_struct *serial_table[NR_PORTS];
static struct termios *serial_termios[NR_PORTS];
static struct termios *serial_termios_locked[NR_PORTS];

#if defined(MODULE) && defined(SERIAL_DEBUG_MCOUNT)
#define DBG_CNT(s) printk("(%s): [%x] refc=%d, serc=%d, ttyc=%d -> %s\n", \
 kdevname(tty->device), (info->flags), serial_refcount,info->count,tty->count,s)
#else
#define DBG_CNT(s)
#endif

/*
 * tmp_buf is used as a temporary buffer by serial_write.  We need to
 * lock it in case the copy_from_user blocks while swapping in a page,
 * and some other program tries to do a serial write at the same time.
 * Since the lock will only come under contention when the system is
 * swapping and available memory is low, it makes sense to share one
 * buffer across all the serial ports, since it significantly saves
 * memory if large numbers of serial ports are open.
 */
static unsigned char *tmp_buf;
#ifdef DECLARE_MUTEX
static DECLARE_MUTEX(tmp_buf_sem);
#else
static struct semaphore tmp_buf_sem = MUTEX;
#endif

#define DEBUG_INITMODULE  0x0001
#define DEBUG_TRM_REC     0x0002
#define DEBUG_RW          0x0004
#define DEBUG_FLOWCTRL    0x0008
#define DEBUG_IRQ         0x0010
#define DEBUG_WRITE       0x0020

static inline int serial_paranoia_check(struct async_struct *info,
                                        kdev_t device, const char *routine)
{
#ifdef SERIAL_PARANOIA_CHECK
        static const char *badmagic =
                "Warning: bad magic number for serial struct (%s) in %s\n";
        static const char *badinfo =
                "Warning: null async_struct for (%s) in %s\n";

        if (!info) {
                printk(badinfo, kdevname(device), routine);
                return 1;
        }
        if (info->magic != SERIAL_MAGIC) {
                printk(badmagic, kdevname(device), routine);
                return 1;
        }
#endif
        return 0;
}

static _INLINE_ unsigned int serial_in(struct async_struct *info, int offset)
{
    return readb((unsigned long) info->iomem_base +
                 (offset<<info->iomem_reg_shift));
}

static _INLINE_ void serial_out(struct async_struct *info, int offset,
                                int value)
{
    writeb(value, (unsigned long) info->iomem_base +
           (offset<<info->iomem_reg_shift));
}

/*
 * We used to support using pause I/O for certain machines.  We
 * haven't supported this for a while, but just in case it's badly
 * needed for certain old 386 machines, I've left these #define's
 * in....
 */
#define serial_inp(info, offset)                serial_in(info, offset)
#define serial_outp(info, offset, value)        serial_out(info, offset, value)


/*
 * For the 16C950
 */
void serial_icr_write(struct async_struct *info, int offset, int  value)
{
        serial_out(info, UART_SCR, offset);
        serial_out(info, UART_ICR, value);
}

unsigned int serial_icr_read(struct async_struct *info, int offset)
{
        int     value;

        serial_icr_write(info, UART_ACR, info->ACR | UART_ACR_ICRRD);
        serial_out(info, UART_SCR, offset);
        value = serial_in(info, UART_ICR);
        serial_icr_write(info, UART_ACR, info->ACR);
        return value;
}

/*
 * ------------------------------------------------------------
 * rs_stop() and rs_start()
 *
 * This routines are called before setting or resetting tty->stopped.
 * They enable or disable transmitter interrupts, as necessary.
 * ------------------------------------------------------------
 */
static void rs_stop(struct tty_struct *tty)
{
        struct async_struct *info = (struct async_struct *)tty->driver_data;
        unsigned long flags;

        if (serial_paranoia_check(info, tty->device, "rs_stop"))
                return;
        
        save_flags(flags); cli();
        if (info->IER & UART_IER_THRI) {
                info->IER &= ~UART_IER_THRI;
                serial_out(info, UART_IER, info->IER);
        }
        if (info->state->type == PORT_16C950) {
                info->ACR |= UART_ACR_TXDIS;
                serial_icr_write(info, UART_ACR, info->ACR);
        }
        restore_flags(flags);
}

static void rs_start(struct tty_struct *tty)
{
        struct async_struct *info = (struct async_struct *)tty->driver_data;
        unsigned long flags;

        if (serial_paranoia_check(info, tty->device, "rs_start"))
                return;
        
        save_flags(flags); cli();
        if (info->xmit.head != info->xmit.tail
            && info->xmit.buf
            && !(info->IER & UART_IER_THRI)) {
                info->IER |= UART_IER_THRI;
                serial_out(info, UART_IER, info->IER);
        }
        if (info->state->type == PORT_16C950) {
                info->ACR &= ~UART_ACR_TXDIS;
                serial_icr_write(info, UART_ACR, info->ACR);
        }
        restore_flags(flags);
}

/*
 * ----------------------------------------------------------------------
 *
 * Here starts the interrupt handling routines.  All of the following
 * subroutines are declared as inline and are folded into
 * rs_interrupt().  They were separated out for readability's sake.
 *
 * Note: rs_interrupt() is a "fast" interrupt, which means that it
 * runs with interrupts turned off.  People who may want to modify
 * rs_interrupt() should try to keep the interrupt handler as fast as
 * possible.  After you are done making modifications, it is not a bad
 * idea to do:
 * 
 * gcc -S -DKERNEL -Wall -Wstrict-prototypes -O6 -fomit-frame-pointer serial.c
 *
 * and look at the resulting assemble code in serial.s.
 *
 *                              - Ted Ts'o (tytso@mit.edu), 7-Mar-93
 * -----------------------------------------------------------------------
 */

/*
 * This routine is used by the interrupt handler to schedule
 * processing in the software interrupt portion of the driver.
 */
static _INLINE_ void rs_sched_event(struct async_struct *info,
                                  int event)
{
        info->event |= 1 << event;
        queue_task(&info->tqueue, &tq_serial);
        mark_bh(OUR_BH);//SERIAL_BH);
}

static _INLINE_ void receive_chars(struct async_struct *info, int *status)
{
        struct tty_struct *tty = info->tty;
        unsigned char ch;
        struct  async_icount *icount;
        int     max_count = 256;

        icount = &info->state->icount;
        do {
                if (tty->flip.count >= TTY_FLIPBUF_SIZE) {
                        tty->flip.tqueue.routine((void *) tty);
                        if (tty->flip.count >= TTY_FLIPBUF_SIZE)
                                return;         // if TTY_DONT_FLIP is set
                }

                ch = serial_inp(info, UART_RX);
                *tty->flip.char_buf_ptr = ch;
                icount->rx++;
                
#ifdef SERIAL_DEBUG_INTR
                printk("rec%02x: %02x...", ch, *status);
#endif
                *tty->flip.flag_buf_ptr = 0;
                if (*status & (UART_LSR_BI | UART_LSR_PE |
                               UART_LSR_FE | UART_LSR_OE)) {
                        /*
                         * For statistics only
                         */
                        if (*status & UART_LSR_BI) {
                            *status &= ~(UART_LSR_FE | UART_LSR_PE);
                            icount->brk++;
                            /*
                             * We do the SysRQ and SAK checking
                             * here because otherwise the break
                             * may get masked by ignore_status_mask
                             * or read_status_mask.
                             */
                            if (info->flags & ASYNC_SAK)
                                do_SAK(tty);
                        } 
                        else if (*status & UART_LSR_PE)
                                icount->parity++;
                        else if (*status & UART_LSR_FE)
                                icount->frame++;
                        if (*status & UART_LSR_OE)
                                icount->overrun++;

                        /*
                         * Mask off conditions which should be ignored.
                         */
                        *status &= info->read_status_mask;

                        if (*status & (UART_LSR_BI)) {
#ifdef SERIAL_DEBUG_INTR
                                printk("handling break....");
#endif
                                *tty->flip.flag_buf_ptr = TTY_BREAK;
                        } else if (*status & UART_LSR_PE)
                                *tty->flip.flag_buf_ptr = TTY_PARITY;
                        else if (*status & UART_LSR_FE)
                                *tty->flip.flag_buf_ptr = TTY_FRAME;
                }
                if ((*status & info->ignore_status_mask) == 0) {
                        tty->flip.flag_buf_ptr++;
                        tty->flip.char_buf_ptr++;
                        tty->flip.count++;
                }
                if ((*status & UART_LSR_OE) &&
                    (tty->flip.count < TTY_FLIPBUF_SIZE)) {
                        /*
                         * Overrun is special, since it's reported
                         * immediately, and doesn't affect the current
                         * character
                         */
                        *tty->flip.flag_buf_ptr = TTY_OVERRUN;
                        tty->flip.count++;
                        tty->flip.flag_buf_ptr++;
                        tty->flip.char_buf_ptr++;
                }
                *status = serial_inp(info, UART_LSR);
        } while ((*status & UART_LSR_DR) && (max_count-- > 0));
        tty_flip_buffer_push(tty);
}

static _INLINE_ void transmit_chars(struct async_struct *info, int *intr_done)
{
        int count;
        if (info->x_char) {
                serial_outp(info, UART_TX, info->x_char);
                info->state->icount.tx++;
                info->x_char = 0;
                if (intr_done)
                        *intr_done = 0;
                return;
        }
        if (info->xmit.head == info->xmit.tail
            || info->tty->stopped
            || info->tty->hw_stopped) {
                info->IER &= ~UART_IER_THRI;
                serial_out(info, UART_IER, info->IER);
                return;
        }
        
        count = info->xmit_fifo_size;
        do {
            serial_out(info, UART_TX, info->xmit.buf[info->xmit.tail]);
            info->xmit.tail = (info->xmit.tail + 1) & (SERIAL_XMIT_SIZE-1);
            info->state->icount.tx++;
            if (info->xmit.head == info->xmit.tail)
                break;
        } while (--count > 0);
        
        if (CIRC_CNT(info->xmit.head,
                     info->xmit.tail,
                     SERIAL_XMIT_SIZE) < WAKEUP_CHARS)
                rs_sched_event(info, RS_EVENT_WRITE_WAKEUP);

#ifdef SERIAL_DEBUG_INTR
        printk("THRE...\n");
#endif
        if (intr_done)
                *intr_done = 0;

        if (info->xmit.head == info->xmit.tail) {
                info->IER &= ~UART_IER_THRI;
                serial_out(info, UART_IER, info->IER);
        }
}

static _INLINE_ void check_modem_status(struct async_struct *info)
{
        int     status;
        struct  async_icount *icount;
        
        status = serial_in(info, UART_MSR);

        if (status & UART_MSR_ANY_DELTA) {
                icount = &info->state->icount;
                /* update input line counters */
                if (status & UART_MSR_TERI)
                        icount->rng++;
                if (status & UART_MSR_DDSR)
                        icount->dsr++;
                if (status & UART_MSR_DDCD) {
                        icount->dcd++;
                }
                if (status & UART_MSR_DCTS)
                        icount->cts++;
                wake_up_interruptible(&info->delta_msr_wait);
        }

        if ((info->flags & ASYNC_CHECK_CD) && (status & UART_MSR_DDCD)) {
#if (defined(SERIAL_DEBUG_OPEN) || defined(SERIAL_DEBUG_INTR))
                printk("ttys%d CD now %s...", info->line,
                       (status & UART_MSR_DCD) ? "on" : "off");
#endif          
                if (status & UART_MSR_DCD)
                        wake_up_interruptible(&info->open_wait);
                else if (!((info->flags & ASYNC_CALLOUT_ACTIVE) &&
                           (info->flags & ASYNC_CALLOUT_NOHUP))) {
#ifdef SERIAL_DEBUG_OPEN
                        printk("doing serial hangup...");
#endif
                        if (info->tty)
                                tty_hangup(info->tty);
                }
        }
        if (info->flags & ASYNC_CTS_FLOW) {
                if (info->tty->hw_stopped) {
                        if (status & UART_MSR_CTS) {
#if (defined(SERIAL_DEBUG_INTR) || defined(SERIAL_DEBUG_FLOW))
                                printk("CTS tx start...");
#endif
                                info->tty->hw_stopped = 0;
                                info->IER |= UART_IER_THRI;
                                serial_out(info, UART_IER, info->IER);
                                rs_sched_event(info, RS_EVENT_WRITE_WAKEUP);
                                return;
                        }
                } else {
                        if (!(status & UART_MSR_CTS)) {
#if (defined(SERIAL_DEBUG_INTR) || defined(SERIAL_DEBUG_FLOW))
                                printk("CTS tx stop...");
#endif
                                info->tty->hw_stopped = 1;
                                info->IER &= ~UART_IER_THRI;
                                serial_out(info, UART_IER, info->IER);
                        }
                }
        }
}

/*--------------------------------------------------------------------+
| This is the driver's interrupt routine, called from mdis-kernel     |
+-------------------------------------------------------------------- */
static int M77_IrqHandler(void *data)
{
    struct M77dev_struct *m77dev = data;
    struct M77chan_struct *m77ch = 0;
    u_int16 isrReg;
	volatile int dummy=0;
    int status, iir, i;
    struct async_struct * info;
    int pass_counter = 0;
        
#ifdef SERIAL_DEBUG_INTR
    printk("rs_interrupt...\n");
#endif

    for(i=0;i<4;i++){
        /* check the four channels for irq */
        m77ch = m77dev->m77chan[i];
        info = m77ch->info;
        if (info){
            do {
                if(!info->tty || (serial_in(info, UART_IIR) & UART_IIR_NO_INT))
                    /* no irq occured on this channel */
                    goto next;

                status = serial_inp(info, UART_LSR);
#ifdef SERIAL_DEBUG_INTR
                printk("status = %x...\n", status);
#endif
                if (status & UART_LSR_DR)
                        receive_chars(info, &status);
                check_modem_status(info);
                if (status & UART_LSR_THRE)
                        transmit_chars(info, 0);
                if (pass_counter++ > RS_ISR_PASS_LIMIT) {
                    break;
                }
                iir = serial_in(info, UART_IIR);
#ifdef SERIAL_DEBUG_INTR
                printk("IIR = %x...\n", iir);
#endif
            } while ((iir & UART_IIR_NO_INT) == 0);
        next:
			dummy = 0; /* satisfy pedantic compilers */
        }
    }
    isrReg = MREAD_D16(m77dev->G_ma, M77_IRQ_REG);
    isrReg |= M77_IRQ_CLEAR;
    MWRITE_D16(m77dev->G_ma, M77_IRQ_REG, isrReg);      

#ifdef SERIAL_DEBUG_INTR
    printk("end.\n");
#endif

    return LL_IRQ_DEVICE;

}

/*
 * This routine is used to handle the "bottom half" processing for the
 * serial driver, known also the "software interrupt" processing.
 * This processing is done at the kernel interrupt level, after the
 * rs_interrupt() has returned, BUT WITH INTERRUPTS TURNED ON.  This
 * is where time-consuming activities which can not be done in the
 * interrupt driver proper are done; the interrupt driver schedules
 * them using rs_sched_event(), and they get done here.
 */
static void do_serial_bh(void)
{
        run_task_queue(&tq_serial);
}

static void do_softint(void *private_)
{
        struct async_struct     *info = (struct async_struct *) private_;
        struct tty_struct       *tty;
        
        tty = info->tty;
        if (!tty)
                return;

        if (test_and_clear_bit(RS_EVENT_WRITE_WAKEUP, &info->event)) {
                if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
                    tty->ldisc.write_wakeup)
                        (tty->ldisc.write_wakeup)(tty);
                wake_up_interruptible(&tty->write_wait);
#ifdef SERIAL_HAVE_POLL_WAIT
                wake_up_interruptible(&tty->poll_wait);
#endif
        }
}

/*
 * ---------------------------------------------------------------
 * Low level utility subroutines for the serial driver:  routines
 * to initialize and startup a serial port, and routines to shutdown a
 * serial port.  Useful stuff like that.
 * ---------------------------------------------------------------
 */

static int startup(struct async_struct * info)
{
    unsigned long flags;
    int retval=0;
    struct serial_state *state= info->state;
    unsigned long page;

    page = get_zeroed_page(GFP_KERNEL);
    if (!page)
        return -ENOMEM;

    save_flags(flags); cli();

    if (info->flags & ASYNC_INITIALIZED) {
        free_page(page);
        goto errout;
    }

    if (info->xmit.buf)
        free_page(page);
    else
        info->xmit.buf = (unsigned char *) page;

    state->m77chan->info = info;
        
#ifdef SERIAL_DEBUG_OPEN
    printk("starting up ttys%d (irq %d)...", info->line, state->irq);
#endif

    if (state->type == PORT_16C950) {
        /* Wake up and initialize UART */
        info->ACR = 0;
        serial_outp(info, UART_LCR, 0xBF);
        serial_outp(info, UART_EFR, UART_EFR_ECB);
        serial_outp(info, UART_IER, 0);
        serial_outp(info, UART_LCR, 0);
        serial_icr_write(info, UART_CSR, 0); /* Reset the UART */
        serial_outp(info, UART_LCR, 0xBF);
        serial_outp(info, UART_EFR, UART_EFR_ECB);
        serial_outp(info, UART_LCR, 0);
    }

    /*
     * Clear the FIFO buffers and disable them
     * (they will be reenabled in change_speed())
     */
    
    serial_outp(info, UART_FCR, UART_FCR_ENABLE_FIFO);
    serial_outp(info, UART_FCR, (UART_FCR_ENABLE_FIFO |
                                 UART_FCR_CLEAR_RCVR |
                                 UART_FCR_CLEAR_XMIT));
    serial_outp(info, UART_FCR, 0);

    /*
     * Clear the interrupt registers.
     */
    (void) serial_inp(info, UART_LSR);
    (void) serial_inp(info, UART_RX);
    (void) serial_inp(info, UART_IIR);
    (void) serial_inp(info, UART_MSR);
    
    /*
     * At this point there's no way the LSR could still be 0xFF;
     * if it is, then bail out, because there's likely no UART
     * here.
     */
    if (!(info->flags & ASYNC_BUGGY_UART) &&
        (serial_inp(info, UART_LSR) == 0xff)) {
        printk("ttyS%d: LSR safety check engaged!\n", state->line);
        if (capable(CAP_SYS_ADMIN)) {
            if (info->tty)
                set_bit(TTY_IO_ERROR, &info->tty->flags);
        } else
            retval = -ENODEV;
        goto errout;
    }
        
    /*----------------------------------+
    |  install irq-handler if necessary |
    +----------------------------------*/
    if (state->m77chan->m77dev->irq_cnt == 0){
        retval = mdis_install_external_irq(state->m77chan->m77dev->G_dev, 
                                           M77_IrqHandler, 
                                           (void *)state->m77chan->m77dev); 
        if (retval < 0) {
            printk("<1> install irq error %d\n", retval);
            if (capable(CAP_SYS_ADMIN)) {
                if (info->tty)
                    set_bit(TTY_IO_ERROR,
                            &info->tty->flags);
                retval = 0;
            }
            goto errout;
        }
    }
    state->m77chan->m77dev->irq_cnt++;

    /*
     * Now, initialize the UART
     */
    serial_outp(info, UART_LCR, UART_LCR_WLEN8);        /* reset DLAB */

    info->MCR = 0;
    if (info->tty->termios->c_cflag & CBAUD)
        info->MCR = UART_MCR_DTR | UART_MCR_RTS;
    {
        if (state->irq != 0)
            info->MCR |= UART_MCR_OUT2;
    }
    info->MCR |= ALPHA_KLUDGE_MCR;              /* Don't ask */
    serial_outp(info, UART_MCR, info->MCR);

    /*
     * Finally, enable interrupts
     */
    info->IER = UART_IER_MSI | UART_IER_RLSI | UART_IER_RDI;
    serial_outp(info, UART_IER, info->IER);     /* enable interrupts */

    /*
     * And clear the interrupt registers again for luck.
     */
    (void)serial_inp(info, UART_LSR);
    (void)serial_inp(info, UART_RX);
    (void)serial_inp(info, UART_IIR);
    (void)serial_inp(info, UART_MSR);

    if (info->tty)
        clear_bit(TTY_IO_ERROR, &info->tty->flags);
    info->xmit.head = info->xmit.tail = 0;

    /*
     * Set up the tty->alt_speed kludge
     */
    if (info->tty) {
        if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_HI)
            info->tty->alt_speed = 57600;
        if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_VHI)
            info->tty->alt_speed = 115200;
        if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_SHI)
            info->tty->alt_speed = 230400;
        if ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_WARP)
            info->tty->alt_speed = 460800;
    }

    /*
     * and set the speed of the serial port
     */
    change_speed(info, 0);
    
    /* enable line driver control in HD-mode */
    info->ACR |= M77_ACR_HD_DRIVER_CTL;
    serial_icr_write(info, UART_ACR, info->ACR);


    info->flags |= ASYNC_INITIALIZED;
    restore_flags(flags);

    return 0;

errout:
    restore_flags(flags);
    
    return retval;
}

/*
 * This routine will shutdown a serial port; interrupts are disabled, and
 * DTR is dropped if the hangup on close termio flag is on.
 */
static void shutdown(struct async_struct * info)
{
        unsigned long   flags;
        struct serial_state *state;

        if (!(info->flags & ASYNC_INITIALIZED))
                return;

        state = info->state;

#ifdef SERIAL_DEBUG_OPEN
        printk("Shutting down serial port %d (irq %d)....", info->line,
               state->irq);
#endif
        
        save_flags(flags); cli(); /* Disable interrupts */

        /*
         * clear delta_msr_wait queue to avoid mem leaks: we may free the irq
         * here so the queue might never be waken up
         */
        wake_up_interruptible(&info->delta_msr_wait);
        

        if (info->xmit.buf) {
                unsigned long pg = (unsigned long) info->xmit.buf;
                info->xmit.buf = 0;
                free_page(pg);
        }

        info->IER = 0;
        serial_outp(info, UART_IER, 0x00);      /* disable all intrs */
        info->MCR &= ~UART_MCR_OUT2;
        info->MCR |= ALPHA_KLUDGE_MCR;          /* Don't ask */
        
        /* disable break condition */
        serial_out(info, UART_LCR, serial_inp(info, UART_LCR) & ~UART_LCR_SBC);
        
        if (!info->tty || (info->tty->termios->c_cflag & HUPCL))
                info->MCR &= ~(UART_MCR_DTR|UART_MCR_RTS);
        serial_outp(info, UART_MCR, info->MCR);

        /* disable FIFO's */    
        serial_outp(info, UART_FCR, (UART_FCR_ENABLE_FIFO |
                                     UART_FCR_CLEAR_RCVR |
                                     UART_FCR_CLEAR_XMIT));
        serial_outp(info, UART_FCR, 0); 

        (void)serial_in(info, UART_RX);    /* read data port to reset things */

        /*
         * Free the IRQ
         */
        if (state->m77chan->m77dev->irq_cnt == 1){
            mdis_remove_external_irq(state->m77chan->m77dev->G_dev  );
        }
        state->m77chan->m77dev->irq_cnt--;

        if (info->tty)
                set_bit(TTY_IO_ERROR, &info->tty->flags);

        info->flags &= ~ASYNC_INITIALIZED;
        restore_flags(flags);
}


/*
 * This routine is called to set the UART divisor registers to match
 * the specified baud rate for a serial port.
 */
static void change_speed(struct async_struct *info,
                         struct termios *old_termios)
{
        int     quot = 0, baud_base, baud;
        unsigned cflag, cval, fcr = 0;
        int     bits;
        unsigned long   flags;

        if (!info->tty || !info->tty->termios)
                return;
        cflag = info->tty->termios->c_cflag;

        /* byte size and parity */
        switch (cflag & CSIZE) {
              case CS5: cval = 0x00; bits = 7; break;
              case CS6: cval = 0x01; bits = 8; break;
              case CS7: cval = 0x02; bits = 9; break;
              case CS8: cval = 0x03; bits = 10; break;
              /* Never happens, but GCC is too dumb to figure it out */
              default:  cval = 0x00; bits = 7; break;
              }
        if (cflag & CSTOPB) {
                cval |= 0x04;
                bits++;
        }
        if (cflag & PARENB) {
                cval |= UART_LCR_PARITY;
                bits++;
        }
        if (!(cflag & PARODD))
                cval |= UART_LCR_EPAR;

        /* Determine divisor based on baud rate */
        baud = tty_get_baud_rate(info->tty);
        if (!baud)
                baud = 9600;    /* B0 transition handled in rs_set_termios */
        baud_base = info->state->baud_base;
        if (info->state->type == PORT_16C950) {
            if (baud <= baud_base)
                serial_icr_write(info, UART_TCR, 0);
            else if (baud <= 2*baud_base) {
                serial_icr_write(info, UART_TCR, 0x8);
                baud_base = baud_base * 2;
            } else if (baud <= 4*baud_base) {
                serial_icr_write(info, UART_TCR, 0x4);
                baud_base = baud_base * 4;
            } else
                serial_icr_write(info, UART_TCR, 0);
        }
        if (baud == 38400 && ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST))
            quot = info->state->custom_divisor;
        else {
            if (baud == 134)
                /* Special case since 134 is really 134.5 */
                quot = (2*baud_base / 269);
            else if (baud)
                quot = baud_base / baud;
        }
        /* If the quotient is zero refuse the change */
        if (!quot && old_termios) {
                info->tty->termios->c_cflag &= ~CBAUD;
                info->tty->termios->c_cflag |= (old_termios->c_cflag & CBAUD);
                baud = tty_get_baud_rate(info->tty);
                if (!baud)
                        baud = 9600;
                if (baud == 38400 &&
                    ((info->flags & ASYNC_SPD_MASK) == ASYNC_SPD_CUST))
                        quot = info->state->custom_divisor;
                else {
                        if (baud == 134)
                                /* Special case since 134 is really 134.5 */
                                quot = (2*baud_base / 269);
                        else if (baud)
                                quot = baud_base / baud;
                }
        }
        /* As a last resort, if the quotient is zero, default to 9600 bps */
        if (!quot)
                quot = baud_base / 9600;
        /*
         * Work around a bug in the Oxford Semiconductor 952 rev B
         * chip which causes it to seriously miscalculate baud rates
         * when DLL is 0.
         */
        if (((quot & 0xFF) == 0) && (info->state->type == PORT_16C950) &&
            (info->state->revision == 0x5201))
                quot++;
        
        info->quot = quot;
        info->timeout = ((info->xmit_fifo_size*HZ*bits*quot) / baud_base);
        info->timeout += HZ/50;         /* Add .02 seconds of slop */

        /* Set up FIFO's */
        if ((info->state->baud_base / quot) < 2400)
            fcr = UART_FCR_ENABLE_FIFO | UART_FCR_TRIGGER_1;
        else
            fcr = UART_FCR_ENABLE_FIFO | UART_FCR_TRIGGER_8;

        
        /* CTS flow control flag and modem status interrupts */
        info->IER &= ~UART_IER_MSI;
        if (info->flags & ASYNC_HARDPPS_CD)
                info->IER |= UART_IER_MSI;
        if (cflag & CRTSCTS) {
                info->flags |= ASYNC_CTS_FLOW;
                info->IER |= UART_IER_MSI;
        } else
                info->flags &= ~ASYNC_CTS_FLOW;
        if (cflag & CLOCAL)
                info->flags &= ~ASYNC_CHECK_CD;
        else {
                info->flags |= ASYNC_CHECK_CD;
                info->IER |= UART_IER_MSI;
        }
        serial_out(info, UART_IER, info->IER);

        /*
         * Set up parity check flag
         */
#define RELEVANT_IFLAG(iflag) (iflag & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK))

        info->read_status_mask = UART_LSR_OE | UART_LSR_THRE | UART_LSR_DR;
        if (I_INPCK(info->tty))
                info->read_status_mask |= UART_LSR_FE | UART_LSR_PE;
        if (I_BRKINT(info->tty) || I_PARMRK(info->tty))
                info->read_status_mask |= UART_LSR_BI;
        
        /*
         * Characters to ignore
         */
        info->ignore_status_mask = 0;
        if (I_IGNPAR(info->tty))
                info->ignore_status_mask |= UART_LSR_PE | UART_LSR_FE;
        if (I_IGNBRK(info->tty)) {
                info->ignore_status_mask |= UART_LSR_BI;
                /*
                 * If we're ignore parity and break indicators, ignore 
                 * overruns too.  (For real raw support).
                 */
                if (I_IGNPAR(info->tty))
                        info->ignore_status_mask |= UART_LSR_OE;
        }
        /*
         * !!! ignore all characters if CREAD is not set
         */
        if ((cflag & CREAD) == 0)
                info->ignore_status_mask |= UART_LSR_DR;
        save_flags(flags); cli();

        serial_outp(info, UART_LCR, cval | UART_LCR_DLAB);      /* set DLAB */
        serial_outp(info, UART_DLL, quot & 0xff);       /* LS of divisor */
        serial_outp(info, UART_DLM, quot >> 8);         /* MS of divisor */
        serial_outp(info, UART_LCR, cval);
                /* reset DLAB */
        info->LCR = cval;                               /* Save LCR */

        if (fcr & UART_FCR_ENABLE_FIFO) {
            /* emulated UARTs (Lucent Venus 167x) need two steps */
            serial_outp(info, UART_FCR, UART_FCR_ENABLE_FIFO);
        }
        serial_outp(info, UART_FCR, fcr);       /* set fcr */


        restore_flags(flags);
}

static void rs_put_char(struct tty_struct *tty, unsigned char ch)
{
        struct async_struct *info = (struct async_struct *)tty->driver_data;
        unsigned long flags;

        if (serial_paranoia_check(info, tty->device, "rs_put_char"))
                return;

        if (!tty || !info->xmit.buf)
                return;

        save_flags(flags); cli();
        if (CIRC_SPACE(info->xmit.head,
                       info->xmit.tail,
                       SERIAL_XMIT_SIZE) == 0) {
                restore_flags(flags);
                return;
        }

        info->xmit.buf[info->xmit.head] = ch;
        info->xmit.head = (info->xmit.head + 1) & (SERIAL_XMIT_SIZE-1);
        restore_flags(flags);
}

static void rs_flush_chars(struct tty_struct *tty)
{
        struct async_struct *info = (struct async_struct *)tty->driver_data;
        unsigned long flags;
                                
        if (serial_paranoia_check(info, tty->device, "rs_flush_chars"))
                return;

        if (info->xmit.head == info->xmit.tail
            || tty->stopped
            || tty->hw_stopped
            || !info->xmit.buf)
                return;

        save_flags(flags); cli();
        info->IER |= UART_IER_THRI;
        serial_out(info, UART_IER, info->IER);
        restore_flags(flags);
}

static int rs_write(struct tty_struct * tty, int from_user,
                    const unsigned char *buf, int count)
{
        int     c, ret = 0;
        struct async_struct *info = (struct async_struct *)tty->driver_data;
        unsigned long flags;

        //printk("<1>" __FUNCTION__ ": line %d: %s\n", info->line, buf);
                                
        if (serial_paranoia_check(info, tty->device, "rs_write"))
                return 0;

        if (!tty || !info->xmit.buf || !tmp_buf)
                return 0;

        save_flags(flags);
        if (from_user) {
                down(&tmp_buf_sem);
                while (1) {
                        int c1;
                        c = CIRC_SPACE_TO_END(info->xmit.head,
                                              info->xmit.tail,
                                              SERIAL_XMIT_SIZE);
                        if (count < c)
                                c = count;
                        if (c <= 0)
                                break;

                        c -= copy_from_user(tmp_buf, buf, c);
                        if (!c) {
                                if (!ret)
                                        ret = -EFAULT;
                                break;
                        }
                        cli();
                        c1 = CIRC_SPACE_TO_END(info->xmit.head,
                                               info->xmit.tail,
                                               SERIAL_XMIT_SIZE);
                        if (c1 < c)
                                c = c1;
                        memcpy(info->xmit.buf + info->xmit.head, tmp_buf, c);
                        info->xmit.head = ((info->xmit.head + c) &
                                           (SERIAL_XMIT_SIZE-1));
                        restore_flags(flags);
                        buf += c;
                        count -= c;
                        ret += c;
                }
                up(&tmp_buf_sem);
        } else {
                cli();
                while (1) {
                        c = CIRC_SPACE_TO_END(info->xmit.head,
                                              info->xmit.tail,
                                              SERIAL_XMIT_SIZE);
                        if (count < c)
                                c = count;
                        if (c <= 0) {
                                break;
                        }
                        memcpy(info->xmit.buf + info->xmit.head, buf, c);
                        info->xmit.head = ((info->xmit.head + c) &
                                           (SERIAL_XMIT_SIZE-1));
                        buf += c;
                        count -= c;
                        ret += c;
                }
                restore_flags(flags);
        }
        if (info->xmit.head != info->xmit.tail
            && !tty->stopped
            && !tty->hw_stopped
            && !(info->IER & UART_IER_THRI)) {
                info->IER |= UART_IER_THRI;
                serial_out(info, UART_IER, info->IER);
        }
        return ret;
}

static int rs_write_room(struct tty_struct *tty)
{
        struct async_struct *info = (struct async_struct *)tty->driver_data;

        if (serial_paranoia_check(info, tty->device, "rs_write_room"))
                return 0;
        return CIRC_SPACE(info->xmit.head, info->xmit.tail, SERIAL_XMIT_SIZE);
}

static int rs_chars_in_buffer(struct tty_struct *tty)
{
    struct async_struct *info = (struct async_struct *)tty->driver_data;
        
    if (serial_paranoia_check(info, tty->device, "rs_chars_in_buffer"))
        return 0;

    return CIRC_CNT(info->xmit.head, info->xmit.tail, SERIAL_XMIT_SIZE);
}

static void rs_flush_buffer(struct tty_struct *tty)
{
        struct async_struct *info = (struct async_struct *)tty->driver_data;
        unsigned long flags;
        
        if (serial_paranoia_check(info, tty->device, "rs_flush_buffer"))
                return;
        save_flags(flags); cli();
        info->xmit.head = info->xmit.tail = 0;
        restore_flags(flags);
        wake_up_interruptible(&tty->write_wait);
#ifdef SERIAL_HAVE_POLL_WAIT
        wake_up_interruptible(&tty->poll_wait);
#endif
        if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) &&
            tty->ldisc.write_wakeup)
                (tty->ldisc.write_wakeup)(tty);
}

/*
 * This function is used to send a high-priority XON/XOFF character to
 * the device
 */
static void rs_send_xchar(struct tty_struct *tty, char ch)
{
        struct async_struct *info = (struct async_struct *)tty->driver_data;

        if (serial_paranoia_check(info, tty->device, "rs_send_char"))
                return;

        info->x_char = ch;
        if (ch) {
                /* Make sure transmit interrupts are on */
                info->IER |= UART_IER_THRI;
                serial_out(info, UART_IER, info->IER);
        }
}

/*
 * ------------------------------------------------------------
 * rs_throttle()
 * 
 * This routine is called by the upper-layer tty layer to signal that
 * incoming characters should be throttled.
 * ------------------------------------------------------------
 */
static void rs_throttle(struct tty_struct * tty)
{
        struct async_struct *info = (struct async_struct *)tty->driver_data;
        unsigned long flags;
#ifdef SERIAL_DEBUG_THROTTLE
        char    buf[64];
        
        printk("throttle %s: %d....\n", tty_name(tty, buf),
               tty->ldisc.chars_in_buffer(tty));
#endif

        if (serial_paranoia_check(info, tty->device, "rs_throttle"))
                return;
        
        if (I_IXOFF(tty))
                rs_send_xchar(tty, STOP_CHAR(tty));

        if (tty->termios->c_cflag & CRTSCTS)
                info->MCR &= ~UART_MCR_RTS;

        save_flags(flags); cli();
        serial_out(info, UART_MCR, info->MCR);
        restore_flags(flags);
}

static void rs_unthrottle(struct tty_struct * tty)
{
        struct async_struct *info = (struct async_struct *)tty->driver_data;
        unsigned long flags;
#ifdef SERIAL_DEBUG_THROTTLE
        char    buf[64];
        
        printk("unthrottle %s: %d....\n", tty_name(tty, buf),
               tty->ldisc.chars_in_buffer(tty));
#endif

        if (serial_paranoia_check(info, tty->device, "rs_unthrottle"))
                return;
        
        if (I_IXOFF(tty)) {
                if (info->x_char)
                        info->x_char = 0;
                else
                        rs_send_xchar(tty, START_CHAR(tty));
        }
        if (tty->termios->c_cflag & CRTSCTS)
                info->MCR |= UART_MCR_RTS;
        save_flags(flags); cli();
        serial_out(info, UART_MCR, info->MCR);
        restore_flags(flags);
}

/*
 * ------------------------------------------------------------
 * rs_ioctl() and friends
 * ------------------------------------------------------------
 */

static int get_serial_info(struct async_struct * info,
                           struct serial_struct * retinfo)
{
        struct serial_struct tmp;
        struct serial_state *state = info->state;
   
        if (!retinfo)
                return -EFAULT;
        memset(&tmp, 0, sizeof(tmp));
        tmp.type = state->type;
        tmp.line = state->line;
        tmp.irq = state->irq;
        tmp.flags = state->flags;
        tmp.xmit_fifo_size = state->xmit_fifo_size;
        tmp.baud_base = state->baud_base;
        tmp.close_delay = state->close_delay;
        tmp.closing_wait = state->closing_wait;
        tmp.custom_divisor = state->custom_divisor;
        if (copy_to_user(retinfo,&tmp,sizeof(*retinfo)))
                return -EFAULT;
        return 0;
}

static int set_serial_info(struct async_struct * info,
                           struct serial_struct * new_info)
{
        struct serial_struct new_serial;
        struct serial_state old_state, *state;
        int                     retval = 0;

        if (copy_from_user(&new_serial,new_info,sizeof(new_serial)))
                return -EFAULT;
        state = info->state;
        old_state = *state;

        if ((new_serial.irq >= NR_IRQS) || (new_serial.irq < 0) || 
            (new_serial.baud_base < 9600)|| (new_serial.type < PORT_UNKNOWN) ||
            (new_serial.type > PORT_MAX) || (new_serial.type == PORT_CIRRUS) ||
            (new_serial.type == PORT_STARTECH)) {
                return -EINVAL;
        }

        /*
         * OK, past this point, all the error checking has been done.
         * At this point, we start making changes.....
         */

        state->baud_base = new_serial.baud_base;
        state->flags = ((state->flags & ~ASYNC_FLAGS) |
                        (new_serial.flags & ASYNC_FLAGS));
        info->flags = ((state->flags & ~ASYNC_INTERNAL_FLAGS) |
                       (info->flags & ASYNC_INTERNAL_FLAGS));
        state->custom_divisor = new_serial.custom_divisor;
        state->close_delay = new_serial.close_delay * HZ/100;
        state->closing_wait = new_serial.closing_wait * HZ/100;

        info->tty->low_latency = (info->flags & ASYNC_LOW_LATENCY) ? 1 : 0;


        if (info->flags & ASYNC_INITIALIZED) {
            /* the ASYNC_SPD_MASK-paramter are not used */
        } 
        else
            retval = startup(info);
        return retval;
}


/*
 * get_lsr_info - get line status register info
 *
 * Purpose: Let user call ioctl() to get info when the UART physically
 *          is emptied.  On bus types like RS485, the transmitter must
 *          release the bus after transmitting. This must be done when
 *          the transmit shift register is empty, not be done when the
 *          transmit holding register is empty.  This functionality
 *          allows an RS485 driver to be written in user space. 
 */
static int get_lsr_info(struct async_struct * info, unsigned int *value)
{
        unsigned char status;
        unsigned int result;
        unsigned long flags;

        save_flags(flags); cli();
        status = serial_in(info, UART_LSR);
        restore_flags(flags);
        result = ((status & UART_LSR_TEMT) ? TIOCSER_TEMT : 0);

        /*
         * If we're about to load something into the transmit
         * register, we'll pretend the transmitter isn't empty to
         * avoid a race condition (depending on when the transmit
         * interrupt happens).
         */
        if (info->x_char || 
            ((CIRC_CNT(info->xmit.head, info->xmit.tail,
                       SERIAL_XMIT_SIZE) > 0) &&
             !info->tty->stopped && !info->tty->hw_stopped))
                result &= ~TIOCSER_TEMT;

        if (copy_to_user(value, &result, sizeof(int)))
                return -EFAULT;
        return 0;
}


static int get_modem_info(struct async_struct * info, unsigned int *value)
{
        unsigned char control, status;
        unsigned int result;
        unsigned long flags;

        control = info->MCR;
        save_flags(flags); cli();
        status = serial_in(info, UART_MSR);
        restore_flags(flags);
        result =  ((control & UART_MCR_RTS) ? TIOCM_RTS : 0)
                | ((control & UART_MCR_DTR) ? TIOCM_DTR : 0)
#ifdef TIOCM_OUT1
                | ((control & UART_MCR_OUT1) ? TIOCM_OUT1 : 0)
                | ((control & UART_MCR_OUT2) ? TIOCM_OUT2 : 0)
#endif
                | ((status  & UART_MSR_DCD) ? TIOCM_CAR : 0)
                | ((status  & UART_MSR_RI) ? TIOCM_RNG : 0)
                | ((status  & UART_MSR_DSR) ? TIOCM_DSR : 0)
                | ((status  & UART_MSR_CTS) ? TIOCM_CTS : 0);

        if (copy_to_user(value, &result, sizeof(int)))
                return -EFAULT;
        return 0;
}

static int set_modem_info(struct async_struct * info, unsigned int cmd,
                          unsigned int *value)
{
        unsigned int arg;
        unsigned long flags;

        if (copy_from_user(&arg, value, sizeof(int)))
                return -EFAULT;

        switch (cmd) {
        case TIOCMBIS: 
                if (arg & TIOCM_RTS)
                        info->MCR |= UART_MCR_RTS;
                if (arg & TIOCM_DTR)
                        info->MCR |= UART_MCR_DTR;
#ifdef TIOCM_OUT1
                if (arg & TIOCM_OUT1)
                        info->MCR |= UART_MCR_OUT1;
                if (arg & TIOCM_OUT2)
                        info->MCR |= UART_MCR_OUT2;
#endif
                if (arg & TIOCM_LOOP)
                        info->MCR |= UART_MCR_LOOP;
                break;
        case TIOCMBIC:
                if (arg & TIOCM_RTS)
                        info->MCR &= ~UART_MCR_RTS;
                if (arg & TIOCM_DTR)
                        info->MCR &= ~UART_MCR_DTR;
#ifdef TIOCM_OUT1
                if (arg & TIOCM_OUT1)
                        info->MCR &= ~UART_MCR_OUT1;
                if (arg & TIOCM_OUT2)
                        info->MCR &= ~UART_MCR_OUT2;
#endif
                if (arg & TIOCM_LOOP)
                        info->MCR &= ~UART_MCR_LOOP;
                break;
        case TIOCMSET:
                info->MCR = ((info->MCR & ~(UART_MCR_RTS |
#ifdef TIOCM_OUT1
                                            UART_MCR_OUT1 |
                                            UART_MCR_OUT2 |
#endif
                                            UART_MCR_LOOP |
                                            UART_MCR_DTR))
                             | ((arg & TIOCM_RTS) ? UART_MCR_RTS : 0)
#ifdef TIOCM_OUT1
                             | ((arg & TIOCM_OUT1) ? UART_MCR_OUT1 : 0)
                             | ((arg & TIOCM_OUT2) ? UART_MCR_OUT2 : 0)
#endif
                             | ((arg & TIOCM_LOOP) ? UART_MCR_LOOP : 0)
                             | ((arg & TIOCM_DTR) ? UART_MCR_DTR : 0));
                break;
        default:
                return -EINVAL;
        }
        save_flags(flags); cli();
        info->MCR |= ALPHA_KLUDGE_MCR;          /* Don't ask */
        serial_out(info, UART_MCR, info->MCR);
        restore_flags(flags);
        return 0;
}


/*
 * rs_break() --- routine which turns the break handling on or off
 */
static void rs_break(struct tty_struct *tty, int break_state)
{
        struct async_struct * info = (struct async_struct *)tty->driver_data;
        unsigned long flags;

        if (serial_paranoia_check(info, tty->device, "rs_break"))
            return;

        save_flags(flags); cli();
        if (break_state == -1)
            info->LCR |= UART_LCR_SBC;
        else
            info->LCR &= ~UART_LCR_SBC;
        serial_out(info, UART_LCR, info->LCR);
        restore_flags(flags);
}


static int rs_ioctl(struct tty_struct *tty, struct file * file,
                    unsigned int cmd, unsigned long arg)
{
    struct async_struct * info = (struct async_struct *)tty->driver_data;
    struct async_icount cprev, cnow;    /* kernel counter temps */
    struct serial_icounter_struct icount;
    unsigned long flags;

    if (serial_paranoia_check(info, tty->device, "rs_ioctl"))
        return -ENODEV;

    if ((cmd != TIOCGSERIAL) && (cmd != TIOCSSERIAL) &&
        (cmd != TIOCSERCONFIG) && (cmd != TIOCSERGSTRUCT) &&
        (cmd != TIOCMIWAIT) && (cmd != TIOCGICOUNT)) {
        if (tty->flags & (1 << TTY_IO_ERROR))
            return -EIO;
    }

    //printk("<1> ioctl %d\n", cmd);
    switch (cmd) {
        case TIOCMGET:
            return get_modem_info(info, (unsigned int *) arg);
        case TIOCMBIS:
        case TIOCMBIC:
        case TIOCMSET:
            return set_modem_info(info, cmd, (unsigned int *) arg);
        case TIOCGSERIAL:
            return get_serial_info(info, (struct serial_struct *) arg);
        case TIOCSSERIAL:
            return set_serial_info(info, (struct serial_struct *) arg);
        /*case TIOCSERCONFIG:*/

        case TIOCSERGETLSR: /* Get line status register */
            return get_lsr_info(info, (unsigned int *) arg);
                
        case TIOCSERGSTRUCT:
            if (copy_to_user((struct async_struct *) arg,
                             info, sizeof(struct async_struct)))
                return -EFAULT;
            return 0;
            /*
             * Wait for any of the 4 modem inputs (DCD,RI,DSR,CTS) to change
             * - mask passed in arg for lines of interest
             *   (use |'ed TIOCM_RNG/DSR/CD/CTS for masking)
             * Caller should use TIOCGICOUNT to see which one it was
             */
        case TIOCMIWAIT:
            save_flags(flags); cli();
            /* note the counters on entry */
            cprev = info->state->icount;
            restore_flags(flags);
            /* Force modem status interrupts on */
            info->IER |= UART_IER_MSI;
            serial_out(info, UART_IER, info->IER);
            while (1) {
                interruptible_sleep_on(&info->delta_msr_wait);
                /* see if a signal did it */
                if (signal_pending(current))
                    return -ERESTARTSYS;
                save_flags(flags); cli();
                cnow = info->state->icount; /* atomic copy */
                restore_flags(flags);
                if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr && 
                    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts)
                    return -EIO; /* no change => error */
                if ( ((arg & TIOCM_RNG) && (cnow.rng != cprev.rng)) ||
                     ((arg & TIOCM_DSR) && (cnow.dsr != cprev.dsr)) ||
                     ((arg & TIOCM_CD)  && (cnow.dcd != cprev.dcd)) ||
                     ((arg & TIOCM_CTS) && (cnow.cts != cprev.cts)) ) {
                    return 0;
                }
                cprev = cnow;
            }
            /* NOTREACHED */

            /* 
             * Get counter of input serial line interrupts (DCD,RI,DSR,CTS)
             * Return: write counters to the user passed counter struct
             * NB: both 1->0 and 0->1 transitions are counted except for
             *     RI where only 0->1 is counted.
             */
        case TIOCGICOUNT:
            save_flags(flags); cli();
            cnow = info->state->icount;
            restore_flags(flags);
            icount.cts = cnow.cts;
            icount.dsr = cnow.dsr;
            icount.rng = cnow.rng;
            icount.dcd = cnow.dcd;
            icount.rx = cnow.rx;
            icount.tx = cnow.tx;
            icount.frame = cnow.frame;
            icount.overrun = cnow.overrun;
            icount.parity = cnow.parity;
            icount.brk = cnow.brk;
            icount.buf_overrun = cnow.buf_overrun;
                
            if (copy_to_user((void *)arg, &icount, sizeof(icount)))
                return -EFAULT;
            return 0;
        case TIOCSERGWILD:
        case TIOCSERSWILD:
            /* "setserial -W" is called in Debian boot */
            printk ("TIOCSER?WILD ioctl obsolete, ignored.\n");
            return 0;

        case M77_PHYS_INT_SET:
            return M77PhysIntSet(info->state->m77chan, arg);
            break;

        case M77_ECHO_SUPPRESS:
            if(arg){
                info->state->m77chan->echo_supress = 1;
                return M77PhysIntSet(info->state->m77chan, 
                              info->state->m77chan->physInt);
            }
            else{
                info->state->m77chan->echo_supress = 0;
                return M77PhysIntSet(info->state->m77chan, 
                              info->state->m77chan->physInt);
            }
            break;
            
        default:
            return -ENOIOCTLCMD;
    }
    return 0;
}

static void rs_set_termios(struct tty_struct *tty, struct termios *old_termios)
{
        struct async_struct *info = (struct async_struct *)tty->driver_data;
        unsigned long flags;
        unsigned int cflag = tty->termios->c_cflag;

        if (   (cflag == old_termios->c_cflag)
            && (   RELEVANT_IFLAG(tty->termios->c_iflag) 
                == RELEVANT_IFLAG(old_termios->c_iflag)))
          return;

        change_speed(info, old_termios);

        /* Handle transition to B0 status */
        if ((old_termios->c_cflag & CBAUD) &&
            !(cflag & CBAUD)) {
                info->MCR &= ~(UART_MCR_DTR|UART_MCR_RTS);
                save_flags(flags); cli();
                serial_out(info, UART_MCR, info->MCR);
                restore_flags(flags);
        }
        
        /* Handle transition away from B0 status */
        if (!(old_termios->c_cflag & CBAUD) &&
            (cflag & CBAUD)) {
                info->MCR |= UART_MCR_DTR;
                if (!(tty->termios->c_cflag & CRTSCTS) || 
                    !test_bit(TTY_THROTTLED, &tty->flags)) {
                        info->MCR |= UART_MCR_RTS;
                }
                save_flags(flags); cli();
                serial_out(info, UART_MCR, info->MCR);
                restore_flags(flags);
        }
        
        /* Handle turning off CRTSCTS */
        if ((old_termios->c_cflag & CRTSCTS) &&
            !(tty->termios->c_cflag & CRTSCTS)) {
                tty->hw_stopped = 0;
                rs_start(tty);
        }

#if 0
        /*
         * No need to wake up processes in open wait, since they
         * sample the CLOCAL flag once, and don't recheck it.
         * XXX  It's not clear whether the current behavior is correct
         * or not.  Hence, this may change.....
         */
        if (!(old_termios->c_cflag & CLOCAL) &&
            (tty->termios->c_cflag & CLOCAL))
                wake_up_interruptible(&info->open_wait);
#endif
}

/*
 * ------------------------------------------------------------
 * rs_close()
 * 
 * This routine is called when the serial port gets closed.  First, we
 * wait for the last remaining data to be sent.  Then, we unlink its
 * async structure from the interrupt chain if necessary, and we free
 * that IRQ if nothing is left in the chain.
 * ------------------------------------------------------------
 */
static void rs_close(struct tty_struct *tty, struct file * filp)
{
        struct async_struct * info = (struct async_struct *)tty->driver_data;
        struct serial_state *state;
        unsigned long flags;

        if (!info || serial_paranoia_check(info, tty->device, "rs_close"))
                return;

        state = info->state;
        
        save_flags(flags); cli();
        
        if (tty_hung_up_p(filp)) {
                DBG_CNT("before DEC-hung");
                MOD_DEC_USE_COUNT;
                restore_flags(flags);
                return;
        }
        
#ifdef SERIAL_DEBUG_OPEN
        printk("rs_close ttys%d, count = %d\n", info->line, state->count);
#endif
        if ((tty->count == 1) && (state->count != 1)) {
                /*
                 * Uh, oh.  tty->count is 1, which means that the tty
                 * structure will be freed.  state->count should always
                 * be one in these conditions.  If it's greater than
                 * one, we've got real problems, since it means the
                 * serial port won't be shutdown.
                 */
                printk("rs_close: bad serial port count; tty->count is 1, "
                       "state->count is %d\n", state->count);
                state->count = 1;
        }
        if (--state->count < 0) {
                printk("rs_close: bad serial port count for ttys%d: %d\n",
                       info->line, state->count);
                state->count = 0;
        }
        if (state->count) {
                DBG_CNT("before DEC-2");
                MOD_DEC_USE_COUNT;
                restore_flags(flags);
                return;
        }
        info->flags |= ASYNC_CLOSING;
        restore_flags(flags);
        /*
         * Save the termios structure, since this port may have
         * separate termios for callout and dialin.
         */
        if (info->flags & ASYNC_NORMAL_ACTIVE)
                info->state->normal_termios = *tty->termios;
        if (info->flags & ASYNC_CALLOUT_ACTIVE)
                info->state->callout_termios = *tty->termios;
        /*
         * Now we wait for the transmit buffer to clear; and we notify 
         * the line discipline to only process XON/XOFF characters.
         */
        tty->closing = 1;
        if (info->closing_wait != ASYNC_CLOSING_WAIT_NONE)
                tty_wait_until_sent(tty, info->closing_wait);
        /*
         * At this point we stop accepting input.  To do this, we
         * disable the receive line status interrupts, and tell the
         * interrupt driver to stop checking the data ready bit in the
         * line status register.
         */
        info->IER &= ~UART_IER_RLSI;
        info->read_status_mask &= ~UART_LSR_DR;
        if (info->flags & ASYNC_INITIALIZED) {
                serial_out(info, UART_IER, info->IER);
                /*
                 * Before we drop DTR, make sure the UART transmitter
                 * has completely drained; this is especially
                 * important if there is a transmit FIFO!
                 */
                rs_wait_until_sent(tty, info->timeout);
        }
        shutdown(info);
        if (tty->driver.flush_buffer)
                tty->driver.flush_buffer(tty);
        if (tty->ldisc.flush_buffer)
                tty->ldisc.flush_buffer(tty);
        tty->closing = 0;
        info->event = 0;
        info->tty = 0;
        if (info->blocked_open) {
                if (info->close_delay) {
                        set_current_state(TASK_INTERRUPTIBLE);
                        schedule_timeout(info->close_delay);
                }
                wake_up_interruptible(&info->open_wait);
        }
        info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE|
                         ASYNC_CLOSING);
        wake_up_interruptible(&info->close_wait);
        MOD_DEC_USE_COUNT;
}

/*
 * rs_wait_until_sent() --- wait until the transmitter is empty
 */
static void rs_wait_until_sent(struct tty_struct *tty, int timeout)
{
        struct async_struct * info = (struct async_struct *)tty->driver_data;
        unsigned long orig_jiffies, char_time;
        int lsr;
        
        if (serial_paranoia_check(info, tty->device, "rs_wait_until_sent"))
                return;

        if (info->state->type == PORT_UNKNOWN)
                return;

        if (info->xmit_fifo_size == 0)
                return; /* Just in case.... */

        orig_jiffies = jiffies;
        /*
         * Set the check interval to be 1/5 of the estimated time to
         * send a single character, and make it at least 1.  The check
         * interval should also be less than the timeout.
         * 
         * Note: we have to use pretty tight timings here to satisfy
         * the NIST-PCTS.
         */
        char_time = (info->timeout - HZ/50) / info->xmit_fifo_size;
        char_time = char_time / 5;
        if (char_time == 0)
                char_time = 1;
        if (timeout && timeout < char_time)
                char_time = timeout;
        /*
         * If the transmitter hasn't cleared in twice the approximate
         * amount of time to send the entire FIFO, it probably won't
         * ever clear.  This assumes the UART isn't doing flow
         * control, which is currently the case.  Hence, if it ever
         * takes longer than info->timeout, this is probably due to a
         * UART bug of some kind.  So, we clamp the timeout parameter at
         * 2*info->timeout.
         */
        if (!timeout || timeout > 2*info->timeout)
                timeout = 2*info->timeout;
#ifdef SERIAL_DEBUG_RS_WAIT_UNTIL_SENT
        printk("In rs_wait_until_sent(%d) check=%lu...", timeout, char_time);
        printk("jiff=%lu...", jiffies);
#endif
        while (!((lsr = serial_inp(info, UART_LSR)) & UART_LSR_TEMT)) {
#ifdef SERIAL_DEBUG_RS_WAIT_UNTIL_SENT
                printk("lsr = %d (jiff=%lu)...", lsr, jiffies);
#endif
                set_current_state(TASK_INTERRUPTIBLE);
                schedule_timeout(char_time);
                if (signal_pending(current))
                        break;
                if (timeout && time_after(jiffies, orig_jiffies + timeout))
                        break;
        }
#ifdef SERIAL_DEBUG_RS_WAIT_UNTIL_SENT
        printk("lsr = %d (jiff=%lu)...done\n", lsr, jiffies);
#endif
}

/*
 * rs_hangup() --- called by tty_hangup() when a hangup is signaled.
 */
static void rs_hangup(struct tty_struct *tty)
{
        struct async_struct * info = (struct async_struct *)tty->driver_data;
        struct serial_state *state = info->state;

        if (serial_paranoia_check(info, tty->device, "rs_hangup"))
                return;

        state = info->state;
        
        rs_flush_buffer(tty);
        if (info->flags & ASYNC_CLOSING)
                return;
        shutdown(info);
        info->event = 0;
        state->count = 0;
        info->flags &= ~(ASYNC_NORMAL_ACTIVE|ASYNC_CALLOUT_ACTIVE);
        info->tty = 0;
        wake_up_interruptible(&info->open_wait);
}

/*
 * ------------------------------------------------------------
 * rs_open() and friends
 * ------------------------------------------------------------
 */
static int block_til_ready(struct tty_struct *tty, struct file * filp,
                           struct async_struct *info)
{
        DECLARE_WAITQUEUE(wait, current);
        struct serial_state *state = info->state;
        int             retval;
        int             do_clocal = 0, extra_count = 0;
        unsigned long   flags;

        /*
         * If the device is in the middle of being closed, then block
         * until it's done, and then try again.
         */
        if (tty_hung_up_p(filp) ||
            (info->flags & ASYNC_CLOSING)) {
                if (info->flags & ASYNC_CLOSING)
                        interruptible_sleep_on(&info->close_wait);
#ifdef SERIAL_DO_RESTART
                return ((info->flags & ASYNC_HUP_NOTIFY) ?
                        -EAGAIN : -ERESTARTSYS);
#else
                return -EAGAIN;
#endif
        }

        /*
         * If this is a callout device, then just make sure the normal
         * device isn't being used.
         */
        if (tty->driver.subtype == SERIAL_TYPE_CALLOUT) {
                if (info->flags & ASYNC_NORMAL_ACTIVE)
                        return -EBUSY;
                if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
                    (info->flags & ASYNC_SESSION_LOCKOUT) &&
                    (info->session != current->session))
                    return -EBUSY;
                if ((info->flags & ASYNC_CALLOUT_ACTIVE) &&
                    (info->flags & ASYNC_PGRP_LOCKOUT) &&
                    (info->pgrp != current->pgrp))
                    return -EBUSY;
                info->flags |= ASYNC_CALLOUT_ACTIVE;
                return 0;
        }
        
        /*
         * If non-blocking mode is set, or the port is not enabled,
         * then make the check up front and then exit.
         */
        if ((filp->f_flags & O_NONBLOCK) ||
            (tty->flags & (1 << TTY_IO_ERROR))) {
                if (info->flags & ASYNC_CALLOUT_ACTIVE)
                        return -EBUSY;
                info->flags |= ASYNC_NORMAL_ACTIVE;
                return 0;
        }

        if (info->flags & ASYNC_CALLOUT_ACTIVE) {
                if (state->normal_termios.c_cflag & CLOCAL)
                        do_clocal = 1;
        } else {
                if (tty->termios->c_cflag & CLOCAL)
                        do_clocal = 1;
        }
        
        /*
         * Block waiting for the carrier detect and the line to become
         * free (i.e., not in use by the callout).  While we are in
         * this loop, state->count is dropped by one, so that
         * rs_close() knows when to free things.  We restore it upon
         * exit, either normal or abnormal.
         */
        retval = 0;
        add_wait_queue(&info->open_wait, &wait);
#ifdef SERIAL_DEBUG_OPEN
        printk("block_til_ready before block: ttys%d, count = %d\n",
               state->line, state->count);
#endif
        save_flags(flags); cli();
        if (!tty_hung_up_p(filp)) {
                extra_count = 1;
                state->count--;
        }
        restore_flags(flags);
        info->blocked_open++;
        while (1) {
                save_flags(flags); cli();
                if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
                    (tty->termios->c_cflag & CBAUD))
                        serial_out(info, UART_MCR,
                                   serial_inp(info, UART_MCR) |
                                   (UART_MCR_DTR | UART_MCR_RTS));
                restore_flags(flags);
                set_current_state(TASK_INTERRUPTIBLE);
                if (tty_hung_up_p(filp) ||
                    !(info->flags & ASYNC_INITIALIZED)) {
#ifdef SERIAL_DO_RESTART
                        if (info->flags & ASYNC_HUP_NOTIFY)
                                retval = -EAGAIN;
                        else
                                retval = -ERESTARTSYS;  
#else
                        retval = -EAGAIN;
#endif
                        break;
                }
                if (!(info->flags & ASYNC_CALLOUT_ACTIVE) &&
                    !(info->flags & ASYNC_CLOSING) &&
                    (do_clocal || (serial_in(info, UART_MSR) &
                                   UART_MSR_DCD)))
                        break;
                if (signal_pending(current)) {
                        retval = -ERESTARTSYS;
                        break;
                }
#ifdef SERIAL_DEBUG_OPEN
                printk("block_til_ready blocking: ttys%d, count = %d\n",
                       info->line, state->count);
#endif
                schedule();
        }
        set_current_state(TASK_RUNNING);
        remove_wait_queue(&info->open_wait, &wait);
        if (extra_count)
                state->count++;
        info->blocked_open--;
#ifdef SERIAL_DEBUG_OPEN
        printk("block_til_ready after blocking: ttys%d, count = %d\n",
               info->line, state->count);
#endif
        if (retval)
                return retval;
        info->flags |= ASYNC_NORMAL_ACTIVE;
        return 0;
}

static int get_async_struct(int line, struct async_struct **ret_info)
{
        struct async_struct *info;
        struct serial_state *sstate;

        sstate = rs_table + line;
        sstate->count++;
        if (sstate->info) {
                *ret_info = sstate->info;
                return 0;
        }
        info = kmalloc(sizeof(struct async_struct), GFP_KERNEL);
        if (!info) {
                sstate->count--;
                return -ENOMEM;
        }
        memset(info, 0, sizeof(struct async_struct));
        init_waitqueue_head(&info->open_wait);
        init_waitqueue_head(&info->close_wait);
        init_waitqueue_head(&info->delta_msr_wait);
        info->magic = SERIAL_MAGIC;
        info->flags = sstate->flags;
        info->iomem_base = sstate->iomem_base;
        info->iomem_reg_shift = sstate->iomem_reg_shift;
        info->xmit_fifo_size = sstate->xmit_fifo_size;
        info->line = line;
        info->tqueue.routine = do_softint;
        info->tqueue.data = info;
        info->state = sstate;
        if (sstate->info) {
                kfree(info);
                *ret_info = sstate->info;
                return 0;
        }
        *ret_info = sstate->info = info;
        return 0;
}

/*
 * This routine is called whenever a serial port is opened.  It
 * enables interrupts for a serial port, linking in its async structure into
 * the IRQ chain.   It also performs the serial-specific
 * initialization for the tty structure.
 *
 * Note that on failure, we don't decrement the module use count - the tty
 * later will call rs_close, which will decrement it for us as long as
 * tty->driver_data is set non-NULL. --rmk
 */
static int rs_open(struct tty_struct *tty, struct file * filp)
{
        struct async_struct     *info;
        int                     retval, line;
        unsigned long           page;

        MOD_INC_USE_COUNT;
        line = MINOR(tty->device) - tty->driver.minor_start;
        if ((line < 0) || (line >= NR_PORTS)) {
                MOD_DEC_USE_COUNT;
                return -ENODEV;
        }
        retval = get_async_struct(line, &info);
        if (retval) {
                MOD_DEC_USE_COUNT;
                return retval;
        }
        tty->driver_data = info;
        info->tty = tty;
        if (serial_paranoia_check(info, tty->device, "rs_open"))
                return -ENODEV;

#ifdef SERIAL_DEBUG_OPEN
        printk("rs_open %s%d, count = %d\n", tty->driver.name, info->line,
               info->state->count);
#endif
#if (LINUX_VERSION_CODE > 0x20100)
        info->tty->low_latency = (info->flags & ASYNC_LOW_LATENCY) ? 1 : 0;
#endif

        /*
         *      This relies on lock_kernel() stuff so wants tidying for 2.5
         */
        if (!tmp_buf) {
                page = get_zeroed_page(GFP_KERNEL);
                if (!page)
                        return -ENOMEM;
                if (tmp_buf)
                        free_page(page);
                else
                        tmp_buf = (unsigned char *) page;
        }

        /*
         * If the port is the middle of closing, bail out now
         */
        if (tty_hung_up_p(filp) ||
            (info->flags & ASYNC_CLOSING)) {
                if (info->flags & ASYNC_CLOSING)
                        interruptible_sleep_on(&info->close_wait);
#ifdef SERIAL_DO_RESTART
                return ((info->flags & ASYNC_HUP_NOTIFY) ?
                        -EAGAIN : -ERESTARTSYS);
#else
                return -EAGAIN;
#endif
        }

        /*
         * Start up serial port
         */
        retval = startup(info);
        if (retval)
                return retval;

        retval = block_til_ready(tty, filp, info);
        if (retval) {
#ifdef SERIAL_DEBUG_OPEN
                printk("rs_open returning after block_til_ready with %d\n",
                       retval);
#endif
                return retval;
        }

        if ((info->state->count == 1) &&
            (info->flags & ASYNC_SPLIT_TERMIOS)) {
                if (tty->driver.subtype == SERIAL_TYPE_NORMAL)
                        *tty->termios = info->state->normal_termios;
                else 
                        *tty->termios = info->state->callout_termios;
                change_speed(info, 0);
        }
        info->session = current->session;
        info->pgrp = current->pgrp;

#ifdef SERIAL_DEBUG_OPEN
        printk("rs_open ttys%d successful...", info->line);
#endif
        return 0;
}

/*
 * /proc fs routines....
 */

static inline int line_info(char *buf, struct serial_state *state)
{
        struct async_struct *info = state->info, scr_info;
        char    stat_buf[30], control, status;
        int     ret;
        unsigned long flags;

        ret = sprintf(buf, "%d: uart:16C954 irq:%d", state->line, state->irq);

		if (!state->port || (state->type == PORT_UNKNOWN)) {
			ret += sprintf(buf+ret, "\n");
			return ret;
		}


        /*
         * Figure out the current RS-232 lines
         */
        if (!info) {
            info = &scr_info;   /* This is just for serial_{in,out} */

            info->magic = SERIAL_MAGIC;
            info->flags = state->flags;
            info->iomem_base = state->iomem_base;
            info->iomem_reg_shift = state->iomem_reg_shift;
            info->quot = 0;
            info->tty = 0;
        }
        save_flags(flags); cli();
        status = serial_in(info, UART_MSR);
        control = info != &scr_info ? info->MCR : serial_in(info, UART_MCR);
        restore_flags(flags); 

        stat_buf[0] = 0;
        stat_buf[1] = 0;
        if (control & UART_MCR_RTS)
                strcat(stat_buf, "|RTS");
        if (status & UART_MSR_CTS)
                strcat(stat_buf, "|CTS");
        if (control & UART_MCR_DTR)
                strcat(stat_buf, "|DTR");
        if (status & UART_MSR_DSR)
                strcat(stat_buf, "|DSR");
        if (status & UART_MSR_DCD)
                strcat(stat_buf, "|CD");
        if (status & UART_MSR_RI)
                strcat(stat_buf, "|RI");

        if (info->quot) {
                ret += sprintf(buf+ret, " baud:%d",
                               state->baud_base / info->quot);
        }

        ret += sprintf(buf+ret, " tx:%d rx:%d",
                      state->icount.tx, state->icount.rx);

        if (state->icount.frame)
                ret += sprintf(buf+ret, " fe:%d", state->icount.frame);
        
        if (state->icount.parity)
                ret += sprintf(buf+ret, " pe:%d", state->icount.parity);
        
        if (state->icount.brk)
                ret += sprintf(buf+ret, " brk:%d", state->icount.brk);  

        if (state->icount.overrun)
                ret += sprintf(buf+ret, " oe:%d", state->icount.overrun);

        /*
         * Last thing is the RS-232 status lines
         */
        ret += sprintf(buf+ret, " %s\n", stat_buf+1);
        return ret;
}

int rs_read_proc(char *page, char **start, off_t off, int count,
                 int *eof, void *data)
{
        int i, len = 0, l;
        off_t   begin = 0;

        len += sprintf(page, "serinfo:1.0 driver:%s revision:%s\n",
                       serial_version, serial_revdate);
        for (i = 0; i < NR_PORTS && len < 4000; i++) {
                l = line_info(page + len, &rs_table[i]);
                len += l;
                if (len+begin > off+count)
                        goto done;
                if (len+begin < off) {
                        begin += len;
                        len = 0;
                }
        }
        *eof = 1;
done:
        if (off >= len+begin)
                return 0;
        *start = page + (off-begin);
        return ((count < begin+len-off) ? count : begin+len-off);
}

/*
 * ---------------------------------------------------------------------
 * rs_init() and friends
 *
 * rs_init() is called at boot-time to initialize the serial driver.
 * ---------------------------------------------------------------------
 */

/*
 * This routine prints out the appropriate serial driver version
 * number, and identifies which options were configured into this
 * driver.
 */
static char serial_options[] __initdata = " no serial options enabled\n";

static _INLINE_ void show_serial_version(void)
{
        printk(KERN_INFO "%s version %s (%s) with%s", serial_name,
               serial_version, serial_revdate,
               serial_options);
}


/*
 * This is a helper routine to autodetect StarTech/Exar/Oxsemi UART's.
 * When this function is called we know it is at least a StarTech
 * 16650 V2, but it might be one of several StarTech UARTs, or one of
 * its clones.  (We treat the broken original StarTech 16650 V1 as a
 * 16550, and why not?  Startech doesn't seem to even acknowledge its
 * existence.)
 * 
 * What evil have men's minds wrought...
 */
static void autoconfig_startech_uarts(struct async_struct *info,
                                      struct serial_state *state,
                                      unsigned long flags)
{
        unsigned char scratch, scratch2, scratch3, scratch4;

        /*
         * First we check to see if it's an Oxford Semiconductor UART.
         *
         * If we have to do this here because some non-National
         * Semiconductor clone chips lock up if you try writing to the
         * LSR register (which serial_icr_read does)
         */
        if (state->type == PORT_16550A) {
                /*
                 * EFR [4] must be set else this test fails
                 *
                 * This shouldn't be necessary, but Mike Hudson
                 * (Exoray@isys.ca) claims that it's needed for 952
                 * dual UART's (which are not recommended for new designs).
                 */
                info->ACR = 0;
                serial_out(info, UART_LCR, 0xBF);
                serial_out(info, UART_EFR, 0x10);
                serial_out(info, UART_LCR, 0x00);
                /* Check for Oxford Semiconductor 16C950 */
                scratch = serial_icr_read(info, UART_ID1);
                scratch2 = serial_icr_read(info, UART_ID2);
                scratch3 = serial_icr_read(info, UART_ID3);
                
                if (scratch == 0x16 && scratch2 == 0xC9 &&
                    (scratch3 == 0x50 || scratch3 == 0x52 ||
                     scratch3 == 0x54)) {
                        state->type = PORT_16C950;
                        state->revision = serial_icr_read(info, UART_REV) |
                                (scratch3 << 8);
                        return;
                }
        }
        
        /*
         * We check for a XR16C850 by setting DLL and DLM to 0, and
         * then reading back DLL and DLM.  If DLM reads back 0x10,
         * then the UART is a XR16C850 and the DLL contains the chip
         * revision.  If DLM reads back 0x14, then the UART is a
         * XR16C854.
         * 
         */

        /* Save the DLL and DLM */

        serial_outp(info, UART_LCR, UART_LCR_DLAB);
        scratch3 = serial_inp(info, UART_DLL);
        scratch4 = serial_inp(info, UART_DLM);

        serial_outp(info, UART_DLL, 0);
        serial_outp(info, UART_DLM, 0);
        scratch2 = serial_inp(info, UART_DLL);
        scratch = serial_inp(info, UART_DLM);
        serial_outp(info, UART_LCR, 0);

        if (scratch == 0x10 || scratch == 0x14) {
                if (scratch == 0x10)
                        state->revision = scratch2;
                state->type = PORT_16850;
                return;
        }

        /* Restore the DLL and DLM */

        serial_outp(info, UART_LCR, UART_LCR_DLAB);
        serial_outp(info, UART_DLL, scratch3);
        serial_outp(info, UART_DLM, scratch4);
        serial_outp(info, UART_LCR, 0);
}

/*
 * This routine is called by rs_init() to initialize a specific serial
 * port.  It determines what type of UART chip this serial port is
 * using: 8250, 16450, 16550, 16550A.  The important question is
 * whether or not this UART is a 16550A or not, since this will
 * determine whether or not we can use its FIFO features or not.
 */
static void autoconfig(struct serial_state * state)
{
        unsigned char status1, status2, scratch, scratch2, scratch3;
        unsigned char save_lcr, save_mcr;
        struct async_struct *info, scr_info;
        unsigned long flags;

        state->type = PORT_UNKNOWN;

#ifdef SERIAL_DEBUG_AUTOCONF
        printk("Testing ttyS%d (0x%04x)...\n", state->line,
               (unsigned) state->iomem_base);
#endif
        
                
        info = &scr_info;       /* This is just for serial_{in,out} */

        info->magic = SERIAL_MAGIC;
        info->state = state;
        info->flags = state->flags;
        info->iomem_base = state->iomem_base;
        info->iomem_reg_shift = state->iomem_reg_shift;

        save_flags(flags); cli();
        
        if (!(state->flags & ASYNC_BUGGY_UART) &&
            !state->iomem_base) {
                /*
                 * Do a simple existence test first; if we fail this,
                 * there's no point trying anything else.
                 * 
                 * 0x80 is used as a nonsense port to prevent against
                 * false positives due to ISA bus float.  The
                 * assumption is that 0x80 is a non-existent port;
                 * which should be safe since include/asm/io.h also
                 * makes this assumption.
                 */
                scratch = serial_inp(info, UART_IER);
                serial_outp(info, UART_IER, 0);
#ifdef __i386__
                outb(0xff, 0x080);
#endif
                scratch2 = serial_inp(info, UART_IER);
                serial_outp(info, UART_IER, 0x0F);
#ifdef __i386__
                outb(0, 0x080);
#endif
                scratch3 = serial_inp(info, UART_IER);
                serial_outp(info, UART_IER, scratch);
                if (scratch2 || scratch3 != 0x0F) {
#ifdef SERIAL_DEBUG_AUTOCONF
                        printk("serial: ttyS%d: simple autoconfig failed "
                               "(%02x, %02x)\n", state->line, 
                               scratch2, scratch3);
#endif
                        restore_flags(flags);
                        return;         /* We failed; there's nothing here */
                }
        }

        save_mcr = serial_in(info, UART_MCR);
        save_lcr = serial_in(info, UART_LCR);

        serial_outp(info, UART_LCR, 0xBF); /* set up for StarTech test */
        serial_outp(info, UART_EFR, 0); /* EFR is the same as FCR */
        serial_outp(info, UART_LCR, 0);
        serial_outp(info, UART_FCR, UART_FCR_ENABLE_FIFO);
        scratch = serial_in(info, UART_IIR) >> 6;
        switch (scratch) {
                case 0:
                        state->type = PORT_16450;
                        break;
                case 1:
                        state->type = PORT_UNKNOWN;
                        break;
                case 2:
                        state->type = PORT_16550;
                        break;
                case 3:
                        state->type = PORT_16550A;
                        break;
        }

        if (state->type == PORT_16550A) {
            /* Check for Startech UART's */
            serial_outp(info, UART_LCR, UART_LCR_DLAB);
            if (serial_in(info, UART_EFR) == 0) {
                /* the MPC8245 has a 16550D with a EFR/UAFR that is zero */
                /* a fifo size of 16 is *not* a ST16650 (it's 32) */
                int size = M77_SIZE_FIFO;
                if (size != 16) {
                    state->type = PORT_16650;
                }       
            } 
            else{
                serial_outp(info, UART_LCR, 0xBF);
                if (serial_in(info, UART_EFR) == 0)
                    autoconfig_startech_uarts(info, state, flags);
            }
        }

        if (state->type == PORT_16550A) {
                /* Check for TI 16750 */
                serial_outp(info, UART_LCR, save_lcr | UART_LCR_DLAB);
                serial_outp(info, UART_FCR,
                            UART_FCR_ENABLE_FIFO | UART_FCR7_64BYTE);
                scratch = serial_in(info, UART_IIR) >> 5;
                if (scratch == 7) {
                        /*
                         * If this is a 16750, and not a cheap UART
                         * clone, then it should only go into 64 byte
                         * mode if the UART_FCR7_64BYTE bit was set
                         * while UART_LCR_DLAB was latched.
                         */
                        serial_outp(info, UART_FCR, UART_FCR_ENABLE_FIFO);
                        serial_outp(info, UART_LCR, 0);
                        serial_outp(info, UART_FCR,
                                    UART_FCR_ENABLE_FIFO | UART_FCR7_64BYTE);
                        scratch = serial_in(info, UART_IIR) >> 5;
                        if (scratch == 6)
                                state->type = PORT_16750;
                }
                serial_outp(info, UART_FCR, UART_FCR_ENABLE_FIFO);
        }
        serial_outp(info, UART_LCR, save_lcr);
        if (state->type == PORT_16450) {
                scratch = serial_in(info, UART_SCR);
                serial_outp(info, UART_SCR, 0xa5);
                status1 = serial_in(info, UART_SCR);
                serial_outp(info, UART_SCR, 0x5a);
                status2 = serial_in(info, UART_SCR);
                serial_outp(info, UART_SCR, scratch);

                if ((status1 != 0xa5) || (status2 != 0x5a))
                        state->type = PORT_8250;
        }
        state->xmit_fifo_size = M77_SIZE_FIFO;

        if (state->type == PORT_UNKNOWN) {
                restore_flags(flags);
                return;
        }

        /*
         * Reset the UART.
         */
        serial_outp(info, UART_MCR, save_mcr);
        serial_outp(info, UART_FCR, (UART_FCR_ENABLE_FIFO |
                                     UART_FCR_CLEAR_RCVR |
                                     UART_FCR_CLEAR_XMIT));
        serial_outp(info, UART_FCR, 0);
        (void)serial_in(info, UART_RX);
        serial_outp(info, UART_IER, 0);
        
        restore_flags(flags);
}


/*
 * The serial driver boot-time initialization code!
 */
static int __init M77_init(void)
{
    int i;
    struct M77chan_struct  *m77chan = 0;
    struct serial_state    *state;
    int retval;
    
    retval = InitModuleParm();
    if(retval){
        printk("error in InitModuleParm, retval: %i\n", retval);
        return retval;
    }

    init_bh(OUR_BH, do_serial_bh);

    show_serial_version();

    /* Initialize the tty_driver structure */   
    memset(&serial_driver, 0, sizeof(struct tty_driver));
    serial_driver.magic = TTY_DRIVER_MAGIC;
    serial_driver.driver_name = "serial_m77";
#if (LINUX_VERSION_CODE > 0x2032D && defined(CONFIG_DEVFS_FS))
    serial_driver.name = "ttd/%d";
#else
    serial_driver.name = "ttyD";
#endif
    serial_driver.major = M77_MAJOR;
    serial_driver.minor_start = 0;
    serial_driver.name_base = 0;
    serial_driver.num = NR_PORTS;
    serial_driver.type = TTY_DRIVER_TYPE_SERIAL;
    serial_driver.subtype = SERIAL_TYPE_NORMAL;
    serial_driver.init_termios = tty_std_termios;
    serial_driver.init_termios.c_cflag =
        B9600 | CS8 | CREAD | HUPCL | CLOCAL;
    serial_driver.flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_NO_DEVFS;
    serial_driver.refcount = &serial_refcount;
    serial_driver.table = serial_table;
    serial_driver.termios = serial_termios;
    serial_driver.termios_locked = serial_termios_locked;

    serial_driver.open = rs_open;
    serial_driver.close = rs_close;
    serial_driver.write = rs_write;
    serial_driver.put_char = rs_put_char;
    serial_driver.flush_chars = rs_flush_chars;
    serial_driver.write_room = rs_write_room;
    serial_driver.chars_in_buffer = rs_chars_in_buffer;
    serial_driver.flush_buffer = rs_flush_buffer;
    serial_driver.ioctl = rs_ioctl;
    serial_driver.throttle = rs_throttle;
    serial_driver.unthrottle = rs_unthrottle;
    serial_driver.set_termios = rs_set_termios;
    serial_driver.stop = rs_stop;
    serial_driver.start = rs_start;
    serial_driver.hangup = rs_hangup;
    serial_driver.break_ctl = rs_break;
    serial_driver.send_xchar = rs_send_xchar;
    serial_driver.wait_until_sent = rs_wait_until_sent;
    serial_driver.read_proc = rs_read_proc;
    
    /*
     * The callout device is just like normal device except for
     * major number and the subtype code.
     */
    callout_driver = serial_driver;
#if (LINUX_VERSION_CODE > 0x2032D && defined(CONFIG_DEVFS_FS))
    callout_driver.name = "cud/%d";
#else
    callout_driver.name = "cud";
#endif
    callout_driver.major = M77AUX_MAJOR;//TTYAUX_MAJOR;
    callout_driver.subtype = SERIAL_TYPE_CALLOUT;
#if (LINUX_VERSION_CODE >= 131343)
    callout_driver.read_proc = 0;
    callout_driver.proc_entry = 0;
#endif

    if (tty_register_driver(&serial_driver))
        panic("Couldn't register serial driver\n");
    if (tty_register_driver(&callout_driver))
        panic("Couldn't register callout driver\n");
        
    for (i = 0, state = rs_table; i < NR_PORTS; i++,state++) {
        state->magic = SSTATE_MAGIC;
        state->line = i;
        state->type = PORT_UNKNOWN;
        state->custom_divisor = 0;
        state->close_delay = 5*HZ/10;
        state->closing_wait = 30*HZ;
        state->callout_termios = callout_driver.init_termios;
        state->normal_termios = serial_driver.init_termios;
        state->icount.cts = state->icount.dsr = 
            state->icount.rng = state->icount.dcd = 0;
        state->icount.rx = state->icount.tx = 0;
        state->icount.frame = state->icount.parity = 0;
        state->icount.overrun = state->icount.brk = 0;
        
        state->xmit_fifo_size = 128;
        state->baud_base = 1152000;
    }

    for (i = 0, state = rs_table; i < NR_PORTS; i++,state++) {
        m77chan = 0;
        retval = GetM77Chan(i, &m77chan);
        if(!retval){
            state->m77chan     = m77chan;
            state->irq         = m77chan->m77dev->irqVector;
            state->iomem_base  = m77chan->iomem_base;
            state->iomem_reg_shift = 1;
            state->channel_nr  = m77chan->channel_nr;

            /* standard function to detect the 16C954 on M77 module*/
            autoconfig(state);

            if (state->type != PORT_16C950) {
#if (LINUX_VERSION_CODE > 0x2032D && defined(CONFIG_DEVFS_FS))
                printk(KERN_INFO "error with ttd/%02d: 16C954 not found\n", 
                       state->line);
#else
                printk(KERN_INFO "error with ttyD%02d: 16C954 not found\n", 
                       state->line);
#endif
                continue;
            }
#if (LINUX_VERSION_CODE > 0x2032D && defined(CONFIG_DEVFS_FS))
            printk(KERN_INFO "ttd/%02d at iomem 0x%p "
                   "is a %s. (Phys interface %d)\n", state->line,
                   state->iomem_base, "16C954", m77chan->physInt);
#else
            printk(KERN_INFO "ttyD%02d at iomem 0x%p "
                   "is a %s. (Phys interface %d)\n", state->line,
                   state->iomem_base, "16C954", m77chan->physInt);
#endif      
            tty_register_devfs(&serial_driver, 0, 
                               serial_driver.minor_start + state->line);
            tty_register_devfs(&callout_driver, 0, 
                               callout_driver.minor_start + state->line);
        }
    }

    return 0;
}

static void __exit M77_fini(void) 
{
    unsigned long flags;
    int e1, e2;
    int i;
    struct async_struct *info;

    struct M77dev_struct *M77dev;
    struct M77dev_struct *nxtM77dev;
    struct M77chan_struct *M77chan;

    save_flags(flags); cli();
   
    remove_bh(OUR_BH);
    if ((e1 = tty_unregister_driver(&serial_driver)))
        printk("serial: failed to unregister serial driver (%d)\n", e1);
    if ((e2 = tty_unregister_driver(&callout_driver)))
        printk("serial: failed to unregister callout driver (%d)\n", e2);
    restore_flags(flags);

    for (i = 0; i < NR_PORTS; i++) {
        if ((info = rs_table[i].info)) {
            rs_table[i].info = NULL;
            kfree(info);
        }
    }

    M77dev = m77struct;
    while(M77dev){
        nxtM77dev = M77dev->m77dev_next;
        for(i=0; i<M77_PORT_COUNT; i++){
            M77chan = M77dev->m77chan[i];
            if(M77chan){
                kfree(M77chan);
            }
        }
        mdis_close_external_dev( (void *)M77dev->G_dev );
        kfree(M77dev);
        M77dev = nxtM77dev;         
    }

    if (tmp_buf) {
        unsigned long pg = (unsigned long) tmp_buf;
        tmp_buf = NULL;
        free_page(pg);
    }
        
}

module_init(M77_init);
module_exit(M77_fini);

static int InitModuleParm(void)
{
    /* Example: minor-nr 4-7 board=a201_1, slot 2, RS485, no echo
    
       insmod men_serial_m77.o minor=4 brdName=a201_1 slotNo=2 mode=4,4,4,4
       echo=0,0,0,0                                              */

    void   *G_dev;
    MACCESS G_ma;

    struct M77chan_struct   *m77chan = 0;
    struct M77dev_struct    *m77dev = 0;
    char   devName[7];

    int m_idx = 0;
    int c_idx = 0;
    int value;
    int ret;
    u_int8 DCR;

    while(brdName[m_idx]) {
       sprintf(devName, "m77_%d", m_idx+1);
       ret = mdis_open_external_dev(0, devName, brdName[m_idx], slotNo[m_idx],
                                    MDIS_MA08, MDIS_MD16, 
                                    256, (void*)&G_ma, NULL, &G_dev);
       if(ret < 0){
		   printk("mdis_open_external_dev for board %s slot %d failed\n",
				  brdName[m_idx], slotNo[m_idx] );
           return ret;
	   }

       m77dev = kmalloc(sizeof(struct M77dev_struct), GFP_KERNEL);
       if (!m77dev) {
           return -ENOMEM;
       }
       memset(m77dev, 0, sizeof(struct M77dev_struct));

       m77dev->G_dev = G_dev;
       m77dev->G_ma  = G_ma;
       m77dev->brdName = brdName[m_idx];
       m77dev->slotNo  = slotNo[m_idx];
       m77dev->irqVector = 255;	/* dummy */

       for(c_idx=0; c_idx<M77_PORT_COUNT; c_idx++){
           m77chan = kmalloc(sizeof(struct M77chan_struct), GFP_KERNEL);
           if (!m77chan) {
               return -ENOMEM;
           }

           memset(m77chan, 0, sizeof(struct M77chan_struct));

           m77dev->m77chan[c_idx] = m77chan;

           m77chan->m77dev = m77dev;
           m77chan->minor = minor[m_idx] + c_idx;
           m77chan->channel_nr = c_idx;
           m77chan->G_ma = G_ma + (void*)(c_idx*0x10);
           /* iomem_base for the serial driver */

           m77chan->iomem_base = (unsigned char*) G_ma + (0x10*c_idx);

#if (defined(_BIG_ENDIAN_) && (!defined(MAC_BYTESWAP))) || ( defined(_LITTLE_ENDIAN_) && defined(MAC_BYTESWAP) )
	   m77chan->iomem_base++; /* use odd addresses */
#elif (defined(_LITTLE_ENDIAN_) && (!defined(MAC_BYTESWAP))) || ( defined(_BIG_ENDIAN_) && defined(MAC_BYTESWAP) )
	   ;
#else
# error "please specify either _BIG_ENDIAN_ or _LITTLE_ENDIAN_"
#endif

           /*------------------------------------+
           |  set driver configuration register  |
           |  - interface mode, echo             |
           +------------------------------------*/
	   
           DCR = mode[c_idx];
           if((mode[c_idx] == M77_RS422_HD) || (mode[c_idx] == M77_RS485_HD)){
               if(echo[c_idx])
                   DCR |= M77_RX_EN;
           }
           MWRITE_D16(G_ma, M77_DCR_REG_BASE + (0x02*c_idx), DCR);

           m77chan->physInt = mode[c_idx];
           m77chan->echo_supress = echo[c_idx];
       }       

       /* Interrupt register, enable driver and irq */
       value = (M77_IRQ_EN | M77_TX_EN);
       MWRITE_D16(G_ma, M77_IRQ_REG, value);
      
       /* linked list */
       if(m77struct == 0)
           m77struct = m77dev;
       else{
           m77struct->m77dev_prev = m77dev;
           m77dev->m77dev_next = m77struct;
           m77struct = m77dev;
       }

       m_idx++;
    }

    return 0;
}

static int GetM77Chan(u_int8 minor, struct M77chan_struct **m77chan_res)
{
    struct M77dev_struct    *m77dev = 0;
    int i = 0;

    m77dev = m77struct;
    while(m77dev){
        for(i=0; i<M77_PORT_COUNT; i++){
            if((m77dev->m77chan[i]->minor == minor)){
                *m77chan_res = m77dev->m77chan[i];
                return 0;
            }
        }
        m77dev = m77dev->m77dev_next;
    }

    return -1;
}

static int M77PhysIntSet(struct M77chan_struct *m77chan, u_int16 drvMode)
{
    int retVal = 0;

    switch (drvMode){
        case M77_RS423:
            MWRITE_D16(m77chan->m77dev->G_ma, 
                       M77_DCR_REG_BASE + (0x02*m77chan->channel_nr), 
                       M77_RS423);
            break;

        case M77_RS422_HD:
            if(m77chan->echo_supress)
                MWRITE_D16(m77chan->m77dev->G_ma, 
                           M77_DCR_REG_BASE + (0x02*m77chan->channel_nr), 
                           M77_RS422_HD);
            else
                MWRITE_D16(m77chan->m77dev->G_ma, 
                           M77_DCR_REG_BASE + (0x02*m77chan->channel_nr), 
                           M77_RS422_HD | M77_RX_EN);

            break;

        case M77_RS422_FD:
            MWRITE_D16(m77chan->m77dev->G_ma, 
                       M77_DCR_REG_BASE + (0x02*m77chan->channel_nr), 
                       M77_RS422_FD);
            break;

        case M77_RS485_HD:
            if(m77chan->echo_supress)
                MWRITE_D16(m77chan->m77dev->G_ma, 
                           M77_DCR_REG_BASE + (0x02*m77chan->channel_nr), 
                           M77_RS485_HD);
            else
                MWRITE_D16(m77chan->m77dev->G_ma, 
                           M77_DCR_REG_BASE + (0x02*m77chan->channel_nr), 
                           M77_RS485_HD | M77_RX_EN);
	    printk("<1> Reg: %d, DCR: %d\n", 
		   M77_DCR_REG_BASE + (0x02*m77chan->channel_nr), 
		   MREAD_D16(m77chan->m77dev->G_ma, 
			     M77_DCR_REG_BASE + (0x02*m77chan->channel_nr)));
            break;

        case M77_RS485_FD:
            MWRITE_D16(m77chan->m77dev->G_ma, 
                       M77_DCR_REG_BASE + (0x02*m77chan->channel_nr), 
                       M77_RS485_FD);
            break;

        case M77_RS232:
            MWRITE_D16(m77chan->m77dev->G_ma, 
                       M77_DCR_REG_BASE + (0x02*m77chan->channel_nr), 
                       M77_RS232);
            break;

        default:
            drvMode = m77chan->physInt;
            retVal = -ENOTTY;
    }
            
    m77chan->physInt = drvMode;

    return retVal;
}
