/*
 *  Copyright (c) 2018 Max Thomas
 */

/*
 * Nintendo Joy-Con serial gamepad driver for Linux
 */

/*
 * This program is free warftware; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *  Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <vojtech@ucw.cz>, or by paper mail:
 * Vojtech Pavlik, Simunkova 1594, Prague 8, 182 00 Czech Republic
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/of.h>
#include <linux/tty.h>
#include <linux/serdev.h>
#include <linux/platform_device.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>

#define DRIVER_DESC	"Nintendo Joy-Con serial gamepad driver"

MODULE_AUTHOR("Max Thomas <mtinc2@gmail.com>");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");


/*
 * Constants.
 */
static const u8 magic_start[0x4] = {0xA1, 0xA2, 0xA3, 0xA4};
static const u8 handshake_start[0xC] = {0x19, 0x01, 0x03, 0x07, 0x00, 0xA5, 0x02, 0x01, 0x7E, 0x00, 0x00, 0x00};
static const u8 get_mac[0xC] = {0x19, 0x01, 0x03, 0x07, 0x00, 0x91, 0x01, 0x00, 0x00, 0x00, 0x00, 0x24};
static const u8 switch_baud[0x14] = {0x19, 0x01, 0x03, 0x0F, 0x00, 0x91, 0x20, 0x08, 0x00, 0x00, 0xBD, 0xB1, 0xC0, 0xC6, 0x2D, 0x00, 0x00, 0x00, 0x00, 0x00};
static const u8 controller_status[0xD] = {0x19, 0x01, 0x03, 0x08, 0x00, 0x92, 0x00, 0x01, 0x00, 0x00, 0x69, 0x2D, 0x1F};
static const u8 unk_1[0xC] = {0x19, 0x01, 0x03, 0x07, 0x00, 0x91, 0x11, 0x00, 0x00, 0x00, 0x00, 0x0E};
static const u8 unk_2[0xC] = {0x19, 0x01, 0x03, 0x07, 0x00, 0x91, 0x10, 0x00, 0x00, 0x00, 0x00, 0x3D};
static const u8 unk_3[0x10] = {0x19, 0x01, 0x03, 0x0B, 0x00, 0x91, 0x12, 0x04, 0x00, 0x00, 0x12, 0xA6, 0x0F, 0x00, 0x00, 0x00};
 
#define JOYCON_MAX_LENGTH 256
#define JOYCON_COMMAND_EXTSEND   (0x91)
#define JOYCON_COMMAND_EXTRET    (0x92)
#define JOYCON_COMMAND_INITRET   (0x94)
#define JOYCON_COMMAND_HANDSHAKE (0xA5)

#define JOYCON_INIT_MAC      (0x1)
#define JOYCON_INIT_BAUDRATE (0x20)
#define JOYCON_INIT_UNK1     (0x11)
#define JOYCON_INIT_UNK2     (0x10)
#define JOYCON_INIT_UNK3     (0x12)

#define JOYCON_EXT_INPUT (0x30)

#define JOYCON_BUTTON_Y          BIT(0)
#define JOYCON_BUTTON_X          BIT(1)
#define JOYCON_BUTTON_B          BIT(2)
#define JOYCON_BUTTON_A          BIT(3)
#define JOYCON_BUTTON_RSL        BIT(4)
#define JOYCON_BUTTON_RSR        BIT(5)
#define JOYCON_BUTTON_R          BIT(6)
#define JOYCON_BUTTON_ZR         BIT(7)
#define JOYCON_BUTTON_MINUS      BIT(8)
#define JOYCON_BUTTON_PLUS       BIT(9)
#define JOYCON_BUTTON_STICKR     BIT(10)
#define JOYCON_BUTTON_STICKL     BIT(11)
#define JOYCON_BUTTON_HOME       BIT(12)
#define JOYCON_BUTTON_SCREENSHOT BIT(13)
#define JOYCON_BUTTON_DOWN       BIT(16)
#define JOYCON_BUTTON_UP         BIT(17)
#define JOYCON_BUTTON_RIGHT      BIT(18)
#define JOYCON_BUTTON_LEFT       BIT(19)
#define JOYCON_BUTTON_LSL        BIT(20)
#define JOYCON_BUTTON_LSR        BIT(21)
#define JOYCON_BUTTON_L          BIT(22)
#define JOYCON_BUTTON_ZL         BIT(23)

