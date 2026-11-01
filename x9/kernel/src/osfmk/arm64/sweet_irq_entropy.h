/* X9 timer/IPI delivery is scheduled software activity, not an external
 * entropy source. Keep the existing collector behavior on other SWEET boards.
 * Spurious GIC interrupts have already returned before this predicate. */
#ifndef SWEET_IRQ_ENTROPY_H
#define SWEET_IRQ_ENTROPY_H
static inline int
sweet_irq_entropy_allowed(int is_x9, unsigned int intid, unsigned int timer_intid)
{
 return !is_x9 || (intid >= 16 && intid != timer_intid);
}
#endif
