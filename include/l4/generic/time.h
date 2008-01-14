/*
 * System time keeping definitions
 *
 * Copyright (C) 2007 Bahadir Balban
 */

#ifndef __GENERIC_TIMER_H__
#define __GENERIC_TIMER_H__

extern volatile u32 jiffies;

int do_timer_irq(void);

#endif /* __GENERIC_TIMER_H__ */