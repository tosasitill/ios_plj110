/*
 * Copyright (c) 2000-2020 Apple Inc. All rights reserved.
 *
 * This file contains the low-level serial drivers used on ARM/ARM64 devices.
 * The generic serial console code in osfmk/console/serial_console.c will call
 * into this code to transmit and receive serial data.
 *
 * Logging can be performed on multiple serial interfaces at once through a
 * method called serial multiplexing. This is implemented by enumerating which
 * serial interfaces are available on boot and registering them into a linked
 * list of interfaces pointed to by gPESF. When outputting or receiving
 * characters, each interface is queried in turn.
 *
 * Please view doc/arm_serial.md for an in-depth description of these drivers.
 */
#include <kern/clock.h>
#include <kern/debug.h>
#include <libkern/OSBase.h>
#include <libkern/section_keywords.h>
#include <mach/mach_time.h>
#include <machine/atomic.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/protos.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm/consistent_debug.h>
#include <pexpert/arm64/board_config.h>
#include <arm64/proc_reg.h>
#include <arm64/sweet_early.h>
#include <pexpert/arm/protos.h>
#include <kern/sched_prim.h>
#if HIBERNATION
#include <machine/pal_hibernate.h>
#endif /* HIBERNATION */

struct pe_serial_functions {
	/* Initialize the underlying serial hardware. */
	void (*init) (void);

	/* Return a non-zero value if the serial interface is ready to send more data. */
	unsigned int (*transmit_ready) (void);

	/* Write a single byte of data to serial. */
	void (*transmit_data) (uint8_t c);

	/* Return a non-zero value if there's a byte of data available. */
	unsigned int (*receive_ready) (void);

	/* Read a single byte from serial. */
	uint8_t (*receive_data) (void);

	/* Enables IRQs from this device. */
	void (*enable_irq) (void);

	/* Disables IRQs from this device and reports whether IRQs were enabled. */
	bool (*disable_irq) (void);

	/* Clears this device's IRQs targeting this agent, returning true if at least one IRQ was cleared. */
	bool (*acknowledge_irq) (void);

	/**
	 * Whether this serial driver can handle irqs. This value should be set by
	 * querying the device tree to see if the serial device has interrupts
	 * associated with it.
	 *
	 * For a device to support IRQs:
	 *   - enable_irq, disable_irq, and acknowledge_irq must be non-null
	 *   - The AppleSerialShim kext must be able to match to the serial device
	 *     in the IORegistry and call serial_enable_irq with the proper
	 *     serial_device_t
	 *   - The device tree entry for the serial device should have an interrupt
	 *     associated with it.
	 */
	bool has_irq;

	/* enum identifying which serial device these functions belong to. */
	serial_device_t device;

	/* Pointer to the next serial interface in the linked-list. */
	struct pe_serial_functions *next;
};

MARK_AS_HIBERNATE_DATA_CONST_LATE static struct pe_serial_functions* gPESF = NULL;

/**
 * Whether uart has been initialized already. This value is kept across a
 * sleep/wake cycle so we know we need to reinitialize when serial_init is
 * called again after wake.
 */
MARK_AS_HIBERNATE_DATA static bool uart_initted = false;

/* Whether uart should run in simple mode that works during hibernation resume. */
MARK_AS_HIBERNATE_DATA static bool uart_hibernation = false;

/** Set <=> transmission is authorized.
 * Always set, unless SERIALMODE_ON_DEMAND is provided at boot,
 * and no data has yet been received.
 * Originaly meant to be a per-pe_serial_functions variable,
 * but the data protection on the structs prevents it. */
static bool serial_do_transmit = 1;

/**
 * Used to track if all IRQs have been initialized. Each bit of this variable
 * represents whether or not a serial device that reports supporting IRQs has
 * been initialized yet (1 -> not initialized, 0 -> initialized)
 */
static uint32_t serial_irq_status = 0;

/**
 * Set by the 'disable-uart-irq' boot-arg to force serial IRQs into polling mode
 * by preventing the serial driver shim kext from registering itself with
 * serial_enable_irq.
 */
static bool disable_uart_irq = 0;

/**
 * Indicates whether or not a given device's irqs have been set up by calling
 * serial_enable_irq for that particular device.
 *
 * @param device_fns Serial functions for the device that is being checked
 * @return Whether or not the irqs have been initialized for that device
 */
static bool
irq_initialized(struct pe_serial_functions *device_fns)
{
	return (serial_irq_status & device_fns->device) == 0;
}

/**
 * Indicates whether or not a given device supports irqs and if they are ready
 * to be used.
 *
 * @param device_fns Serial functions for the device that is being checked
 * @return Whether or not the device can and will send IRQs.
 */
static bool
irq_available_and_ready(struct pe_serial_functions *device_fns)
{
	return device_fns->has_irq && irq_initialized(device_fns);
}

/**
 * Searches through the global serial functions list and returns the serial function for a particular device
 *
 * @param device The device identifier to search for
 * @return Serial functions for the specified device
 */
static struct pe_serial_functions *
get_serial_functions(serial_device_t device)
{
	struct pe_serial_functions *fns = gPESF;
	while (fns != NULL) {
		if (fns->device == device) {
			return fns;
		}
		fns = fns->next;
	}
	return NULL;
}

/**
 * The action to take when polling and waiting for a serial device to be ready
 * for output. On ARM64, takes a WFE because the WFE timeout will wake us up in
 * the worst case. On ARMv7 devices, we need to hot poll.
 */
static void
serial_poll(void)
{
	#if __arm64__
	__builtin_arm_wfe();
	#endif
}

/**
 * This ensures that if we have a future product that supports hibernation, but
 * doesn't support either UART serial or dock-channels, then hibernation will
 * gracefully fall back to the serial method that is supported.
 */
#if HIBERNATION || defined(APPLE_UART)
MARK_AS_HIBERNATE_DATA static volatile apple_uart_registers_t *apple_uart_registers = 0;
#endif /* HIBERNATION || defined(APPLE_UART) */

#if HIBERNATION || defined(DOCKCHANNEL_UART)
MARK_AS_HIBERNATE_DATA static vm_offset_t dockchannel_uart_base = 0;
#endif /* HIBERNATION || defined(DOCKCHANNEL_UART) */

/*****************************************************************************/

#ifdef APPLE_UART
static int32_t dt_sampling  = -1;
static int32_t dt_ubrdiv    = -1;

static void apple_uart_set_baud_rate(uint32_t baud_rate);

/**
 * The Apple UART is configured to use 115200-8-N-1 communication.
 */
static void
apple_uart_init(void)
{
	ucon_t ucon = { .raw = 0 };
	// Use NCLK (which is constant) instead of PCLK (which is variable).
	ucon.clock_selection = UCON_CLOCK_SELECTION_NCLK;
	ucon.transmit_mode = UCON_TRANSMIT_MODE_INTERRUPT_OR_POLLING;
	ucon.receive_mode = UCON_RECEIVE_MODE_INTERRUPT_OR_POLLING;
	apple_uart_registers->ucon = ucon;

	// Configure 8-N-1 communication.
	ulcon_t ulcon = { .raw = 0 };
	ulcon.word_length = ULCON_WORD_LENGTH_8_BITS;
	ulcon.parity_mode = ULCON_PARITY_MODE_NONE;
	ulcon.number_of_stop_bits = ULCON_STOP_BITS_1;
	apple_uart_registers->ulcon = ulcon;

	apple_uart_set_baud_rate(115200);

	// Enable and reset FIFOs.
	ufcon_t ufcon = { .raw = 0 };
	ufcon.fifo_enable = 1;
	ufcon.tx_fifo_reset = 1;
	ufcon.rx_fifo_reset = 1;
	apple_uart_registers->ufcon = ufcon;
}

