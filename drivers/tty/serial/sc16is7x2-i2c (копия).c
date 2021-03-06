#include <linux/kernel.h>
#include <linux/slab.h>
//#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>

#include <linux/platform_data/sc16is7x2.h>

#include <linux/serial_reg.h>
#include <linux/serial_core.h>
#include <linux/freezer.h>
#include <linux/delay.h>
#include <linux/gpio.h>

#include<linux/time.h>

#define DEBUG

#define DRIVER_NAME		"sc16is7x2-i2c"
#define TYPE_NAME		"SC16IS7x2-I2C"

#define MAX_SC16IS7X2		8
#define FIFO_SIZE		64

#define REG_READ	0x80
#define REG_WRITE	0x00

/* Special registers */
#define REG_SPR	    0x07	/* Test character register */
#define REG_TXLVL	0x08	/* Transmitter FIFO Level register */
#define REG_RXLVL	0x09	/* Receiver FIFO Level register */
#define REG_IOD		0x0A	/* IO Direction register */
#define REG_IOS		0x0B	/* IO State register */
#define REG_IOI		0x0C	/* IO Interrupt Enable register */
#define REG_IOC		0x0E	/* IO Control register */
#define REG_EFCR	0x0F	/* Extra Features Control Register */


#define IOC_SRESET	0x08    /* Software reset */
#define IOC_GPIO30	0x04    /* GPIO 3:0 unset: as IO, set: as modem pins */
#define IOC_GPIO74	0x02    /* GPIO 7:4 unset: as IO, set: as modem pins */
#define IOC_IOLATCH	0x01    /* Unset: input unlatched, set: input latched */

#define EFCR_RTSINVER 0x20  /* Invert not-RTS signal in RS-485 mode */
#define EFCR_RTSCON   0x10  /* Auto RS-485 mode: Enable the transmitter to control the RTS pin. */


struct sc16is7x2_chip;

struct timespec ts_start,ts_end,test_of_time;

static struct uart_driver sc16is7x2_uart_driver = {
    .owner          = THIS_MODULE,
    .driver_name    = DRIVER_NAME,
    .dev_name       = "ttyNSC",
    .nr             = MAX_SC16IS7X2,
};

/*
 * Some registers must be read back to modify.
 * To save time we cache them here in memory.
 */
struct sc16is7x2_channel {
    struct sc16is7x2_chip	*chip;	/* back link */
    struct uart_port	uart;

    /* Workqueue that does all the magic */
    struct workqueue_struct *workqueue;
    struct work_struct	work;

    u16		quot;		/* baud rate divisor */
    u8		iir;		/* state of IIR register */
    u8		lsr;		/* state of LSR register */
    u8		msr;		/* state of MSR register */
    u8      ier ;		/* cache for IER register */    u8		fcr;		/* cache for FCR register */
    u8		lcr;		/* cache for LCR register */
    u8		mcr;		/* cache for MCR register */
    u8		efr;		/* cache for EFR register */
    u8      efcr;       /* cache for EFCR register */

#ifdef DEBUG
    bool		handle_irq;
#endif
    bool		handle_baud;	/* baud rate needs update */
    bool		handle_regs;	/* other regs need update */
    u8		buf[1024]; /* fifo transfer buffer */
};

/* Each client has this additional data */
struct sc16is7x2_chip {
    struct i2c_client *client;

    struct sc16is7x2_channel channel[2];

    unsigned int	uartclk;
    /* uart line number of the first channel */
    unsigned	uart_base;
    /* number assigned to the first GPIO */
    unsigned	gpio_base;

    char		*gpio_label;
    /* list of GPIO names (array length = SC16IS7X2_NR_GPIOS) */
    const char	*const *gpio_names;

#ifdef CONFIG_GPIOLIB
    struct gpio_chip gpio;
    struct mutex	io_lock;	/* lock for GPIO functions */
    u8		io_dir;		/* cache for IODir register */
    u8		io_state;	/* cache for IOState register */
    u8		io_gpio;	/* PIN is GPIO */
    u8		io_control;	/* cache for IOControl register */
#endif
};

static inline u8 write_cmd(u8 reg, u8 ch)
{
    return ((reg & 0xf) << 3 | (ch & 0x3) << 1);
}

static inline u8 read_cmd(u8 reg, u8 ch)
{
    return write_cmd(reg, ch);
}

/*
 * sc16is7x2_write - Write a new register content (sync)
 * @reg: Register offset
 * @ch:  Channel (0 or 1)
 */
static int sc16is7x2_write(struct i2c_client *client, u8 reg, u8 ch, u8 val)
{
    u8 out;
    out = write_cmd(reg, ch);

    s32 ret = i2c_smbus_write_byte_data(client, out, val);

    printk("   %s (ch%i) %d bytes written to reg %x val %x reg_dev %x \n", __func__, ch, ret, reg, val, out);

    return ret;
}

/**
 * sc16is7x2_read - Read back register content
 * @reg: Register offset
 * @ch:  Channel (0 or 1)
 *
 * Returns positive 8 bit value from the device if successful or a
 * negative value on error
 */
static int sc16is7x2_read(struct i2c_client *client, unsigned reg, unsigned ch)
{
    s32 ret = i2c_smbus_read_byte_data(client, read_cmd(reg, ch));
    printk("sc16is7x2_read: %x ", ret);
    return ret;
}


/* ******************************** IRQ ********************************* */
/* Trigger work thread*/
static void sc16is7x2_dowork(struct sc16is7x2_channel *chan)
{
    printk("sc16is7x2_dowork \n");

    if(!freezing(current))
    {
        queue_work(chan->workqueue, &chan->work);
    }
}

static irqreturn_t sc16is7x2_irq(int irq, void *data)
{
    printk("%s \n", __func__);

    struct sc16is7x2_channel *chan = data;

#ifdef DEBUG
    /* Indicate irq */
    chan->handle_irq = true;
#endif

    getnstimeofday(&ts_start);
    printk("Curr time: %d ", ts_start.tv_nsec);

    /* Trigger work thread */
    sc16is7x2_dowork(chan);
   return IRQ_WAKE_THREAD;
}

