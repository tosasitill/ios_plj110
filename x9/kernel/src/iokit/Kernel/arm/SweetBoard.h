/*
 * The boot CPU selects the board from the bounded ADT compatible list after
 * PE_init_platform. Keep MT6789 and MT6993 distinct: neither may enter the
 * Qualcomm fixed-MMIO paths, and X9 must never run an X676C power sequence.
 */
#ifndef _IOKIT_ARM_SWEETBOARD_H
#define _IOKIT_ARM_SWEETBOARD_H

#include <arm64/sweet_early.h>

static inline bool
sweet_board_x676c(void)
{
	return sweet_early_is_x676c() != 0;
}

static inline bool
sweet_board_x9(void)
{
	return sweet_early_is_x9() != 0;
}

static inline bool
sweet_board_qcom(void)
{
	return !sweet_board_x676c() && !sweet_board_x9();
}

#endif /* _IOKIT_ARM_SWEETBOARD_H */