static void
apple_uart_enable_irq(void)
{
	// Set the Tx FIFO interrupt trigger level to 0 bytes so interrupts occur when
	// the Tx FIFO is completely empty; this leads to higher Tx throughput.
	apple_uart_registers->ufcon.tx_fifo_interrupt_trigger_level_dma_watermark = UFCON_TX_FIFO_ITL_0_BYTES;

	// Enable Tx interrupts.
	apple_uart_registers->ucon.transmit_interrupt = 1;
}

static bool
apple_uart_disable_irq(void)
{
	/* Disables Tx interrupts */
	ucon_t ucon = apple_uart_registers->ucon;
	const bool irqs_were_enabled = ucon.transmit_interrupt;

	if (irqs_were_enabled) {
		ucon.transmit_interrupt = 0;
		apple_uart_registers->ucon = ucon;
	}

	return irqs_were_enabled;
}

static bool
apple_uart_ack_irq(void)
{
	apple_uart_registers->utrstat.transmit_interrupt_status = 1;
	return true;
}

static void
apple_uart_drain_fifo(void)
{
	while (({
		ufstat_t ufstat = apple_uart_registers->ufstat;
		ufstat.tx_fifo_full || ufstat.tx_fifo_count;
	})) {
		serial_poll();
	}
}

static void
apple_uart_set_baud_rate(uint32_t baud_rate)
{
	uint32_t div = 0;
	const uint32_t uart_clock = (uint32_t)gPEClockFrequencyInfo.fix_frequency_hz;
	uint32_t sample_rate = 16;

	if (baud_rate < 300) {
		baud_rate = 9600;
	}

	if (dt_sampling != -1) {
		// Use the sampling rate specified in the Device Tree
		sample_rate = dt_sampling & 0xf;
	}

	if (dt_ubrdiv != -1) {
		// Use the ubrdiv specified in the Device Tree
		div = dt_ubrdiv & 0xffff;
	} else {
		// Calculate ubrdiv. UBRDIV = (SourceClock / (BPS * Sample Rate)) - 1
		div = uart_clock / (baud_rate * sample_rate);

		uint32_t actual_baud = uart_clock / ((div + 0) * sample_rate);
		uint32_t baud_low    = uart_clock / ((div + 1) * sample_rate);

		// Adjust div to get the closest target baudrate
		if ((baud_rate - baud_low) > (actual_baud - baud_rate)) {
			div--;
		}
	}

	ubrdiv_t ubrdiv = apple_uart_registers->ubrdiv;
	ubrdiv.sample_rate = 16 - sample_rate;
	ubrdiv.ubr_div = div;
	apple_uart_registers->ubrdiv = ubrdiv;
}

MARK_AS_HIBERNATE_TEXT static unsigned int
apple_uart_transmit_ready(void)
{
	return !apple_uart_registers->ufstat.tx_fifo_full;
}

MARK_AS_HIBERNATE_TEXT static void
apple_uart_transmit_data(uint8_t c)
{
	apple_uart_registers->utxh.txdata = c;
}

static unsigned int
apple_uart_receive_ready(void)
{
	const ufstat_t ufstat = apple_uart_registers->ufstat;
	return ufstat.rx_fifo_full || ufstat.rx_fifo_count;
}

static uint8_t
apple_uart_receive_data(void)
{
	return apple_uart_registers->urxh.rxdata;
}

MARK_AS_HIBERNATE_DATA_CONST_LATE
static struct pe_serial_functions apple_serial_functions =
{
	.init = apple_uart_init,
	.transmit_ready = apple_uart_transmit_ready,
	.transmit_data = apple_uart_transmit_data,
	.receive_ready = apple_uart_receive_ready,
	.receive_data = apple_uart_receive_data,
	.enable_irq = apple_uart_enable_irq,
	.disable_irq = apple_uart_disable_irq,
	.acknowledge_irq = apple_uart_ack_irq,
	.device = SERIAL_APPLE_UART
};

#endif /* APPLE_UART */

/*****************************************************************************/

#ifdef DOCKCHANNEL_UART
#define DOCKCHANNEL_WR_MAX_STALL_US (30*1000)

static vm_offset_t      dock_agent_base;
static uint32_t         max_dockchannel_drain_period;
static uint64_t         dockchannel_drain_deadline;  // Deadline for external agent to drain before a software drain occurs
static bool             use_sw_drain;
static uint32_t         dock_wstat_mask;
static uint64_t         prev_dockchannel_spaces;        // Previous w_stat level of the DockChannel.
static uint64_t         dockchannel_stall_grace;
MARK_AS_HIBERNATE_DATA static bool     use_sw_drain;
MARK_AS_HIBERNATE_DATA static uint32_t dock_wstat_mask;

// forward reference
static struct pe_serial_functions dockchannel_serial_functions;

//=======================
// Local funtions
//=======================

static int
dockchannel_drain_on_stall()
{
	// Called when DockChannel runs out of spaces.
	// Check if the DockChannel reader has stalled. If so, empty the DockChannel ourselves.
	// Return number of bytes drained.

	if (mach_absolute_time() >= dockchannel_drain_deadline) {
		// It's been more than DOCKCHANEL_WR_MAX_STALL_US and nobody read from the FIFO
		// Drop a character.
		(void)rDOCKCHANNELS_DOCK_RDATA1(DOCKCHANNEL_UART_CHANNEL);
		os_atomic_inc(&prev_dockchannel_spaces, relaxed);
		return 1;
	}
	return 0;
}

static void
dockchannel_clear_intr(void)
{
	rDOCKCHANNELS_AGENT_AP_INTR_CTRL &= ~(0x3);
	rDOCKCHANNELS_AGENT_AP_INTR_STATUS |= 0x3;
	rDOCKCHANNELS_AGENT_AP_ERR_INTR_CTRL &= ~(0x3);
	rDOCKCHANNELS_AGENT_AP_ERR_INTR_STATUS |= 0x3;
}

static bool
dockchannel_disable_irq(void)
{
	const uint32_t ap_intr_ctrl = rDOCKCHANNELS_AGENT_AP_INTR_CTRL;
	const bool irqs_were_enabled = ap_intr_ctrl & 0x1;
	if (irqs_were_enabled) {
		rDOCKCHANNELS_AGENT_AP_INTR_CTRL = ap_intr_ctrl & ~(0x1);
	}
	return irqs_were_enabled;
}

static void
dockchannel_enable_irq(void)
{
	// set interrupt to be when fifo has 255 empty
	rDOCKCHANNELS_DEV_WR_WATERMARK(DOCKCHANNEL_UART_CHANNEL) = 0xFF;
	rDOCKCHANNELS_AGENT_AP_INTR_CTRL |= 0x1;
}

static bool
dockchannel_ack_irq(void)
{
	/* First check if the IRQ is for the kernel */
	if (rDOCKCHANNELS_AGENT_AP_INTR_STATUS & 0x1) {
		rDOCKCHANNELS_AGENT_AP_INTR_STATUS |= 0x1;
		return true;
	}
	return false;
}

MARK_AS_HIBERNATE_TEXT static void
dockchannel_transmit_data(uint8_t c)
{
	rDOCKCHANNELS_DEV_WDATA1(DOCKCHANNEL_UART_CHANNEL) = (unsigned)c;

	if (use_sw_drain && !uart_hibernation) {
		os_atomic_dec(&prev_dockchannel_spaces, relaxed); // After writing a byte we have one fewer space than previously expected.
	}
}