/* ********************************  ********************************* */
static int sc16is7x2_handle_rx(struct i2c_client *client, unsigned ch)
{
    printk("%s channel %d\n", __func__, ch);
    struct sc16is7x2_chip *ts = i2c_get_clientdata(client);
    struct sc16is7x2_channel *chan = &ts->channel[ch];

    getnstimeofday(&ts_end);
    test_of_time = timespec_sub(ts_end,ts_start);

    printk("Curr time: %d ", ts_end.tv_nsec);
    printk("Period: %d ", test_of_time.tv_nsec);

    unsigned long flags;
    u8 lsr = chan->lsr;
    u8 iir = 0;
    u8 fcr = 0;

    int rxlvl=0;
    int reg_rxlvl=0;

    int i, j;
    i=0;

    unsigned num_read;
    num_read = 0;

    iir = sc16is7x2_read(client, UART_IIR, ch);

    switch (iir & 0x3F) {
        case UART_IIR_RDI:
            printk(KERN_INFO "Pending interrupt: Receiver data interrupt");
            break;
        case UART_IIR_RLSI:
            printk(KERN_INFO "Pending interrupt: Receiver line status interrupt");
            break;
        case UART_IIR_RTOI:
            printk(KERN_INFO "Pending interrupt: Receiver time-out interrupt");
            break;
        default:
            printk(KERN_INFO "Pending interrupt: Interrupt is not registered: %08x\n", iir);
            //return;
    }

    fcr = sc16is7x2_read(client, UART_FCR, ch);
    printk(KERN_INFO "FCR: %08x\n", fcr);
    printk(KERN_INFO "TLR: %08x\n", sc16is7x2_read(client, 7, ch));
    printk(KERN_INFO "LCR: %08x\n", sc16is7x2_read(client, UART_LCR, ch));
    printk(KERN_INFO "MCR: %08x\n", sc16is7x2_read(client, UART_MCR, ch));
    printk(KERN_INFO "EFR: %08x\n", sc16is7x2_read(client, UART_EFR, ch));
    printk(KERN_INFO "LSR: %08x\n", sc16is7x2_read(client, UART_LSR, ch));
    printk(KERN_INFO "IER: %08x\n", sc16is7x2_read(client, UART_IER, ch));
    printk(KERN_INFO "EFCR: %08x\n", sc16is7x2_read(client, 0xF, ch));

    rxlvl = sc16is7x2_read(client, REG_RXLVL, ch);
    if (rxlvl <= 0) {
        printk(KERN_INFO "ERROR: rxlvl<=0 ");
        printk(KERN_INFO "RXLVL = : %08x\n", rxlvl);
        return;
    } else if (rxlvl > FIFO_SIZE) {
        /* Ensure sanity of RX level */
        printk(KERN_INFO "ERROR: rxlvl>FIFO_SIZE");
        rxlvl = FIFO_SIZE;
    }
    // Read data from FIFO
    printk("%s RXLVL = %d \n", __func__, rxlvl);
    rxlvl = i2c_smbus_read_i2c_block_data(client, read_cmd(UART_RX, ch), rxlvl, &chan->buf[i]);
    printk("%s reads bytes = %d \n", __func__, rxlvl);
    i+=rxlvl;

    rxlvl = sc16is7x2_read(client, REG_RXLVL, ch);
    if (rxlvl > 0) {
        printk("%s FIFO estimated bytes = %d \n", __func__, sc16is7x2_read(client, REG_RXLVL, ch));
        rxlvl = i2c_smbus_read_i2c_block_data(client, read_cmd(UART_RX, ch), rxlvl, &chan->buf[i]);
        printk("%s reads bytes = %d \n", __func__, rxlvl);
        i+=rxlvl;
        printk("%s FIFO estimated bytes = %d \n", __func__, sc16is7x2_read(client, REG_RXLVL, ch));
    }
//    for(j=0; j<i; j++)
//              printk("%s chan->buf[%d] = %x \n", __func__, j, chan->buf[j]);


    struct uart_port *uart = &chan->uart;
    struct tty_struct *tty = uart->state->port.tty;

    spin_lock_irqsave(&uart->lock, flags);

    if (unlikely(lsr & UART_LSR_BRK_ERROR_BITS)) {
        /*
         * For statistics only
         */
        if (lsr & UART_LSR_BI) {
            lsr &= ~(UART_LSR_FE | UART_LSR_PE);
            chan->uart.icount.brk++;
            /*
             * We do the SysRQ and SAK checking
             * here because otherwise the break
             * may get masked by ignore_status_mask
             * or read_status_mask.
             */
            if (uart_handle_break(&chan->uart))
            {
                printk("uart_handle_break \n");
                goto ignore_char;
            }
        } else if (lsr & UART_LSR_PE)
            chan->uart.icount.parity++;
        else if (lsr & UART_LSR_FE)
            chan->uart.icount.frame++;
        if (lsr & UART_LSR_OE)
            chan->uart.icount.overrun++;
    }

    rxlvl = i;
    /* Insert received data */
    uart->icount.rx += rxlvl;
    printk(KERN_INFO "tty_insert_flip_string\n");
    tty_insert_flip_string(tty, &chan->buf[0], rxlvl);

ignore_char:
    spin_unlock_irqrestore(&uart->lock, flags);

    /* Push the received data to receivers */
    if (rxlvl) {
        printk(KERN_INFO "tty_flip_buffer_push\n");
        tty_flip_buffer_push(tty);
    }

    rxlvl = sc16is7x2_read(client, REG_RXLVL, ch);
    printk("%s RXLVL = %d \n", __func__, rxlvl);
    return rxlvl;
}


