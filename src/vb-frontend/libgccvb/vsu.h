#ifndef __VSU_H
#define __VSU_H

#include "types.h"



/****** VSU Registers ******/
u8 volatile* const VSU_REGS = (u8*)0x01000400;

/****** VSU Register Mnemonics ******/
#define	S1INT	0x00
#define	S1LRV	0x04
#define	S1FQL	0x08
#define	S1FQH	0x0C
#define	S1EV0	0x10
#define	S1EV1	0x14
#define	S1RAM	0x18

#define	S2INT	0x40
#define	S2LRV	0x44
#define	S2FQL	0x48
#define	S2FQH	0x4C
#define	S2EV0	0x50
#define	S2EV1	0x54
#define	S2RAM	0x58

#define	S3INT	0x80
#define	S3LRV	0x84
#define	S3FQL	0x88
#define	S3FQH	0x8C
#define	S3EV0	0x90
#define	S3EV1	0x94
#define	S3RAM	0x98

#define	S4INT	0xC0
#define	S4LRV	0xC4
#define	S4FQL	0xC8
#define	S4FQH	0xCC
#define	S4EV0	0xD0
#define	S4EV1	0xD4
#define	S4RAM	0xD8

#define	S5INT	0x100
#define	S5LRV	0x104
#define	S5FQL	0x108
#define	S5FQH	0x10C
#define	S5EV0	0x110
#define	S5EV1	0x114
#define	S5RAM	0x118
#define S5SWP	0x11C

#define	S6INT	0x140
#define	S6LRV	0x144
#define	S6FQL	0x148
#define	S6FQH	0x14C
#define	S6EV0	0x150
#define	S6EV1	0x154

#define	SSTOP	0x180

#endif