static unsigned int
dockchannel_receive_ready(void)
{
	return rDOCKCHANNELS_DEV_RDATA0(DOCKCHANNEL_UART_CHANNEL) & 0x7f;
}

static uint8_t
dockchannel_receive_data(void)
{
	return (uint8_t)((rDOCKCHANNELS_DEV_RDATA1(DOCKCHANNEL_UART_CHANNEL) >> 8) & 0xff);
}

MARK_AS_HIBERNATE_TEXT static unsigned int
dockchannel_transmit_ready(void)
{
	uint32_t spaces = rDOCKCHANNELS_DEV_WSTAT(DOCKCHANNEL_UART_CHANNEL) & dock_wstat_mask;

	if (!uart_hibernation) {
		if (use_sw_drain) {
			if (spaces > prev_dockchannel_spaces) {
				// More spaces showed up. That can only mean someone read the FIFO.
				// Note that if the DockFIFO is empty we cannot tell if someone is listening,
				// we can only give them the benefit of the doubt.
				dockchannel_drain_deadline = mach_absolute_time() + dockchannel_stall_grace;
			}
			prev_dockchannel_spaces = spaces;
			return spaces || dockchannel_drain_on_stall();
		}
	}

	return spaces;
}

static void
dockchannel_init(void)
{
	if (use_sw_drain) {
		nanoseconds_to_absolutetime(DOCKCHANNEL_WR_MAX_STALL_US * NSEC_PER_USEC, &dockchannel_stall_grace);
	}

	// Clear all interrupt enable and status bits
	dockchannel_clear_intr();

	// Setup DRAIN timer
	rDOCKCHANNELS_DEV_DRAIN_CFG(DOCKCHANNEL_UART_CHANNEL) = max_dockchannel_drain_period;

	// Drain timer doesn't get loaded with value from drain period register if fifo
	// is already full. Drop a character from the fifo.
	rDOCKCHANNELS_DOCK_RDATA1(DOCKCHANNEL_UART_CHANNEL);
}

MARK_AS_HIBERNATE_DATA_CONST_LATE
static struct pe_serial_functions dockchannel_serial_functions =
{
	.init = dockchannel_init,
	.transmit_ready = dockchannel_transmit_ready,
	.transmit_data = dockchannel_transmit_data,
	.receive_ready = dockchannel_receive_ready,
	.receive_data = dockchannel_receive_data,
	.enable_irq = dockchannel_enable_irq,
	.disable_irq = dockchannel_disable_irq,
	.acknowledge_irq = dockchannel_ack_irq,
	.device = SERIAL_DOCKCHANNEL
};

#endif /* DOCKCHANNEL_UART */

/****************************************************************************/
#ifdef PI3_UART
vm_offset_t pi3_gpio_base_vaddr = 0;
vm_offset_t pi3_aux_base_vaddr = 0;
static unsigned int
pi3_uart_tr0(void)
{
	return (unsigned int) BCM2837_GET32(BCM2837_AUX_MU_LSR_REG_V) & 0x20;
}

static void
pi3_uart_td0(uint8_t c)
{
	BCM2837_PUT32(BCM2837_AUX_MU_IO_REG_V, (uint32_t) c);
}

static unsigned int
pi3_uart_rr0(void)
{
	return (unsigned int) BCM2837_GET32(BCM2837_AUX_MU_LSR_REG_V) & 0x01;
}

static uint8_t
pi3_uart_rd0(void)
{
	return (uint8_t) BCM2837_GET32(BCM2837_AUX_MU_IO_REG_V);
}

static void
pi3_uart_init(void)
{
	// Scratch variable
	uint32_t i;

	// Reset mini uart registers
	BCM2837_PUT32(BCM2837_AUX_ENABLES_V, 1);
	BCM2837_PUT32(BCM2837_AUX_MU_CNTL_REG_V, 0);
	BCM2837_PUT32(BCM2837_AUX_MU_LCR_REG_V, 3);
	BCM2837_PUT32(BCM2837_AUX_MU_MCR_REG_V, 0);
	BCM2837_PUT32(BCM2837_AUX_MU_IER_REG_V, 0);
	BCM2837_PUT32(BCM2837_AUX_MU_IIR_REG_V, 0xC6);
	BCM2837_PUT32(BCM2837_AUX_MU_BAUD_REG_V, 270);

	i = (uint32_t)BCM2837_FSEL_REG(14);
	// Configure GPIOs 14 & 15 for alternate function 5
	i &= ~(BCM2837_FSEL_MASK(14));
	i |= (BCM2837_FSEL_ALT5 << BCM2837_FSEL_OFFS(14));
	i &= ~(BCM2837_FSEL_MASK(15));
	i |= (BCM2837_FSEL_ALT5 << BCM2837_FSEL_OFFS(15));

	BCM2837_PUT32(BCM2837_FSEL_REG(14), i);

	BCM2837_PUT32(BCM2837_GPPUD_V, 0);

	// Barrier before AP spinning for 150 cycles
	__builtin_arm_isb(ISB_SY);

	for (i = 0; i < 150; i++) {
		asm volatile ("add x0, x0, xzr");
	}

	__builtin_arm_isb(ISB_SY);

	BCM2837_PUT32(BCM2837_GPPUDCLK0_V, (1 << 14) | (1 << 15));

	__builtin_arm_isb(ISB_SY);

	for (i = 0; i < 150; i++) {
		asm volatile ("add x0, x0, xzr");
	}

	__builtin_arm_isb(ISB_SY);

	BCM2837_PUT32(BCM2837_GPPUDCLK0_V, 0);

	BCM2837_PUT32(BCM2837_AUX_MU_CNTL_REG_V, 3);
}

SECURITY_READ_ONLY_LATE(static struct pe_serial_functions) pi3_uart_serial_functions =
{
	.init = pi3_uart_init,
	.transmit_ready = pi3_uart_tr0,
	.transmit_data = pi3_uart_td0,
	.receive_ready = pi3_uart_rr0,
	.receive_data = pi3_uart_rd0,
	.device = SERIAL_PI3_UART
};

#endif /* PI3_UART */

/*****************************************************************************/

#ifdef VMAPPLE_UART

static vm_offset_t vmapple_uart0_base_vaddr = 0;

#define PL011_LCR_WORD_LENGTH_8  0x60u
#define PL011_LCR_FIFO_DISABLE   0x00u

#define PL011_LCR_FIFO_ENABLE    0x10u

#define PL011_LCR_ONE_STOP_BIT   0x00u
#define PL011_LCR_PARITY_DISABLE 0x00u
#define PL011_LCR_BREAK_DISABLE  0x00u
#define PL011_IBRD_DIV_38400     0x27u
#define PL011_FBRD_DIV_38400     0x09u
#define PL011_ICR_CLR_ALL_IRQS   0x07ffu
#define PL011_CR_UART_ENABLE     0x01u
#define PL011_CR_TX_ENABLE       0x100u
#define PL011_CR_RX_ENABLE       0x200u