static void sc16is7x2_handle_tx(struct i2c_client *client, unsigned ch)
{
    printk("%s \n", __func__);
 //   return;

    struct sc16is7x2_chip *ts = i2c_get_clientdata(client);
    struct sc16is7x2_channel *chan = &ts->channel[ch];
    struct uart_port *uart = &chan->uart;
    struct circ_buf *xmit = &uart->state->xmit;
    unsigned long flags;
    unsigned i, len;
    int txlvl;

    if (chan->uart.x_char && chan->lsr & UART_LSR_THRE) {
        printk("tx: x-char\n");
        sc16is7x2_write(client, UART_TX, ch, uart->x_char);
        uart->icount.tx++;
        uart->x_char = 0;
        return;
    }
    if (uart_circ_empty(xmit) || uart_tx_stopped(&chan->uart)) {
        /* No data to send or TX is stopped */
        printk("No data to send or TX is stopped\n");
        return;
    }

    txlvl = sc16is7x2_read(client, REG_TXLVL, ch);
    if (txlvl <= 0) {
        printk("%s (%i) fifo full\n", __func__, ch);
        return;
    }

    txlvl = min(txlvl, 30);

    /* number of bytes to transfer to the fifo */

    //    printk("%s (%i) %d txlvl\n", __func__, ch, txlvl);

    len = min(txlvl, (int)uart_circ_chars_pending(xmit));

    //    printk("%s (%i) %d bytes\n", __func__, ch, len);

    printk("%s (%i) %d bytes \n", __func__, ch, len);

    spin_lock_irqsave(&uart->lock, flags);
    for (i = 0; i <= len-1 ; i++)
    {
        chan->buf[i] = xmit->buf[xmit->tail];
        xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);

        printk(" %s chan->buf[%i] = %d \n", __func__, i, chan->buf[i]);
    }
    uart->icount.tx += len;
    spin_unlock_irqrestore(&uart->lock, flags);


    //    chan->buf[0] = write_cmd(UART_TX, ch);

    i2c_smbus_write_block_data(client, write_cmd(UART_TX, ch), len, chan->buf);
    //    printk("client transfer TX handling failed \n");

    if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
        uart_write_wakeup(uart);
}

static void sc16is7x2_handle_baud(struct i2c_client *client, unsigned ch)
{
    struct sc16is7x2_chip *ts = i2c_get_clientdata(client);
    struct sc16is7x2_channel *chan = &ts->channel[ch];

    printk("%s\n", __func__);
    if (!chan->handle_baud)
        return;

    printk("%s\n", __func__);

    sc16is7x2_write(client, UART_IER, ch, 0);
    sc16is7x2_write(client, UART_LCR, ch, UART_LCR_DLAB); /* access DLL&DLM */
    sc16is7x2_write(client, UART_DLL, ch, chan->quot & 0xff);
    sc16is7x2_write(client, UART_DLM, ch, chan->quot >> 8);
    sc16is7x2_write(client, UART_LCR, ch, chan->lcr);     /* reset DLAB */

    chan->handle_baud = false;
}

static void sc16is_clear_fifos(struct i2c_client *client, unsigned ch)
{
    printk("%s\n", __func__);
    sc16is7x2_write(client, UART_FCR, ch, UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
    sc16is7x2_write(client, UART_FCR, ch, UART_FCR_ENABLE_FIFO);
}

static void sc16is7x2_handle_regs(struct i2c_client *client, unsigned ch)
{
    struct sc16is7x2_chip *ts = i2c_get_clientdata(client);
    struct sc16is7x2_channel *chan = &ts->channel[ch];

    if (!chan->handle_regs)
        return;

    printk("%s\n", __func__);

    sc16is7x2_write(client, UART_LCR, ch, 0xBF);  /* access EFR */
    sc16is7x2_write(client, UART_EFR, ch, chan->efr);
    sc16is7x2_write(client, UART_LCR, ch, chan->lcr);
    sc16is7x2_write(client, UART_FCR, ch, chan->fcr);
    sc16is7x2_write(client, UART_MCR, ch, chan->mcr);
    sc16is7x2_write(client, UART_IER, ch, chan->ier);

    chan->handle_regs = false;
}

static void sc16is7x2_read_status(struct i2c_client *client, unsigned ch)
{
    struct sc16is7x2_chip *ts = i2c_get_clientdata(client);
    struct sc16is7x2_channel *chan = &ts->channel[ch];
    u8 ier;

#ifdef DEBUG

    //~ ier = sc16is7x2_read(client, UART_IER, ch);
#endif
   printk("%s channel %d\n", __func__, ch);


    ier = sc16is7x2_read(client, UART_IER, ch);

    chan->iir = sc16is7x2_read(client, UART_IIR, ch);
    chan->msr = sc16is7x2_read(client, UART_MSR, ch);
    chan->lsr = sc16is7x2_read(client, UART_LSR, ch);

    //    chan->fcr = sc16is7x2_read(client, UART_FCR, ch); //only writable
    chan->efcr = sc16is7x2_read(client, REG_EFCR, ch);

    //    printk("%s ier=0x%02x iir=0x%02x msr=0x%02x lsr=0x%02x efcr=0x%02x \n", __func__, ier, chan->iir, chan->msr, chan->lsr, chan->efcr);

}

static void sc16is7x2_handle_channel(struct work_struct *w)
{
    struct sc16is7x2_channel *chan = container_of(w, struct sc16is7x2_channel, work);
    struct sc16is7x2_chip *ts = chan->chip;
    unsigned ch = (chan == ts->channel) ? 0 : 1;

    sc16is7x2_read_status(ts->client, ch); // tmp

#ifdef DEBUG
    printk("%s (%i) %s\n", __func__, ch,
           chan->handle_irq ? "irq" : "");
    chan->handle_irq = false;
#endif

    do {
        sc16is7x2_handle_baud(ts->client, ch);
        sc16is7x2_handle_regs(ts->client, ch);
        sc16is7x2_read_status(ts->client, ch);

        sc16is7x2_handle_tx(ts->client, ch);
        sc16is7x2_handle_rx(ts->client, ch);
    } while (!(chan->iir & UART_IIR_NO_INT));

    printk("%s finished\n", __func__);
}

/* ******************************** UART ********************************* */

#define to_sc16is7x2_channel(port) \
    container_of(port, struct sc16is7x2_channel, uart)

static unsigned int sc16is7x2_tx_empty(struct uart_port *port)
{
    struct sc16is7x2_channel *chan = to_sc16is7x2_channel(port);
    struct sc16is7x2_chip *ts = chan->chip;
    unsigned lsr;

    printk("%s = %s\n", __func__,
           chan->lsr & UART_LSR_TEMT ? "yes" : "no");

    lsr = chan->lsr;
    return lsr & UART_LSR_TEMT ? TIOCSER_TEMT : 0;
}

static unsigned int sc16is7x2_get_mctrl(struct uart_port *port)
{
    struct sc16is7x2_channel *chan = to_sc16is7x2_channel(port);
    struct sc16is7x2_chip *ts = chan->chip;
    unsigned int status;
    unsigned int ret;

    //    printk("%s (0x%02x)\n", __func__, chan->msr);

    status = chan->msr;

    ret = 0;
    if (status & UART_MSR_DCD)
        ret |= TIOCM_CAR;
    if (status & UART_MSR_RI)
        ret |= TIOCM_RNG;
    if (status & UART_MSR_DSR)
        ret |= TIOCM_DSR;
    if (status & UART_MSR_CTS)
        ret |= TIOCM_CTS;
    return ret;
}

static void sc16is7x2_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
    struct sc16is7x2_channel *chan = to_sc16is7x2_channel(port);
    struct sc16is7x2_chip *ts = chan->chip;

    printk("%s (0x%02x)\n", __func__, mctrl);

    /* TODO: set DCD and DSR
     * CTS/RTS is handled automatically
     */
}