#define JOYCON_BUTTONS_LEFT  (0xFFE900)
#define JOYCON_BUTTONS_RIGHT (0x76FF)
#define JOYCON_BUTTONS_ALL (JOYCON_BUTTONS_LEFT | JOYCON_BUTTONS_RIGHT)

#define JOYCON_BUTTONSET_LEFT  BIT(0)
#define JOYCON_BUTTONSET_RIGHT BIT(1)

typedef struct joycon_uart_initial
{
	u8 magic[3];
	u8 total_size;
	u8 pad;
} joycon_uart_initial;

typedef struct joycon_uart_header
{
	joycon_uart_initial initial;
	u8 command;
	u8 data[5];
	u8 crc;
} joycon_uart_header;

typedef struct joycon_subcmd_packet
{
	joycon_uart_header pre;
	u8 command;
	u8 data[];
} joycon_subcmd_packet;

/* Joy-Con button mappings */
static const signed short joycon_common_btn[] = {
	BTN_Y, BTN_X, BTN_B, BTN_A,
	BTN_0, BTN_1, BTN_TR, BTN_TR2,
	BTN_SELECT, BTN_START, 
	BTN_THUMBR, BTN_THUMBL,
	BTN_MODE, BTN_Z, 
	BTN_4, BTN_5, /* These two buttons do not exist on retail controllers. */
	BTN_DPAD_DOWN, BTN_DPAD_UP, BTN_DPAD_RIGHT, BTN_DPAD_LEFT, 
	BTN_2, BTN_3, BTN_TL, BTN_TL2,
	-1 /* terminating entry */
};

/* Driver data */
struct joycon_device {
	struct list_head	list;

	struct device		*dev;

	const char		*name;
	int			irq;
};

/*
 * Per-Joy-Con data.
 */

struct joycon {
	struct serdev_device *serdev;
	struct input_dev *input_dev;
	struct device *dev;
	
	bool handshaken;
	bool initialized;
	
	int timeout_samples;
	int num_samples;
	
	u8 button_set;
	
	int idx;
	u32 buttons;
	u8 mac[6];
};

/*
 * Globals
 */
static DEFINE_MUTEX(joycon_device_lock);
static DEFINE_MUTEX(joycon_input_lock);
static LIST_HEAD(joycon_device_list);

static struct serdev_device_ops joycon_ops;
static struct input_dev *input_dev;
static u32 buttons;
static int stick_lx, stick_ly, stick_rx, stick_ry;

static struct workqueue_struct *joycon_wq;
struct work_timeout_poll {
	struct delayed_work work;
	struct joycon* joycon;
};

struct work_input_poll {
	struct delayed_work work;
	struct joycon* joycon;
};

struct work_sync_poll {
	struct delayed_work work;
};
 
static void timeout_handler(struct work_struct *work)
{
	struct work_timeout_poll * data = (struct work_timeout_poll *)work;
	struct joycon *joycon = data->joycon;
	struct serdev_device *serdev = joycon->serdev;
	int err;
	
	printk("timeout! last %x current %x diff %x\n", joycon->timeout_samples, joycon->num_samples, joycon->num_samples - joycon->timeout_samples);
	
	// We haven't gotten any samples within 200ms, we've probably been disconnected.
	if ((joycon->num_samples - joycon->timeout_samples == 0 && joycon->num_samples) || !joycon->initialized)
	{
		joycon->handshaken = false;
		joycon->initialized = false;
		while (!joycon->handshaken)
		{
			err = serdev_device_write(serdev, magic_start, sizeof(magic_start), msecs_to_jiffies(200));
			if (err) goto fail;

			err = serdev_device_write(serdev, handshake_start, sizeof(handshake_start), msecs_to_jiffies(200));
			if (err) goto fail;

			usleep_range(180000, 200000);
			printk("Sent handshake\n");
		}
		
		// Get MAC
		serdev_device_write(serdev, get_mac, sizeof(get_mac), msecs_to_jiffies(200));
		
		//serdev_device_write(serdev, switch_baud, sizeof(switch_baud), msecs_to_jiffies(200));

		serdev_device_write(serdev, unk_1, sizeof(unk_1), msecs_to_jiffies(200));
		serdev_device_write(serdev, unk_2, sizeof(unk_2), msecs_to_jiffies(200));
		serdev_device_write(serdev, unk_3, sizeof(unk_3), msecs_to_jiffies(200));
		
		joycon->num_samples = 0;
		joycon->initialized = true;
	}
	
	//kfree(data);

fail:
	joycon->timeout_samples = joycon->num_samples;
	INIT_DELAYED_WORK(&data->work,timeout_handler);
	queue_delayed_work(joycon_wq, &data->work, msecs_to_jiffies(200));
}