#define VMAPPLE_UART0_DR         *((volatile uint32_t *) (vmapple_uart0_base_vaddr + 0x00))
#define VMAPPLE_UART0_ECR        *((volatile uint32_t *) (vmapple_uart0_base_vaddr + 0x04))
#define VMAPPLE_UART0_FR         *((volatile uint32_t *) (vmapple_uart0_base_vaddr + 0x18))
#define VMAPPLE_UART0_IBRD       *((volatile uint32_t *) (vmapple_uart0_base_vaddr + 0x24))
#define VMAPPLE_UART0_FBRD       *((volatile uint32_t *) (vmapple_uart0_base_vaddr + 0x28))
#define VMAPPLE_UART0_LCR_H      *((volatile uint32_t *) (vmapple_uart0_base_vaddr + 0x2c))
#define VMAPPLE_UART0_CR         *((volatile uint32_t *) (vmapple_uart0_base_vaddr + 0x30))
#define VMAPPLE_UART0_TIMSC      *((volatile uint32_t *) (vmapple_uart0_base_vaddr + 0x38))
#define VMAPPLE_UART0_ICR        *((volatile uint32_t *) (vmapple_uart0_base_vaddr + 0x44))

static unsigned int
vmapple_uart_transmit_ready(void)
{
	return (unsigned int) !(VMAPPLE_UART0_FR & 0x20);
}

static void
vmapple_uart_transmit_data(uint8_t c)
{
	VMAPPLE_UART0_DR = (uint32_t) c;
}

static unsigned int
vmapple_uart_receive_ready(void)
{
	return (unsigned int) !(VMAPPLE_UART0_FR & 0x10);
}

static uint8_t
vmapple_uart_receive_data(void)
{
	return (uint8_t) (VMAPPLE_UART0_DR & 0xff);
}

static void
vmapple_uart_init(void)
{
	VMAPPLE_UART0_CR = 0x0;
	VMAPPLE_UART0_ECR = 0x0;
	VMAPPLE_UART0_LCR_H = (
		PL011_LCR_WORD_LENGTH_8 |
		PL011_LCR_FIFO_ENABLE |
		PL011_LCR_ONE_STOP_BIT |
		PL011_LCR_PARITY_DISABLE |
		PL011_LCR_BREAK_DISABLE
		);
	VMAPPLE_UART0_IBRD = PL011_IBRD_DIV_38400;
	VMAPPLE_UART0_FBRD = PL011_FBRD_DIV_38400;
	VMAPPLE_UART0_TIMSC = 0x0;
	VMAPPLE_UART0_ICR = PL011_ICR_CLR_ALL_IRQS;
	VMAPPLE_UART0_CR = (
		PL011_CR_UART_ENABLE |
		PL011_CR_TX_ENABLE |
		PL011_CR_RX_ENABLE
		);
}

SECURITY_READ_ONLY_LATE(static struct pe_serial_functions) vmapple_uart_serial_functions =
{
	.init = vmapple_uart_init,
	.transmit_ready = vmapple_uart_transmit_ready,
	.transmit_data = vmapple_uart_transmit_data,
	.receive_ready = vmapple_uart_receive_ready,
	.receive_data = vmapple_uart_receive_data,
	.device = SERIAL_VMAPPLE_UART
};

#endif /* VMAPPLE_UART */

/*****************************************************************************/

/**
 * Output @str onto every registered serial interface by polling.
 *
 * @param str The stringto output.
 */
static void uart_sends_force_poll(
	const char *str
	);

static void
register_serial_functions(struct pe_serial_functions *fns)
{
	fns->next = gPESF;
	gPESF = fns;
}

/*****************************************************************************/

/*
 * sweet: framebuffer "serial" for boards with no usable UART (the Redmi Note
 * 10 Pro's debug UART engine runs in SPI mode). kprintf output is drawn onto
 * the boot framebuffer described by the ADT node "sweet-fbcon":
 *   reg    = <phys size>       absolute physical address, not arm-io relative
 *   width, height              pixels
 *   stride                     bytes per line, 32bpp
 * Lines wrap back to the top instead of scrolling; the line after the cursor
 * is kept blank so the newest output is easy to find.
 */

extern unsigned char iso_font[256 * 16];

#define FBCON_SCALE     2
#define FBCON_FG        0xffffffffu
#define FBCON_BG        0xff000000u
#define FBCON_MAX_PIXELS SWEET_FBCON_MAX_PIXELS
#define FBCON_GRID_MAX_COLS 136
#define FBCON_GRID_MAX_ROWS 150

extern int sweet_console_muted;

static volatile uint32_t *fbcon_base;
static uint32_t fbcon_width, fbcon_height, fbcon_stride;
static uint32_t fbcon_cols, fbcon_rows, fbcon_col, fbcon_row;
uint32_t fbcon_shadow[FBCON_MAX_PIXELS] __attribute__((aligned(64)));
static uint8_t fbcon_char_grid[FBCON_GRID_MAX_ROWS][FBCON_GRID_MAX_COLS];

#include "x9_lk_console.h"

/* Used only after X9 GUI composition has published the boot framebuffer. */
int x9_lk_gui_present(void) { return x9_lk_gui_step(); }
int x9_lk_gui_cancel(void) { return x9_lk_gui_disarm(); }
/* Panel DCS long write between frames (AppleCLCD brightness). See x9_lk_dcs_long. */
int x9_lk_panel_dcs(const uint8_t *data, unsigned len, int readback, uint32_t rd[4]) {
 return x9_lk_dcs_long(data, len, readback, rd);
}
int x9_lk_gui_busy(void) {
 return x9_lk_active ? !!(x9_lk_gui_armed || (X9_LK_READ(x9_lk_dsi,0x0c)&0x80000000u)) : -1;
}
/* Read only: known LK buffers and the already used DSI status register.
 * No guessed addresses and no extra MMIO writes. */
/* Once: every EXDMA engine LK left configured, accepted or not. LK's
 * orange-state banner stays on screen while we only refresh the accepted
 * full-screen layer; this names the layer that still carries it. Read only. */
static void x9_lk_engine_dump(void) {
 static int done;if(done)return;done=1;
 for(unsigned i=0;i<13;i++) {
  volatile uint32_t *p=x9_lk_engines[i];if(!p)continue;
  uint32_t en=X9_LK_READ(p,0x20),layer=X9_LK_READ(p,0x40),size=X9_LK_READ(p,0x48),fmt=X9_LK_READ(p,0x50);
  uint32_t high=X9_LK_READ(p,0xf4c);uint64_t address=X9_LK_READ(p,0xf40)|((uint64_t)(high&15)<<32);
  unsigned accepted=0;for(unsigned j=0;j<x9_lk_count;j++)accepted|=x9_lk_addresses[j]==address;
  printf("X9 LK engine: i=%u pa=%llx en=%08x layer=%08x size=%ux%u fmt=%08x pitch=%u off44=%08x off4c=%08x r2f0=%08x r300=%08x addr=%llx f4c=%08x accepted=%u\n",
   i,x9_lk_engine_pa[i],en,layer,size&0x3fffu,(size>>16)&0x3fffu,fmt,X9_LK_READ(p,0x2f4)&0xffffu,
   X9_LK_READ(p,0x44),X9_LK_READ(p,0x4c),X9_LK_READ(p,0x2f0),X9_LK_READ(p,0x300),address,high,accepted);
 }
}
/* Boot token x9lo: discover the buffer of every other enabled LK layer whose
 * memory lies inside LK's framebuffer reservation: the orange-state banner.
 * Discovery only, no display register and no mapping. The clear itself runs in
 * AppleCLCD (see sweet_x9_clear_lk_overlays): pexpert has no IOMemoryDescriptor
 * and ml_io_map_wcomb() would take io_map()'s permanent 8 MiB io_submap, which
 * cannot serve a frame-sized request this late (rev247 lo panicked at 10.28 s,
 * right after the engine dump, with kmem_alloc(14139392) failed with 3
 * = KERN_NO_SPACE @ vm_kern.c:178). */