static void sc16is7x2_stop_tx(struct uart_port *port)
{
    /* Do nothing */
}

static void sc16is7x2_start_tx(struct uart_port *port)
{
    struct sc16is7x2_channel *chan = to_sc16is7x2_channel(port);
    struct sc16is7x2_chip *ts = chan->chip;

    printk("%s \n", __func__);

    /* Trigger work thread for sending data */
    sc16is7x2_dowork(chan);
}

static void sc16is7x2_stop_rx(struct uart_port *port)
{
    struct sc16is7x2_channel *chan = to_sc16is7x2_channel(port);
    struct sc16is7x2_chip *ts = chan->chip;

    //    printk("%s\n", __func__);

    chan->ier &= ~UART_IER_RLSI;
    chan->uart.read_status_mask &= ~UART_LSR_DR;
    chan->handle_regs = true;
    /* Trigger work thread for doing the actual configuration change */
    sc16is7x2_dowork(chan);
}

static void sc16is7x2_enable_ms(struct uart_port *port)
{
    struct sc16is7x2_channel *chan = to_sc16is7x2_channel(port);
    //    struct sc16is7x2_chip *ts = chan->chip;

    //    printk("%s\n", __func__);

    chan->ier |= UART_IER_MSI;
    chan->handle_regs = true;
    /* Trigger work thread for doing the actual configuration change */
    sc16is7x2_dowork(chan);
}

static void sc16is7x2_break_ctl(struct uart_port *port, int break_state)
{
    /* We don't support break control yet, do nothing */
}

static int sc16is7x2_startup(struct uart_port *port)
{
    struct sc16is7x2_channel *chan = to_sc16is7x2_channel(port);
    struct sc16is7x2_chip *ts = chan->chip;
    struct i2c_client *client = ts->client;

    unsigned ch = (&ts->channel[1] == chan) ? 1 : 0;
    unsigned long flags;

    printk(KERN_INFO "sc16is7x2_startup");

    /* Clear the interrupt registers. */
    sc16is7x2_write(client, UART_LCR, ch, UART_LCR_WLEN8);
    sc16is7x2_write(client, UART_IER, ch, 0);
    sc16is7x2_read_status(client, ch);

    /* Initialize work queue */
    chan->workqueue = create_workqueue(DRIVER_NAME);
    if (!chan->workqueue) {
        printk("Workqueue creation failed\n");
        return -EBUSY;
    }
    INIT_WORK(&chan->work, sc16is7x2_handle_channel);

      /* Setup IRQ. Actually we have a low active IRQ, but we want
        * one shot behaviour */
    if (request_threaded_irq(client->irq, NULL, sc16is7x2_irq, IRQF_TRIGGER_FALLING | IRQF_SHARED, DRIVER_NAME, chan)) {
        printk("IRQ request failed\n");
        destroy_workqueue(chan->workqueue);
        chan->workqueue = NULL;
        return -EBUSY;
    }
    spin_lock_irqsave(&chan->uart.lock, flags);
    chan->lcr = UART_LCR_WLEN8;
    chan->mcr = 0;
    chan->fcr = 0;
    chan->ier = UART_IER_RLSI | UART_IER_RDI | UART_IER_THRI;
    spin_unlock_irqrestore(&chan->uart.lock, flags);

    sc16is7x2_write(client, UART_FCR, ch, UART_FCR_ENABLE_FIFO | UART_FCR_CLEAR_RCVR | UART_FCR_CLEAR_XMIT);
    sc16is7x2_write(client, UART_FCR, ch, chan->fcr);

    /* Now, initialize the UART */
    sc16is7x2_write(client, UART_LCR, ch, chan->lcr);
    sc16is7x2_write(client, UART_MCR, ch, chan->mcr);
    sc16is7x2_write(client, UART_IER, ch, chan->ier);

    sc16is7x2_write(client, REG_EFCR, ch, chan->efcr);  // RS-232

    sc16is7x2_handle_baud(ts->client, ch);
    sc16is7x2_handle_regs(ts->client, ch);

    return 0;
}