static void input_handler(struct work_struct *work)
{
	struct work_input_poll * data = (struct work_input_poll *)work;
	struct joycon *joycon = data->joycon;
	
	if (joycon->initialized)
	{
		serdev_device_write(joycon->serdev, controller_status, sizeof(controller_status), msecs_to_jiffies(200));
	}
	
	INIT_DELAYED_WORK(&data->work, input_handler);
	queue_delayed_work(joycon_wq, &data->work, msecs_to_jiffies(16));
}

static void sync_handler(struct work_struct *work)
{
	struct work_sync_poll * data = (struct work_sync_poll *)work;
	int i;
	
	mutex_lock(&joycon_input_lock);
	for (i = 0; joycon_common_btn[i] >= 0; i++)
	{
		input_report_key(input_dev, joycon_common_btn[i], buttons & BIT(i));
	}
	
	input_report_abs(input_dev, ABS_X, stick_lx);
	input_report_abs(input_dev, ABS_Y, stick_ly);
	input_report_abs(input_dev, ABS_RX, stick_rx);
	input_report_abs(input_dev, ABS_RY, stick_ry);
	input_sync(input_dev);
	mutex_unlock(&joycon_input_lock);
	
	INIT_DELAYED_WORK(&data->work, sync_handler);
	queue_delayed_work(joycon_wq, &data->work, msecs_to_jiffies(10));
}

static void joycon_extret_parse(struct serdev_device *serdev, u8* packet, u32 size)
{
	struct joycon *joycon = serdev_device_get_drvdata(serdev);
	struct input_dev *input_dev = joycon->input_dev;

	switch (packet[0])
	{
		case JOYCON_EXT_INPUT:
			joycon->buttons = packet[3] | packet[4] << 8 | packet[5] << 16;
			
			mutex_lock(&joycon_input_lock);
			if (joycon->button_set & JOYCON_BUTTONSET_LEFT)
			{
				buttons &= ~JOYCON_BUTTONS_LEFT;;
				buttons |= (joycon->buttons & JOYCON_BUTTONS_LEFT);
			}
			if (joycon->button_set & JOYCON_BUTTONSET_RIGHT)
			{
				buttons &= ~JOYCON_BUTTONS_RIGHT;
				buttons |= (joycon->buttons & JOYCON_BUTTONS_RIGHT);
			}

			if (joycon->button_set & JOYCON_BUTTONSET_LEFT)
			{
				stick_lx = ((packet[7] & 0x0F) << 4) | ((packet[6] & 0xF0) >> 4);
				stick_ly = 256 - packet[8];
			}
			
			if (joycon->button_set & JOYCON_BUTTONSET_RIGHT)
			{
				stick_rx = ((packet[10] & 0x0F) << 4) | ((packet[9] & 0xF0) >> 4);
				stick_ry = 256 - packet[11];
			}
			mutex_unlock(&joycon_input_lock);
			

			joycon->num_samples++;
			break;
		default:
			printk("Unknown extret %x\n", packet[0]);
			break;
	}
}

