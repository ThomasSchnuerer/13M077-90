/*********************  P r o g r a m  -  M o d u l e ***********************/
/*!  
 *        \file  serial_m77.h
 *
 *      \author  nw/ts
 *        $Date: 2008/05/05 17:53:23 $
 *    $Revision: 1.5 $
 * 
 *       \brief  Kernel 2.4/2.6 driver Header for M77 
 *				 (M45N/M69N for 2.6 only) Modules 
 *
 *     Switches: -
 */
/*-------------------------------[ History ]---------------------------------
 *
 * $Log: serial_m77.h,v $
 * Revision 1.5  2008/05/05 17:53:23  ts
 * changed comment about usage of readb()/writeb() to MREAD_D16/MWRITE_D16
 *
 * Revision 1.4  2007/08/13 15:10:34  ts
 * Release Version, Cosmetics
 *
 * Revision 1.3  03.08.2007 16:06:06  ts
 * ioctl M45_TIO_TRI_MODE for M45N added
 * Cosmetics
 *
 * Revision 1.2  25.06.2007 17:07:32 by ts
 * + additional defines for new 2.6 driver
 *
 * Revision 1.1  11.06.2003 10:06:08 by kp
 * Initial Revision
 *
 *---------------------------------------------------------------------------
 * (c) Copyright 2007 by MEN mikro elektronik GmbH, Nuremberg, Germany 
 ****************************************************************************/
#ifndef _LINUX_SERIAL_M77_H
#define _LINUX_SERIAL_M77_H

/* 
 * Interrupt Registers, not shifted because used in the ISR directly with
 * M_WRITE/READ_D16 to shorten the ISR latency
 */
#define  M45_REG_IR1			(0x48)
#define  M45_REG_IR2			(0xc8)
#define  M69_REG_IR				(0x48)
#define  M77_REG_IR				(0x48)

/*
 * Other Addresses here are shifted << 1 in serial_in()/out() because
 * the MM Interface has no A0 bit
 */
#define M77_DCR_REG_BASE		0x20	/* Driver Configuration Registers */
#define M45_TCR_TRISTATE0		0x1
#define M45_TCR_TRISTATE1		0x2
#define M45_TCR_TRISTATE2		0x4
#define M45_TCR_TRISTATE3		0x1
#define M45_TCR_TRISTATE4		0x2

#define M45_TCR1_REG			0x20
#define M45_TCR2_REG			0x60

#define M77_EFR_OFFSET			0x02	/* enhanced features register 	*/
#define M77_XON1_OFFSET			0x04	/* XON1 flow control character  */
#define M77_XON2_OFFSET			0x05	/* XON2 flow control character 	*/
#define M77_XOFF1_OFFSET		0x06	/* XOFF1 flow control character */
#define M77_XOFF2_OFFSET		0x07    /* XOFF2 flow control character */

/* automatic Xon Xoff flow control values */
#define M77_XON_CHAR			17		/* Xon character = ^Q */
#define M77_XOFF_CHAR			19		/* Xoff character = ^S */


/* see Data sheet p.38  "ACR[4:3] DTR# line Configuration" */
#define OX954_ACR_DTR			(0x18)

/* Module Type Identification:  */
#define MOD_M45					0x7d2d
#define MOD_M69					0x7d45
#define MOD_M77					0x004d	/* No 'New' Module = No Offset 0x7d00 */

/* how many UARTs per module ? */
#define MOD_M45_CHAN_NUM		8
#define MOD_M69_CHAN_NUM		4
#define MOD_M77_CHAN_NUM		4

/*
 *  M77 special ioctl functions
 */
#define M77_IOCTL_MAGIC			't'
#define M77_IOCTLBASE			240

/*  M45N special ioctl for Tristate Modes */
#define M45_TIO_TRI_MODE 	_IO(M77_IOCTL_MAGIC, M77_IOCTLBASE + 2)

/*  M77 special ioctl function for physical Modes */
#define M77_PHYS_INT_SET   _IO(M77_IOCTL_MAGIC, M77_IOCTLBASE + 1)

/*  M77 special ioctl functions for echo Modes */
#define M77_ECHO_SUPPRESS  _IO(M77_IOCTL_MAGIC, M77_IOCTLBASE + 0)


/* M77 special M77_PHYS_INT_SET ioctl arguments */
#define M77_RS423        0x00  /*  arg for RS423 , OBSOLETE on new M77 */
#define M77_RS422_HD     0x01  /*  arg for RS422 half duplex */
#define M77_RS422_FD     0x02  /*  arg for RS422 full duplex */
#define M77_RS485_HD     0x03  /*  arg for RS485 half duplex */
#define M77_RS485_FD     0x04  /*  arg for RS485 full duplex */
#define M77_RS232        0x07  /*  arg for RS232 			 */

#define M77_RX_EN        0x08  /* RX_EN bit mask */
#define M77_IR_DRVEN     0x04  /* IR Register Driver enable bit 			*/
#define M77_IR_IMASK     0x02  /* IR Register IRQ Mask (IRQ dis/enable bit) */
#define M77_IR_IRQ     	 0x01  /* IR Register IRQ pending bit				*/


#endif /* _LINUX_SERIAL_M77_H */