static void sc16is7x2_shutdown(struct uart_port *port)
{
    struct sc16is7x2_channel *chan = to_sc16is7x2_channel(port);
    struct sc16is7x2_chip *ts = chan->chip;
    struct i2c_client *client = ts->client;
    unsigned long flags;
    unsigned ch = port->line & 0x01;

    //    printk("%s\n", __func__);

    BUG_ON(!chan);
    //    BUG_ON(!ts);

    /* Free the interrupt */
    free_irq(client->irq, chan);

    if (chan->workqueue) {
        /* Flush and destroy work queue */
        flush_workqueue(chan->workqueue);
        destroy_workqueue(chan->workqueue);
        chan->workqueue = NULL;
    }

    /* Suspend HW */
    spin_lock_irqsave(&chan->uart.lock, flags);
    chan->ier = UART_IERX_SLEEP;
    spin_unlock_irqrestore(&chan->uart.lock, flags);
    sc16is7x2_write(client, UART_IER, ch, chan->ier);
}

static void
sc16is7x2_set_termios(struct uart_port *port, struct ktermios *termios,
                      struct ktermios *old)
{
    struct sc16is7x2_channel *chan = to_sc16is7x2_channel(port);
    struct sc16is7x2_chip *ts = chan->chip;
    //    struct i2c_client *client = ts->client;
    unsigned long flags;
    unsigned int baud;
    u8 lcr, fcr = 0;

    /* Ask the core to calculate the divisor for us. */
    baud = uart_get_baud_rate(port, termios, old,
                              //                              19200,
                              port->uartclk / 16 / 0xffff,
                              port->uartclk / 16);
    chan->quot = uart_get_divisor(port, baud);
    chan->handle_baud = true;

    //    printk("%s (baud %u) chan->quot %d \n", __func__, baud, chan->quot);

    /* set word length */
    switch (termios->c_cflag & CSIZE) {
    case CS5:
        lcr = UART_LCR_WLEN5;

        printk("%s port->uartclk %d (baud %u) chan->quot %d UART_LCR_WLEN5 \n", __func__, port->uartclk, baud, chan->quot);
        break;
    case CS6:
        lcr = UART_LCR_WLEN6;

        printk("%s port->uartclk %d (baud %u) chan->quot %d UART_LCR_WLEN6 \n", __func__, port->uartclk, baud, chan->quot);
        break;
    case CS7:
        lcr = UART_LCR_WLEN7;

        printk("%s port->uartclk %d (baud %u) chan->quot %d UART_LCR_WLEN7 \n", __func__, port->uartclk, baud, chan->quot);
        break;
    default:
    case CS8:
        lcr = UART_LCR_WLEN8;

        printk("%s port->uartclk %d (baud %u) chan->quot %d UART_LCR_WLEN8 \n", __func__, port->uartclk, baud, chan->quot);
        break;
    }

    if (termios->c_cflag & CSTOPB)
        lcr |= UART_LCR_STOP;
    if (termios->c_cflag & PARENB)
        lcr |= UART_LCR_PARITY;
    if (!(termios->c_cflag & PARODD))
        lcr |= UART_LCR_EPAR;
#ifdef CMSPAR
    if (termios->c_cflag & CMSPAR)
        lcr |= UART_LCR_SPAR;
#endif

    fcr = UART_FCR_ENABLE_FIFO;
    /* configure the fifo */
    if (baud < 2400)
        fcr |= UART_FCR_TRIGGER_1;
    else
    {
        //        printk("%s (baud %u) UART_FCR_R_TRIG_01 | UART_FCR_T_TRIG_10 \n", __func__, baud);

        fcr |= UART_FCR_R_TRIG_01 | UART_FCR_T_TRIG_10;
    }

    //    chan->efr = UART_EFR_ECB;
    //    chan->mcr |= UART_MCR_RTS;
    if (termios->c_cflag & CRTSCTS)
    {
        //        printk("%s (baud %u) UART_EFR_CTS | UART_EFR_RTS \n", __func__, baud);

        //        chan->efr |= UART_EFR_CTS | UART_EFR_RTS;
    }

    /*
     * Ok, we're now changing the port state.  Do it with
     * interrupts disabled.
     */
    spin_lock_irqsave(&chan->uart.lock, flags);

    /* we are sending char from a workqueue so enable */
    chan->uart.state->port.tty->low_latency = 1;

    /* Update the per-port timeout. */
    uart_update_timeout(port, termios->c_cflag, baud);

    chan->uart.read_status_mask = UART_LSR_OE | UART_LSR_THRE | UART_LSR_DR;
    if (termios->c_iflag & INPCK)
    {
        //        printk("%s (baud %u) INPCK \n", __func__, baud);
        chan->uart.read_status_mask |= UART_LSR_FE | UART_LSR_PE;
    }
    if (termios->c_iflag & (BRKINT | PARMRK))
    {
        //        printk("%s (baud %u) (BRKINT | PARMRK) \n", __func__, baud);
        chan->uart.read_status_mask |= UART_LSR_BI;
    }

    /* Characters to ignore */
    chan->uart.ignore_status_mask = 0;
    if (termios->c_iflag & IGNPAR)
    {
        //        printk("%s (baud %u) (UART_LSR_PE | UART_LSR_FE) \n", __func__, baud);
        chan->uart.ignore_status_mask |= UART_LSR_PE | UART_LSR_FE;
    }
    if (termios->c_iflag & IGNBRK) {
        //        printk("%s (baud %u) (UART_LSR_BI) \n", __func__, baud);
        chan->uart.ignore_status_mask |= UART_LSR_BI;
        /*
         * If we're ignoring parity and break indicators,
         * ignore overruns too (for real raw support).
         */
        if (termios->c_iflag & IGNPAR)
        {
            //            printk("%s (baud %u) (UART_LSR_OE) \n", __func__, baud);
            chan->uart.ignore_status_mask |= UART_LSR_OE;
        }
    }