static uint64_t x9_lk_overlay_address[4];
static uint64_t x9_lk_overlay_bytes[4];
static uint32_t x9_lk_overlay_pitch[4];
static uint32_t x9_lk_overlay_height[4];
static unsigned x9_lk_overlay_engine[4];
static unsigned x9_lk_overlays;
static int x9_lk_overlay_mode;
static void x9_lk_scan_overlays(void) {
 static int done;if(done)return;done=1;
 int v=0;
 /* Default: blank the LK orange-state layer (device-verified harmless,
  * 22.txt/24.txt). x9lo=0 keeps it, x9lo=2 or bare x9ob only observes. */
 if(PE_parse_boot_argn("x9lo",&v,sizeof(v)))
  x9_lk_overlay_mode=v==0?0:(v<0||v==1)?1:2;   /* bare "x9lo" parses as -1 */
 else if(PE_parse_boot_argn("x9ob",&v,sizeof(v)))
  x9_lk_overlay_mode=2;
 else
  x9_lk_overlay_mode=1;
 if(!x9_lk_overlay_mode)return;
 for(unsigned i=0;i<13&&x9_lk_overlays<4;i++) {
  volatile uint32_t *p=x9_lk_engines[i];if(!p)continue;
  uint32_t en=X9_LK_READ(p,0x20),layer=X9_LK_READ(p,0x40),size=X9_LK_READ(p,0x48);
  uint32_t high=X9_LK_READ(p,0xf4c);uint64_t address=X9_LK_READ(p,0xf40)|((uint64_t)(high&15)<<32);
  uint32_t w=size&0x3fffu,h=(size>>16)&0x3fffu,pitch=X9_LK_READ(p,0x2f4)&0xffffu;
  unsigned accepted=0;for(unsigned j=0;j<x9_lk_count;j++)accepted|=x9_lk_addresses[j]==address;
  if(!(en&1)||!(layer&1)||accepted||!w||!h||pitch<w*4||(high&~15u))continue;
  uint64_t bytes=((uint64_t)pitch*h+0x3fff)&~(uint64_t)0x3fff;
  if(address<X9_LK_FB||address+bytes>X9_LK_FB+X9_LK_RESERVE){
   printf("X9 LK overlay: i=%u addr=%llx %ux%u outside LK reservation; left alone\n",i,address,w,h);continue;}
  x9_lk_overlay_address[x9_lk_overlays]=address;
  x9_lk_overlay_bytes[x9_lk_overlays]=bytes;
  x9_lk_overlay_pitch[x9_lk_overlays]=pitch;
  x9_lk_overlay_height[x9_lk_overlays]=h;
  x9_lk_overlay_engine[x9_lk_overlays]=i;
  x9_lk_overlays++;
  printf("X9 LK overlay: i=%u addr=%llx %ux%u pitch=%u %llu B queued (%s)\n",
   i,address,w,h,pitch,(unsigned long long)bytes,x9_lk_overlay_mode==1?"clear":"observe only");
 }
 if(!x9_lk_overlays)printf("X9 LK overlay: none queued\n");
}
int x9_lk_overlay(unsigned index, uint64_t *address, uint64_t *bytes, unsigned *engine,
 uint32_t *pitch, uint32_t *height) {
 if(index>=x9_lk_overlays)return 0;
 *address=x9_lk_overlay_address[index];*bytes=x9_lk_overlay_bytes[index];
 *engine=x9_lk_overlay_engine[index];
 *pitch=x9_lk_overlay_pitch[index];*height=x9_lk_overlay_height[index];
 return 1;
}
/* 1 = blank the queued layer, >=2 = read it and leave it untouched. */
int x9_lk_overlay_level(void) {
 return x9_lk_overlay_mode;
}
void x9_lk_gui_probe(void) {
 if(!x9_lk_active)return;
 x9_lk_engine_dump();
 x9_lk_scan_overlays();
 printf("X9 LK status: dsi=%08x buffers=%u mask=%x id=%u te_armed=%u\n",
     X9_LK_READ(x9_lk_dsi,0x0c),x9_lk_count,x9_lk_mask,x9_lk_id,x9_lk_gui_armed);
 for(unsigned i=0;i<x9_lk_count;i++) {
  uint32_t hash=0,nonblack=0;
  for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++) {
   uint32_t pixel=x9_lk_buffers[i][(y*2759/15)*1280+x*1255/15];
   hash=(hash<<5)|(hash>>27);hash^=pixel;nonblack+=!!(pixel&0xffffffu);
  }
  printf("X9 LK buffer: index=%u pa=%llx sample=256 nonblack=%u checksum=%08x\n",
    i,x9_lk_addresses[i],nonblack,hash);
 }
}


static void
fbcon_clear_row(uint32_t row)
{
	if (sweet_console_muted) {
		return;
	}
	if (row < FBCON_GRID_MAX_ROWS) {
		bzero(&fbcon_char_grid[row], sizeof(fbcon_char_grid[row]));
	}
	uint32_t stride_u64 = fbcon_stride / 8;
	uint32_t width_u64 = fbcon_width / 2;
	uint64_t bg64 = ((uint64_t)FBCON_BG << 32) | FBCON_BG;
	volatile uint64_t *base64 = (volatile uint64_t *)fbcon_base;
	uint64_t *shadow64 = (uint64_t *)fbcon_shadow;

	for (uint32_t y = row * 16 * FBCON_SCALE; y < (row + 1) * 16 * FBCON_SCALE && y < fbcon_height; y++) {
		volatile uint64_t *line_vram = base64 + y * stride_u64;
		uint64_t *line_shadow = shadow64 + y * width_u64;
		for (uint32_t x = 0; x < width_u64; x++) {
			line_shadow[x] = bg64;
			line_vram[x] = bg64;
		}
	}
}

static void
fbcon_scroll(void)
{
	if (sweet_console_muted) {
		return;
	}
	uint32_t cell_h = 16 * FBCON_SCALE;
	uint32_t top = (fbcon_rows - 1) * cell_h;
	uint32_t stride_u64 = fbcon_stride / 8;
	uint32_t width_u64 = fbcon_width / 2;
	uint64_t bg64 = ((uint64_t)FBCON_BG << 32) | FBCON_BG;
	volatile uint64_t *base64 = (volatile uint64_t *)fbcon_base;
	uint64_t *shadow64 = (uint64_t *)fbcon_shadow;

	/* 1. Fast scroll Character Grid in L1 cache (5 KB) */
	if (fbcon_rows > 1 && fbcon_rows <= FBCON_GRID_MAX_ROWS) {
		bcopy(&fbcon_char_grid[1], &fbcon_char_grid[0], (fbcon_rows - 1) * sizeof(fbcon_char_grid[0]));
		bzero(&fbcon_char_grid[fbcon_rows - 1], sizeof(fbcon_char_grid[0]));
	}

	/* 2. Fast scroll inside cached DRAM shadow buffer (L1/L2 hits) */
	bcopy((const void *)(shadow64 + cell_h * width_u64), (void *)shadow64, (size_t)top * width_u64 * 8);

	/* 3. Clear bottom row in shadow buffer */
	for (uint32_t y = top; y < top + cell_h && y < fbcon_height; y++) {
		uint64_t *dst = shadow64 + y * width_u64;
		for (uint32_t x = 0; x < width_u64; x++) {
			dst[x] = bg64;
		}
	}

	/* 4. Stream shadow buffer to physical VRAM using memcpy (fast vector writes) */
	for (uint32_t y = 0; y < top + cell_h && y < fbcon_height; y++) {
		memcpy((void *)(base64 + y * stride_u64), (const void *)(shadow64 + y * width_u64), width_u64 * 8);
	}

	fbcon_row = fbcon_rows - 1;
}