static void joycon_initret_parse(struct serdev_device *serdev, u8* packet, u32 size)
{
	struct joycon *joycon = serdev_device_get_drvdata(serdev);
	int i, j;

	switch (packet[0])
	{
		case JOYCON_INIT_MAC:
			
			for (i = 0xC; i >= 0x6; i--)
				joycon->mac[j++] = packet[i];
			printk("Joy-Con with MAC %02X::%02X::%02X::%02X::%02X::%02X\n", joycon->mac[0], joycon->mac[1], joycon->mac[2], joycon->mac[3], joycon->mac[4], joycon->mac[5]);
			
			// TODO: Better detection
			if (joycon->mac[0] != 0x7C)
				joycon->button_set = JOYCON_BUTTONSET_LEFT;
			else
				joycon->button_set = JOYCON_BUTTONSET_RIGHT;
			break;
		case JOYCON_INIT_UNK1:
		case JOYCON_INIT_UNK2:
		case JOYCON_INIT_UNK3:
			break;
		case JOYCON_INIT_BAUDRATE:
			serdev_device_set_baudrate(serdev, 3125000);
			printk("High-speed baudrate shifted...\n");
			break;
		default:
			printk("Unknown initret %x\n", packet[0]);
			break;
	}
}

static void joycon_packet_parse(struct serdev_device *serdev, u8* packet, size_t size)
{
	struct joycon *joycon = serdev_device_get_drvdata(serdev);
	joycon_uart_header* header = (joycon_uart_header*) packet;

	switch (header->command)
	{
		case JOYCON_COMMAND_EXTRET:
			joycon_extret_parse(serdev, packet + sizeof(joycon_uart_header), (header->data[0] << 8) | header->data[1]);
			break;
		case JOYCON_COMMAND_INITRET:
			joycon_initret_parse(serdev, packet + sizeof(joycon_uart_initial) + 1, size - sizeof(joycon_uart_initial) - 1);
			break;
		case JOYCON_COMMAND_HANDSHAKE:
			printk("Got handshake response\n");
			joycon->handshaken = true;
			break;
		default:
			printk("Unknown command %x\n", header->command);
			break;
	}
}

static int joycon_serdev_receive_buf(struct serdev_device *serdev, const unsigned char *buf, size_t len)
{
	struct joycon *joycon = serdev_device_get_drvdata(serdev);
	int i;
	
	//printk("Got data from Joy-Con size %zu\n", len);

	if (!joycon || serdev != joycon->serdev) {
		WARN_ON(1);
		return 0;
	}
	
	joycon_packet_parse(serdev, buf, len);

	return len;
}

static void joycon_serdev_write_wakeup(struct serdev_device *serdev)
{
	//printk("Wakeup\n");
}

static int joycon_serdev_probe(struct serdev_device *serdev)
{
	struct joycon *joycon;
	
	struct work_timeout_poll *work_timeout;
	struct work_input_poll *work_input;
	struct work_sync_poll *work_sync;
	int i;
	int err = -ENOMEM;

	printk("Joy-Con probe!\n");

	joycon = devm_kzalloc(&serdev->dev, sizeof(*joycon), GFP_KERNEL);
	if (!joycon)
		goto fail2;

	if (input_dev) goto input_exists;

	// TODO: For USB/BT this should be an array w/ Joy-Con 
	//       being matched to input devices by halves
	input_dev = input_allocate_device();
	if (!input_dev) goto fail2;
	
	input_dev->name = "Joy-Con Rails";
	input_dev->id.bustype = BUS_VIRTUAL;
	input_dev->id.vendor = 0x057E;
	input_dev->id.product = 0x2008;
	input_dev->id.version = 0x0100;
	input_dev->dev.parent = &serdev->dev;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	
	/* set up standard buttons */
	for (i = 0; joycon_common_btn[i] >= 0; i++)
		__set_bit(joycon_common_btn[i], input_dev->keybit);

	input_set_abs_params(input_dev, ABS_X, 32, 255 - 32, 0, 4);
	input_set_abs_params(input_dev, ABS_Y, 32, 255 - 32, 0, 4);
	input_set_abs_params(input_dev, ABS_RX, 32, 255 - 32, 0, 4);
	input_set_abs_params(input_dev, ABS_RY, 32, 255 - 32, 0, 4);
	
	err = input_register_device(input_dev);
	if (err) goto fail1;
	
	// Start input sync polling
	work_sync = kmalloc(sizeof(struct work_sync_poll), GFP_KERNEL);
	INIT_DELAYED_WORK(&work_sync->work, sync_handler);
	queue_delayed_work(joycon_wq, &work_sync->work, msecs_to_jiffies(10));

input_exists:
	joycon->serdev = serdev;
	joycon->dev = &serdev->dev;
	joycon->input_dev = input_dev;

	serdev_device_set_drvdata(serdev, joycon);
	
	serdev_device_open(serdev);
	serdev_device_set_flow_control(serdev, true);
	serdev_device_set_baudrate(serdev, 1000000);
	printk("Brought baudrate up\n");
	
	serdev_device_set_client_ops(serdev, &joycon_ops);
	
	// Set up work polling
	work_timeout = kmalloc(sizeof(struct work_timeout_poll), GFP_KERNEL);
	INIT_DELAYED_WORK(&work_timeout->work, timeout_handler);
	work_timeout->joycon = joycon;
	queue_delayed_work(joycon_wq, &work_timeout->work, msecs_to_jiffies(200));
	
	work_input = kmalloc(sizeof(struct work_input_poll), GFP_KERNEL);
	work_input->joycon = joycon;
	INIT_DELAYED_WORK(&work_input->work, input_handler);
	queue_delayed_work(joycon_wq, &work_input->work, msecs_to_jiffies(200));

	return 0;

 fail3: serdev_device_set_drvdata(serdev, NULL);
 fail1:	input_free_device(input_dev);
 fail2:
	kfree(joycon);
	return err;
}