    /* ignore all characters if CREAD is not set */
    if ((termios->c_cflag & CREAD) == 0)
    {
        //        printk("%s (baud %u) (UART_LSR_DR) \n", __func__, baud);
        chan->uart.ignore_status_mask |= UART_LSR_DR;
    }

    /* CTS flow control flag and modem status interrupts */
    chan->ier &= ~UART_IER_MSI;
    if (UART_ENABLE_MS(&chan->uart, termios->c_cflag))
    {
        //        printk("%s (baud %u) (UART_LSR_DR) \n", __func__, baud);
        chan->ier |= UART_IER_MSI;
    }

    chan->lcr = lcr;	/* Save LCR */
    chan->fcr = fcr;	/* Save FCR */
    chan->handle_regs = true;
    //    printk("%s lcr=%x, fcr=%x \n", __func__, lcr, fcr);

    spin_unlock_irqrestore(&chan->uart.lock, flags);

    /* Trigger work thread for doing the actual configuration change */
    sc16is7x2_dowork(chan);
}

static const char * sc16is7x2_type(struct uart_port *port)
{
    //    printk("%s\n", __func__);
    return TYPE_NAME;
}

static void sc16is7x2_release_port(struct uart_port *port)
{
    //    printk("%s\n", __func__);
}

static int sc16is7x2_request_port(struct uart_port *port)
{
    //    printk("%s\n", __func__);
    return 0;
}

static void sc16is7x2_config_port(struct uart_port *port, int flags)
{
    struct sc16is7x2_channel *chan = to_sc16is7x2_channel(port);
    struct sc16is7x2_chip *ts = chan->chip;

    //    printk("%s\n", __func__);
    if (flags & UART_CONFIG_TYPE)
        chan->uart.type = PORT_SC16IS7X2;
}

static int
sc16is7x2_verify_port(struct uart_port *port, struct serial_struct *ser)
{
    if (ser->type == PORT_UNKNOWN || ser->type == PORT_SC16IS7X2)
        return 0;

    return -EINVAL;
}

/* ******************************** GPIO ********************************* */
#ifdef CONFIG_GPIOLIB
static int sc16is7x2_gpio_request(struct gpio_chip *gpio, unsigned offset)
{
    struct sc16is7x2_chip *ts =
            container_of(gpio, struct sc16is7x2_chip, gpio);
    int control = (offset < 4) ? IOC_GPIO30 : IOC_GPIO74;
    int ret = 0;

    BUG_ON(offset > 8);
    printk(&ts->client->dev, "%s: offset = %d\n", __func__, offset);

    mutex_lock(&ts->io_lock);

    /* GPIO 0:3 and 4:7 can only be controlled as block */
    ts->io_gpio |= BIT(offset);
    if (ts->io_control & control) {
        printk(&ts->client->dev, "activate GPIOs %s\n",
               (offset < 4) ? "0-3" : "4-7");
        ts->io_control &= ~control;
        ret = sc16is7x2_write(ts->client, REG_IOC, 0, ts->io_control);
    }

    mutex_unlock(&ts->io_lock);

    return ret;
}

static void sc16is7x2_gpio_free(struct gpio_chip *gpio, unsigned offset)
{
    struct sc16is7x2_chip *ts =
            container_of(gpio, struct sc16is7x2_chip, gpio);
    int control = (offset < 4) ? IOC_GPIO30 : IOC_GPIO74;
    int mask = (offset < 4) ? 0x0f : 0xf0;

    BUG_ON(offset > 8);

    mutex_lock(&ts->io_lock);

    /* GPIO 0:3 and 4:7 can only be controlled as block */
    ts->io_gpio &= ~BIT(offset);
    printk(&ts->client->dev, "%s: io_gpio = 0x%02X\n", __func__, ts->io_gpio);
    if (!(ts->io_control & control) && !(ts->io_gpio & mask)) {
        printk(&ts->client->dev, "deactivate GPIOs %s\n",
               (offset < 4) ? "0-3" : "4-7");
        ts->io_control |= control;
        sc16is7x2_write(ts->client, REG_IOC, 0, ts->io_control);
    }

    mutex_unlock(&ts->io_lock);
}

static int sc16is7x2_direction_input(struct gpio_chip *gpio, unsigned offset)
{
    struct sc16is7x2_chip *ts =
            container_of(gpio, struct sc16is7x2_chip, gpio);
    unsigned io_dir;

    BUG_ON(offset > 8);

    mutex_lock(&ts->io_lock);

    ts->io_dir &= ~BIT(offset);
    io_dir = ts->io_dir;

    mutex_unlock(&ts->io_lock);

    return sc16is7x2_write(ts->client, REG_IOD, 0, io_dir);
}

static int sc16is7x2_direction_output(struct gpio_chip *gpio, unsigned offset,
                                      int value)
{
    struct sc16is7x2_chip *ts =
            container_of(gpio, struct sc16is7x2_chip, gpio);

    BUG_ON(offset > 8);

    mutex_lock(&ts->io_lock);

    if (value)
        ts->io_state |= BIT(offset);
    else
        ts->io_state &= ~BIT(offset);

    ts->io_dir |= BIT(offset);

    mutex_unlock(&ts->io_lock);

    sc16is7x2_write(ts->client, REG_IOS, 0, ts->io_state);
    return sc16is7x2_write(ts->client, REG_IOD, 0, ts->io_dir);
}