static void
fbcon_newline(void)
{
	fbcon_col = 0;
	if (fbcon_row + 1 >= fbcon_rows) {
		fbcon_scroll();
	} else {
		fbcon_row++;
		fbcon_clear_row(fbcon_row);
	}
}

static void
fbcon_init(void)
{
	fbcon_cols = fbcon_width / (8 * FBCON_SCALE);
	fbcon_rows = fbcon_height / (16 * FBCON_SCALE);
	fbcon_col = fbcon_row = 0;
	for (uint32_t r = 0; r < fbcon_rows; r++) {
		fbcon_clear_row(r);
	}
	x9_lk_present();
}

static unsigned int
fbcon_transmit_ready(void)
{
	return 1;
}

static void
fbcon_transmit_data(uint8_t c)
{
	if (sweet_console_muted) {
		return;
	}
	if (c == '\r') {
		fbcon_col = 0;
		return;
	}
	if (c == '\n') {
		fbcon_newline();
		x9_lk_present();
		return;
	}
	if (fbcon_col >= fbcon_cols) {
		fbcon_newline();
	}
	if (fbcon_row < fbcon_rows && fbcon_col < fbcon_cols &&
	    fbcon_row < FBCON_GRID_MAX_ROWS && fbcon_col < FBCON_GRID_MAX_COLS) {
		fbcon_char_grid[fbcon_row][fbcon_col] = c;
	}
	const unsigned char *glyph = iso_font + c * 16;
	uint32_t x0 = fbcon_col * 8 * FBCON_SCALE, y0 = fbcon_row * 16 * FBCON_SCALE;
	uint32_t stride_px = fbcon_stride / 4;
	for (uint32_t gy = 0; gy < 16; gy++) {
		for (uint32_t sy = 0; sy < FBCON_SCALE; sy++) {
			uint32_t y = y0 + gy * FBCON_SCALE + sy;
			volatile uint32_t *vram_line = fbcon_base + y * stride_px + x0;
			uint32_t *shadow_line = fbcon_shadow + y * fbcon_width + x0;
			for (uint32_t gx = 0; gx < 8; gx++) {
				uint32_t px = (glyph[gy] & (1u << gx)) ? FBCON_FG : FBCON_BG;
				for (uint32_t sx = 0; sx < FBCON_SCALE; sx++) {
					uint32_t off = gx * FBCON_SCALE + sx;
					shadow_line[off] = px;
					vram_line[off] = px;
				}
			}
		}
	}
	fbcon_col++;
	if (fbcon_col == fbcon_cols) {
		x9_lk_present();
	}
}

static unsigned int
fbcon_receive_ready(void)
{
	return 0;
}

static uint8_t
fbcon_receive_data(void)
{
	return 0;
}

SECURITY_READ_ONLY_LATE(static struct pe_serial_functions) sweet_fbcon_serial_functions =
{
	.init = fbcon_init,
	.transmit_ready = fbcon_transmit_ready,
	.transmit_data = fbcon_transmit_data,
	.receive_ready = fbcon_receive_ready,
	.receive_data = fbcon_receive_data,
	.device = SERIAL_SWEET_FBCON
};

static void
fbcon_probe(void)
{
	DTEntry entry;
	uint64_t const *reg;
	uint32_t const *val;
	unsigned int size;

	if (SecureDTFindEntry("name", "sweet-fbcon", &entry) != kSuccess ||
	    SecureDTGetProperty(entry, "reg", (void const **)&reg, &size) != kSuccess || size != 16) {
		return;
	}
	if (SecureDTGetProperty(entry, "width", (void const **)&val, &size) != kSuccess ||
	    size != sizeof(*val)) {
		return;
	}
	fbcon_width = *val;
	if (SecureDTGetProperty(entry, "height", (void const **)&val, &size) != kSuccess ||
	    size != sizeof(*val)) {
		return;
	}
	fbcon_height = *val;
	if (SecureDTGetProperty(entry, "stride", (void const **)&val, &size) != kSuccess ||
	    size != sizeof(*val)) {
		return;
	}
	fbcon_stride = *val;
	if (!sweet_fb_layout_valid(reg[0], reg[1], fbcon_width, fbcon_height, fbcon_stride)) {
		return;
	}
	if (sweet_early_is_x9() && (reg[0] != 0xfc910000ULL ||
	    fbcon_width != 1256 || fbcon_height != 2760 || fbcon_stride != 5120 ||
	    reg[1] > 0x3660000ULL)) {
		return;
	}
	uint64_t frame_bytes = (uint64_t)fbcon_stride * fbcon_height;
	sweet_early_mark(17, "fbcon config read; mapping");
	/* Map one frame, not the entire multi-buffer display carveout. Align the
	 * request ourselves: early io_map otherwise under-reserves VA for an
	 * unaligned physical address that crosses one more kernel page. */
	vm_offset_t offset = reg[0] & 0x3fffULL;
	vm_size_t map_size = (frame_bytes + offset + 0x3fffULL) & ~0x3fffULL;
	vm_offset_t mapping = ml_io_map_wcomb(reg[0] - offset, map_size);
	fbcon_base = mapping ? (volatile uint32_t *)(mapping + offset) : NULL;
	sweet_early_mark(18, "fbcon mapping returned");
	if (fbcon_base != NULL) {
		if (sweet_early_is_x9()) {
			if (x9_lk_prepare(fbcon_base)) {
				sweet_pstore_append("x9 native LK console enabled\n",
				    sizeof("x9 native LK console enabled\n") - 1);
			} else {
				sweet_pstore_append("x9 native LK console validation failed\n",
				    sizeof("x9 native LK console validation failed\n") - 1);
			}
		}
		register_serial_functions(&sweet_fbcon_serial_functions);
	}
}

#if HIBERNATION
/**
 * Transitions the serial driver into a mode that can be run in the hibernation
 * resume context. In this mode, the serial driver runs at a barebones level
 * without making sure the serial devices are properly initialized or utilizing
 * features such as the software drain timer for dockchannels.
 *
 * Upon the next call to serial_init (once the hibernation image has been
 * loaded), this mode is exited and we return to the normal operation of the
 * driver.
 */
MARK_AS_HIBERNATE_TEXT void
serial_hibernation_init(void)
{
	uart_hibernation = true;
#if defined(APPLE_UART)
	apple_uart_registers = (apple_uart_registers_t *)gHibernateGlobals.hibUartRegPhysBase;
#endif /* defined(APPLE_UART) */
#if defined(DOCKCHANNEL_UART)
	dockchannel_uart_base = gHibernateGlobals.dockChannelRegPhysBase;
#endif /* defined(DOCKCHANNEL_UART) */
}

/**
 * Transitions the serial driver back to non-hibernation mode so it can resume
 * normal operations. Should only be called from serial_init on a hibernation
 * resume.
 */
MARK_AS_HIBERNATE_TEXT static void
serial_hibernation_cleanup(void)
{
	uart_hibernation = false;
#if defined(APPLE_UART)
	apple_uart_registers = (apple_uart_registers_t *)gHibernateGlobals.hibUartRegVirtBase;
#endif /* defined(APPLE_UART) */
#if defined(DOCKCHANNEL_UART)
	dockchannel_uart_base = gHibernateGlobals.dockChannelRegVirtBase;
#endif /* defined(DOCKCHANNEL_UART) */
}
#endif /* HIBERNATION */