static void joycon_serdev_remove(struct serdev_device *serdev)
{
	struct joycon *joycon = serdev_device_get_drvdata(serdev);
	printk("Joy-Con remove!\n");
	
	input_unregister_device(joycon->input_dev);
	kfree(joycon);
}

/*
 * The serdev driver structure.
 */
#ifdef CONFIG_OF
static const struct of_device_id joycon_uart_of_match[] = {
	{ .compatible = "nintendo,joycon-uart" },
	{ },
};
MODULE_DEVICE_TABLE(of, joycon_uart_of_match);
#endif

static struct serdev_device_driver joycon_serdev_driver = {
	.probe = joycon_serdev_probe,
	.remove = joycon_serdev_remove,
	.driver = {
		.name = "joycon-uart",
		.of_match_table = of_match_ptr(joycon_uart_of_match),
	},
};

static struct serdev_device_ops joycon_ops = {
	.receive_buf = joycon_serdev_receive_buf,
	.write_wakeup = joycon_serdev_write_wakeup,
};

static int joycon_probe(struct platform_device *pdev)
{
	struct joycon_device *dev;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;
	dev->irq = platform_get_irq(pdev, 0);

	platform_set_drvdata(pdev, dev);

	dev_info(&pdev->dev, "%s device registered.\n", dev->name);

	/* Place this instance on the device list */
	mutex_lock(&joycon_device_lock);
	list_add_tail(&dev->list, &joycon_device_list);
	mutex_unlock(&joycon_device_lock);

	return 0;
}

static int joycon_remove(struct platform_device *pdev)
{
	struct joycon_device *dev = platform_get_drvdata(pdev);

	mutex_lock(&joycon_device_lock);
	list_del(&dev->list);
	mutex_unlock(&joycon_device_lock);

	dev_info(&pdev->dev, "%s device unregistered.\n", dev->name);

	return 0;
}

static struct platform_driver joycon_driver = {
	.probe = joycon_probe,
	.remove = joycon_remove,
	.driver = {
		.name = "joycon",
	},
};

int __init joycon_init(void)
{
	printk("joycon init\n");

	joycon_wq = create_workqueue("joycon_wq");

	platform_driver_register(&joycon_driver);
	serdev_device_driver_register(&joycon_serdev_driver);

	return 0;
}

void __exit joycon_exit(void)
{
	printk("joycon exit\n");
	platform_driver_unregister(&joycon_driver);
	serdev_device_driver_unregister(&joycon_serdev_driver);
	
	flush_workqueue(joycon_wq);
	destroy_workqueue(joycon_wq);
}

module_init(joycon_init);
module_exit(joycon_exit);