static int sc16is7x2_get(struct gpio_chip *gpio, unsigned offset)
{
    struct sc16is7x2_chip *ts =
            container_of(gpio, struct sc16is7x2_chip, gpio);
    int level = -EINVAL;

    BUG_ON(offset > 8);

    mutex_lock(&ts->io_lock);

    if (ts->io_dir & BIT(offset)) {
        /* Output: return cached level */
        level = (ts->io_state >> offset) & 0x01;
    } else {
        /* Input: read out all pins */
        level = sc16is7x2_read(ts->client, REG_IOS, 0);
        if (level >= 0) {
            ts->io_state = level;
            level = (ts->io_state >> offset) & 0x01;
        }
    }

    mutex_unlock(&ts->io_lock);

    return level;
}

static void sc16is7x2_set(struct gpio_chip *gpio, unsigned offset, int value)
{
    struct sc16is7x2_chip *ts =
            container_of(gpio, struct sc16is7x2_chip, gpio);
    unsigned io_state;

    BUG_ON(offset > 8);

    mutex_lock(&ts->io_lock);

    if (value)
        ts->io_state |= BIT(offset);
    else
        ts->io_state &= ~BIT(offset);
    io_state = ts->io_state;

    mutex_unlock(&ts->io_lock);

    sc16is7x2_write(ts->client, REG_IOS, 0, io_state);
}
#endif /* CONFIG_GPIOLIB */

static int sc16is7x2_register_gpio(struct sc16is7x2_chip *ts)
{
#ifdef CONFIG_GPIOLIB
    ts->gpio.label = (ts->gpio_label!=NULL) ? ts->gpio_label : DRIVER_NAME;
    ts->gpio.request	= sc16is7x2_gpio_request;
    ts->gpio.free	= sc16is7x2_gpio_free;
    ts->gpio.get	= sc16is7x2_get;
    ts->gpio.set	= sc16is7x2_set;
    ts->gpio.direction_input = sc16is7x2_direction_input;
    ts->gpio.direction_output = sc16is7x2_direction_output;

    ts->gpio.base = ts->gpio_base;
    //    ts->gpio.names = (ts->gpio_names!=NULL) ? ts->gpio_names : DRIVER_NAME;
    ts->gpio.ngpio = SC16IS7X2_NR_GPIOS;
    ts->gpio.can_sleep = 1;
    ts->gpio.dev = &ts->client->dev;
    ts->gpio.owner = THIS_MODULE;

    mutex_init(&ts->io_lock);

    /* disable all GPIOs, enable on request */
    ts->io_gpio = 0;
    ts->io_control = 0; //IOC_GPIO30 | IOC_GPIO74;
    ts->io_state = 0;
    ts->io_dir = 0;

    sc16is7x2_write(ts->client, REG_IOI, 0, 0); /* no support for irqs yet */
    sc16is7x2_write(ts->client, REG_IOC, 0, ts->io_control);
    sc16is7x2_write(ts->client, REG_IOS, 0, ts->io_state);
    sc16is7x2_write(ts->client, REG_IOD, 0, ts->io_dir);

    int ret = gpiochip_add(&ts->gpio);
    if (ret < 0)
    {
        printk("%s could not register gpiochip \n", __func__);
    } else
    {
        printk("%s registered gpiochip \n", __func__);
    }

//    gpio_direction_output(214, 0);
//    gpio_set_value(214, 0);

    return ret;
#else
    return 0;
#endif
}

static struct uart_ops sc16is7x2_uart_ops = {
    .tx_empty	= sc16is7x2_tx_empty,
    .set_mctrl	= sc16is7x2_set_mctrl,
    .get_mctrl	= sc16is7x2_get_mctrl,
    .stop_tx        = sc16is7x2_stop_tx,
    .start_tx	= sc16is7x2_start_tx,
    .stop_rx	= sc16is7x2_stop_rx,
    .enable_ms      = sc16is7x2_enable_ms,
    .break_ctl      = sc16is7x2_break_ctl,
    .startup	= sc16is7x2_startup,
    .shutdown	= sc16is7x2_shutdown,
    .set_termios	= sc16is7x2_set_termios,
    .type		= sc16is7x2_type,
    .release_port   = sc16is7x2_release_port,
    .request_port   = sc16is7x2_request_port,
    .config_port	= sc16is7x2_config_port,
    .verify_port	= sc16is7x2_verify_port,
};

static int sc16is7x2_register_uart_port(struct sc16is7x2_chip *data, unsigned ch)
{
    //    printk("%s ch %d \n", __func__, ch);f
    struct sc16is7x2_channel *chan = &(data->channel[ch]);
    struct uart_port *uart = &chan->uart;

    /* Disable irqs and go to sleep */
    //    sc16is7x2_write(data->client, UART_IER, ch, UART_IERX_SLEEP);

    chan->chip = data;

    //    printk("%s irq: %d uartclk: %lu \n", __func__, data->client->irq, data->uartclk);
    //    printk(KERN_ERR "irq: %d\n" , data->client->irq);
    uart->irq = data->client->irq;
    uart->uartclk = data->uartclk;
    uart->fifosize = FIFO_SIZE;
    uart->ops = &sc16is7x2_uart_ops;
    uart->flags = UPF_SKIP_TEST | UPF_BOOT_AUTOCONF;
    uart->line = data->uart_base + ch;
    uart->type = PORT_SC16IS7X2;
    uart->dev = &data->client->dev;

    //    tty_buffer_init

    return uart_add_one_port(&sc16is7x2_uart_driver, uart);
}