int
serial_init(void)
{
	DTEntry         entryP = NULL;
	uint32_t        prop_size;
	vm_offset_t     soc_base;
	uintptr_t const *reg_prop;
	uint32_t const  *prop_value __unused = NULL;

	struct pe_serial_functions *fns = gPESF;

	/**
	 * Even if the serial devices have already been initialized on cold boot,
	 * when coming out of a sleep/wake, they'll need to be re-initialized. Since
	 * the uart_initted value is kept across a sleep/wake, always re-initialize
	 * to be safe.
	 */
	if (uart_initted) {
#if HIBERNATION
		if (uart_hibernation) {
			serial_hibernation_cleanup();
		}
#endif /* HIBERNATION */
		while (fns != NULL) {
			fns->init();
			fns = fns->next;
		}

		return 1;
	}

	soc_base = pe_arm_get_soc_base_phys();

	if (soc_base == 0) {
		return 0;
	}

	PE_parse_boot_argn("disable-uart-irq", &disable_uart_irq, sizeof(disable_uart_irq));

	fbcon_probe();

#ifdef PI3_UART
	if (SecureDTFindEntry("name", "gpio", &entryP) == kSuccess) {
		SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size);
		pi3_gpio_base_vaddr = ml_io_map(soc_base + *reg_prop, *(reg_prop + 1));
	}
	if (SecureDTFindEntry("name", "aux", &entryP) == kSuccess) {
		SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size);
		pi3_aux_base_vaddr = ml_io_map(soc_base + *reg_prop, *(reg_prop + 1));
	}
	if ((pi3_gpio_base_vaddr != 0) && (pi3_aux_base_vaddr != 0)) {
		register_serial_functions(&pi3_uart_serial_functions);
	}
#endif /* PI3_UART */

#ifdef VMAPPLE_UART
	if (SecureDTFindEntry("name", "uart0", &entryP) == kSuccess) {
		SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size);
		vmapple_uart0_base_vaddr = ml_io_map(soc_base + *reg_prop, *(reg_prop + 1));
	}

	if (vmapple_uart0_base_vaddr != 0) {
		register_serial_functions(&vmapple_uart_serial_functions);
	}
#endif /* VMAPPLE_UART */

#ifdef DOCKCHANNEL_UART
	uint32_t no_dockchannel_uart = 0;
	if (SecureDTFindEntry("name", "dockchannel-uart", &entryP) == kSuccess) {
		SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size);
		// Should be two reg entries
		if (prop_size / sizeof(uintptr_t) != 4) {
			panic("Malformed dockchannel-uart property");
		}
		dockchannel_uart_base = ml_io_map(soc_base + *reg_prop, *(reg_prop + 1));
		dock_agent_base = ml_io_map(soc_base + *(reg_prop + 2), *(reg_prop + 3));
		PE_parse_boot_argn("no-dockfifo-uart", &no_dockchannel_uart, sizeof(no_dockchannel_uart));
		// Keep the old name for boot-arg
		if (no_dockchannel_uart == 0) {
			register_serial_functions(&dockchannel_serial_functions);
			SecureDTGetProperty(entryP, "max-aop-clk", (void const **)&prop_value, &prop_size);
			max_dockchannel_drain_period = (uint32_t)((prop_value)?  (*prop_value * 0.03) : DOCKCHANNEL_DRAIN_PERIOD);
			prop_value = NULL;
			SecureDTGetProperty(entryP, "enable-sw-drain", (void const **)&prop_value, &prop_size);
			use_sw_drain = (prop_value)?  *prop_value : 0;
			prop_value = NULL;
			SecureDTGetProperty(entryP, "dock-wstat-mask", (void const **)&prop_value, &prop_size);
			dock_wstat_mask = (prop_value)?  *prop_value : 0x1ff;
			prop_value = NULL;
			SecureDTGetProperty(entryP, "interrupts", (void const **)&prop_value, &prop_size);
			if (prop_value) {
				dockchannel_serial_functions.has_irq = true;
			}

			/* Set sane defaults for dockchannel globals. */
			prev_dockchannel_spaces = rDOCKCHANNELS_DEV_WSTAT(DOCKCHANNEL_UART_CHANNEL) & dock_wstat_mask;
			dockchannel_drain_deadline = mach_absolute_time() + dockchannel_stall_grace;
		} else {
			dockchannel_clear_intr();
		}
		// If no dockchannel-uart is found in the device tree, fall back
		// to looking for the traditional UART serial console.
	}

#endif /* DOCKCHANNEL_UART */

#ifdef APPLE_UART
	char const *serial_compat = 0;
	uint32_t use_legacy_uart = 0;

	/* Check if we should enable this deprecated serial device. */
	PE_parse_boot_argn("use-legacy-uart", &use_legacy_uart, sizeof(use_legacy_uart));

	/*
	 * The boot serial port should have a property named "boot-console".
	 * If we don't find it there, look for "uart0" and "uart1".
	 */
	if (SecureDTFindEntry("boot-console", NULL, &entryP) == kSuccess) {
		SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size);
		apple_uart_registers = (apple_uart_registers_t *)ml_io_map(soc_base + *reg_prop, *(reg_prop + 1));
		SecureDTGetProperty(entryP, "compatible", (void const **)&serial_compat, &prop_size);
	} else if (SecureDTFindEntry("name", "uart0", &entryP) == kSuccess) {
		SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size);
		apple_uart_registers = (apple_uart_registers_t *)ml_io_map(soc_base + *reg_prop, *(reg_prop + 1));
		SecureDTGetProperty(entryP, "compatible", (void const **)&serial_compat, &prop_size);
	} else if (SecureDTFindEntry("name", "uart1", &entryP) == kSuccess) {
		SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size);
		apple_uart_registers = (apple_uart_registers_t *)ml_io_map(soc_base + *reg_prop, *(reg_prop + 1));
		SecureDTGetProperty(entryP, "compatible", (void const **)&serial_compat, &prop_size);
	}

	if (NULL != entryP) {
		prop_value = NULL;
		SecureDTGetProperty(entryP, "sampling", (void const **)&prop_value, &prop_size);
		if (prop_value) {
			dt_sampling = *prop_value;
		}

		prop_value = NULL;
		SecureDTGetProperty(entryP, "ubrdiv", (void const **)&prop_value, &prop_size);
		if (prop_value) {
			dt_ubrdiv = *prop_value;
		}

		SecureDTGetProperty(entryP, "interrupts", (void const **)&prop_value, &prop_size);
		if (prop_value) {
			apple_serial_functions.has_irq = true;
		}

		/* Workaround to enable legacy serial for fastsim targets until clients migrate to dockchannels. */
		SecureDTGetProperty(entryP, "enable-legacy-serial", (void const **)&prop_value, &prop_size);
		if (prop_value) {
			use_legacy_uart = 1;
		}
	}

	if (use_legacy_uart && serial_compat && !strcmp(serial_compat, "uart-1,samsung")) {
		register_serial_functions(&apple_serial_functions);
	}
#endif /* APPLE_UART */

	if (gPESF == NULL) {
		return 0;
	}

	fns = gPESF;
	while (fns != NULL) {
		serial_do_transmit = 1;
		fns->init();
		if (fns->has_irq) {
			serial_irq_status |= fns->device; // serial_device_t is one-hot
		}
		fns = fns->next;
	}