/* TODO */
static int __devinit sc16is7x2_probe(struct i2c_client *client,
                                     const struct i2c_device_id *id)
{
    //    printk("sc16is7x2_probe \n");

    int ret = 0;

    if (!i2c_check_functionality(client->adapter,
                                 I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
                                 I2C_FUNC_SMBUS_I2C_BLOCK)) {
        printk(KERN_ERR "%s: needed i2c functionality is not supported\n", __func__);
        return -ENODEV;
    }

    struct sc16is7x2_platform_data *pdata = client->dev.platform_data;
    if (pdata == NULL) {
        printk("sc16is7x2_probe no platform data\n");
        //		return -EINVAL;
    }

    struct sc16is7x2_chip *data;
    /* Only even uart base numbers are supported */
    data = kzalloc(sizeof(struct sc16is7x2_chip), GFP_KERNEL);
    if (!data) {
        printk(KERN_ERR "%s: no memory\n", __func__);
        ret = -ENOMEM;
        //        goto exit;
    }

    /* default settings at probe */
    data->client = client;
    i2c_set_clientdata(client, data);

    // pdata = client->data.platform_data;

    /* TODO: do something */
#if 0
    //    ret = sc16is7x2_probe_dt(ts, client);
    //    if (ret > 0)
    //        sc16is7x2_probe_pdata(ts, client);
    //    else if (ret < 0)
    //        return ret;
#endif

    data->uartclk = pdata->uartclk;
    data->uart_base = pdata->uart_base;
    data->gpio_base = pdata->gpio_base;

    data->gpio_label = pdata->label;
    data->gpio_names = pdata->names;

    if (!data->gpio_base) {
        printk("incorrect uartclk \n");
    }
    if (!data->gpio_base) {
        printk("incorrect gpio_base \n");
        //        return -EINVAL;
    }

    if (data->uart_base & 1) {
        printk("incorrect uart_base \n");
        //        return -EINVAL;
    }

    /* Reset the chip */
    sc16is7x2_write(data->client, REG_IOC, 0, IOC_SRESET);

    ret = sc16is7x2_register_uart_port(data, 0);
    if (ret)
    {
        printk("exit_uart0 \n");
        goto exit_uart0;
    }

    ret = sc16is7x2_register_uart_port(data, 1);
    if (ret)
    {
        printk("exit_uart1 \n");
        goto exit_uart1;
    }

    ret = sc16is7x2_register_gpio(data);
    if (ret)
    {
        printk("exit_destroy \n");
        //        goto exit_destroy;
    }

    printk(DRIVER_NAME " (irq %d), 2 UARTs, 8 GPIOs\n"
           "    ttyNSC%d, ttyNSC%d, gpiochip%d\n",
           data->client->irq,
           data->uart_base, data->uart_base + 1,
           data->gpio_base);

    sc16is7x2_startup(&data->channel[0].uart);

    printk(KERN_INFO "TLR: %08x\n", sc16is7x2_read(client, 7, 0));
    printk(KERN_INFO "LCR: %08x\n", sc16is7x2_read(client, UART_LCR, 0));
    printk(KERN_INFO "MCR: %08x\n", sc16is7x2_read(client, UART_MCR, 0));
    printk(KERN_INFO "EFR: %08x\n", sc16is7x2_read(client, UART_EFR, 0));
    printk(KERN_INFO "LSR: %08x\n", sc16is7x2_read(client, UART_LSR, 0));
    printk(KERN_INFO "IER: %08x\n", sc16is7x2_read(client, UART_IER, 0));
    printk(KERN_INFO "EFCR: %08x\n", sc16is7x2_read(client, 0xF, 0));

    return 0;

exit:
    return ret;

exit_uart1:
    uart_remove_one_port(&sc16is7x2_uart_driver, &data->channel[1].uart);

exit_uart0:
    uart_remove_one_port(&sc16is7x2_uart_driver, &data->channel[0].uart);

exit_destroy:
    //dev_set_drvdata(&client->dev, NULL);
    kfree(data);


    printk(KERN_INFO "sc16is7x2_probe_error");
    return ret;
}

static int __devexit sc16is7x2_remove(struct i2c_client *client)
{
    struct sc16is7x2_chip *data = i2c_get_clientdata(client);
    /*  sleep mode to save power */

    int ret;

    if (data == NULL)
        return -ENODEV;

    ret = uart_remove_one_port(&sc16is7x2_uart_driver, &data->channel[0].uart);
    if (ret)
        return ret;

    ret = uart_remove_one_port(&sc16is7x2_uart_driver, &data->channel[1].uart);
    if (ret)
        return ret;

#if 0
#ifdef CONFIG_GPIOLIB
    ret = gpiochip_remove(&data->gpio);
    if (ret)
        return ret;
#endif
#endif

    kfree(data);

    return 0;
}

#ifdef CONFIG_PM
static int sc16is7x2_suspend(struct i2c_client *client, pm_message_t msg)
{
    struct sc16is7x2_chip *dev = i2c_get_clientdata(client);

    return 0;
}

static int sc16is7x2_resume(struct i2c_client *client)
{
    struct sc16is7x2_chip *dev = i2c_get_clientdata(client);

    return 0;
}
#else
#define sc16is7x2_suspend NULL
#define sc16is7x2_resume  NULL
#endif

static const struct i2c_device_id sc16is7x2_ids[] = {
    { DRIVER_NAME, 0 },
    { }
};

static struct i2c_driver sc16is7x2_i2c_driver = {
    .probe    = sc16is7x2_probe,
    .remove   = __devexit_p(sc16is7x2_remove),
    .id_table = sc16is7x2_ids,
    .suspend  = sc16is7x2_suspend,
    .resume   = sc16is7x2_resume,
    .driver   = {
        .name = DRIVER_NAME,
    },
};

static int __init sc16is7x2_init(void)
{
    int ret = uart_register_driver(&sc16is7x2_uart_driver);
    if (ret)
        return ret;
    return i2c_add_driver(&sc16is7x2_i2c_driver);
}

static void __exit sc16is7x2_exit(void)
{
    i2c_del_driver(&sc16is7x2_i2c_driver);
    uart_unregister_driver(&sc16is7x2_uart_driver);
}

module_init(sc16is7x2_init);
//subsys_initcall(sc16is7x2_init);
module_exit(sc16is7x2_exit);

MODULE_AUTHOR("artthevan");
MODULE_DESCRIPTION("SC16IS7x2 I2C based UART chip");
MODULE_LICENSE("GPL");
//MODULE_ALIAS("I2C:" DRIVER_NAME);