#if HIBERNATION
	/* hibernation needs to know the UART register addresses since it can't directly use this serial driver */
	if (dockchannel_uart_base) {
		gHibernateGlobals.dockChannelRegPhysBase = ml_vtophys(dockchannel_uart_base);
		gHibernateGlobals.dockChannelRegVirtBase = dockchannel_uart_base;
		gHibernateGlobals.dockChannelWstatMask = dock_wstat_mask;
	}
	if (apple_uart_registers) {
		gHibernateGlobals.hibUartRegPhysBase = ml_vtophys((vm_offset_t)apple_uart_registers);
		gHibernateGlobals.hibUartRegVirtBase = (vm_offset_t)apple_uart_registers;
	}
#endif /* HIBERNATION */

	/* Complete. */
	uart_initted = true;
	return 1;
}

/**
 * Forbid or allow transmission over each serial until they receive data.
 */
void
serial_set_on_demand(bool on_demand)
{
	/* Enable or disable transmission. */
	serial_do_transmit = !on_demand;

	/* If on-demand is enabled, report it. */
	if (on_demand) {
		uart_sends_force_poll(
			"On-demand serial mode selected.\n"
			"Waiting for user input to send logs.\n"
			);
	}
}

/**
 * Returns a deadline for the longest time the serial driver should wait for an
 * interrupt for. This serves as a timeout for the IRQ to allow for the software
 * drain timer that dockchannels supports.
 *
 * @param fns serial functions representing the device to find the deadline for
 *
 * @returns absolutetime deadline for this device's IRQ.
 */
static uint64_t
serial_interrupt_deadline(__unused struct pe_serial_functions *fns)
{
#if defined(DOCKCHANNEL_UART)
	if (fns->device == SERIAL_DOCKCHANNEL && use_sw_drain) {
		return dockchannel_drain_deadline;
	}
#endif

	/**
	 *  Default to 1.5ms for all other devices. 1.5ms was chosen as the baudrate
	 * of the AppleSerialDevice is 115200, meaning that it should only take
	 * ~1.5ms to drain the 16 character buffer completely.
	 */
	uint64_t timeout_interval;
	nanoseconds_to_absolutetime(1500 * NSEC_PER_USEC, &timeout_interval);
	return mach_absolute_time() + timeout_interval;
}

/**
 * Goes to sleep waiting for an interrupt from a specificed serial device.
 *
 * @param fns serial functions representing the device to wait for
 */
static void
serial_wait_for_interrupt(struct pe_serial_functions *fns)
{
	assert_wait_deadline(fns, THREAD_UNINT, serial_interrupt_deadline(fns));
	if (!fns->transmit_ready()) {
		fns->enable_irq();
		thread_block(THREAD_CONTINUE_NULL);
	} else {
		clear_wait(current_thread(), THREAD_AWAKENED);
	}
}

/**
 * Output a character onto every registered serial interface.
 *
 * @param c The character to output.
 * @param poll Whether the driver should poll to send the character or if it can
 *             wait for an interrupt.
 * @param force : send even if transmission is disabled on the serial.
 */
MARK_AS_HIBERNATE_TEXT void
static
uart_putc_options_(char c, bool poll, bool force)
{
	struct pe_serial_functions *fns = gPESF;

	while (fns != NULL) {
		if (force || serial_do_transmit) {
			while (!fns->transmit_ready()) {
				if (!uart_hibernation) {
					if (!poll && irq_available_and_ready(fns)) {
						serial_wait_for_interrupt(fns);
					} else {
						serial_poll();
					}
				}
			}
			fns->transmit_data((uint8_t)c);
		}
		fns = fns->next;
	}
}

/**
 * Output a character onto every registered serial interface whose
 * transmission is enabled..
 *
 * @param c The character to output.
 * @param poll Whether the driver should poll to send the character or if it can
 *             wait for an interrupt
 */
MARK_AS_HIBERNATE_TEXT void
uart_putc_options(char c, bool poll)
{
	uart_putc_options_(c, poll, 0);
}

/**
 * Output a character onto every registered serial interface whose
 * transmission is enabled by polling.
 *
 * @param c The character to output.
 */
void
uart_putc(char c)
{
	uart_putc_options(c, true);
}

/**
 * Output @str onto every registered serial interface by polling.
 *
 * @param str The stringto output.
 */
static void
uart_sends_force_poll(
	const char *str)
{
	char c;
	while ((c = *(str++))) {
		uart_putc_options_(c, true, true);
	}
}

/**
 * Read a character from the first registered serial interface that has data
 * available.
 *
 * @return The character if any interfaces have data available, otherwise -1.
 */
int
uart_getc(void)
{
	struct pe_serial_functions *fns = gPESF;
	while (fns != NULL) {
		if (fns->receive_ready()) {
			serial_do_transmit = 1;
			return (int)fns->receive_data();
		}
		fns = fns->next;
	}
	return -1;
}

/**
 * Enables IRQs for a specific serial device and returns whether or not IRQs for
 * that device where enabled successfully. For a serial driver to have irqs
 * enabled, it must have the enable_irq, disable_irq, and acknowledge_irq
 * functions defined and the has_irq flag set.
 *
 * @param device Serial device to enable irqs on
 * @note This function should only be called from the AppleSerialShim kext
 */
kern_return_t
serial_irq_enable(serial_device_t device)
{
	struct pe_serial_functions *fns = get_serial_functions(device);

	if (!fns || !fns->has_irq || disable_uart_irq) {
		return KERN_FAILURE;
	}

	serial_irq_status &= ~device;

	return KERN_SUCCESS;
}

/**
 * Performs any actions needed to handle this IRQ. Wakes up the thread waiting
 * on the interrupt if one exists.
 *
 * @param device Serial device that generated the IRQ.
 * @note Interrupts will have already been cleared and disabled by serial_irq_filter.
 * @note This function should only be called from the AppleSerialShim kext.
 */
kern_return_t
serial_irq_action(serial_device_t device)
{
	struct pe_serial_functions *fns = get_serial_functions(device);

	if (!fns || !fns->has_irq) {
		return KERN_FAILURE;
	}

	/**
	 * Because IRQs are enabled only when we know a thread is about to sleep, we
	 * can call wake up and reasonably expect there to be a thread waiting.
	 */
	thread_wakeup(fns);

	return KERN_SUCCESS;
}

/**
 * Returns true if the pending IRQ for device is one that can be handled by the
 * platform serial driver.
 *
 * @param device Serial device that generated the IRQ.
 * @note This function is called from a primary interrupt context and should be
 *       kept lightweight.
 * @note This function should only be called from the AppleSerialShim kext
 */
bool
serial_irq_filter(serial_device_t device)
{
	struct pe_serial_functions *fns = get_serial_functions(device);

	if (!fns || !fns->has_irq) {
		return false;
	}

	/**
	 * Disable IRQs until next time a thread waits for an interrupt to prevent an interrupt storm.
	 */
	const bool had_irqs_enabled = fns->disable_irq();
	const bool was_our_interrupt = fns->acknowledge_irq();

	/* Re-enable IRQs if the interrupt wasn't for us. */
	if (had_irqs_enabled && !was_our_interrupt) {
		fns->enable_irq();
	}

	return was_our_interrupt;
}

/**
 * Prepares all serial devices to go to sleep by draining the hardware FIFOs
 * and disabling interrupts.
 */
void
serial_go_to_sleep(void)
{
	struct pe_serial_functions *fns = gPESF;
	while (fns != NULL) {
		if (irq_available_and_ready(fns)) {
			fns->disable_irq();
		}
		fns = fns->next;
	}

#ifdef APPLE_UART
	/* APPLE_UART needs to drain FIFO before sleeping */
	if (get_serial_functions(SERIAL_APPLE_UART)) {
		apple_uart_drain_fifo();
	}
#endif /* APPLE_UART */
}
