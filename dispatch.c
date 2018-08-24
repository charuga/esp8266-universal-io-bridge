#include "dispatch.h"

#include "util.h"
#include "application.h"
#include "io.h"
#include "stats.h"
#include "i2c.h"
#include "display.h"
#include "time.h"
#include "i2c_sensor.h"
#include "socket.h"
#include "sequencer.h"
#include "init.h"

#if IMAGE_OTA == 1
#include <rboot-api.h>
#endif

typedef enum
{
	ts_copy,
	ts_dodont,
	ts_data,
} telnet_strip_state_t;

typedef enum
{
	socket_state_idle,
	socket_state_received,
	socket_state_processing,
	socket_state_sending,
} socket_state_t;

_Static_assert(sizeof(telnet_strip_state_t) == 4, "sizeof(telnet_strip_state) != 4");

enum
{
	uart_task_id					= USER_TASK_PRIO_0,
	uart_task_queue_length			= 3,

	command_task_id					= USER_TASK_PRIO_1,
	command_task_queue_length		= 12,

	timer_task_id					= USER_TASK_PRIO_2,
	timer_task_queue_length			= 2,
};

static os_event_t uart_task_queue[uart_task_queue_length];
static os_event_t command_task_queue[command_task_queue_length];
static os_event_t timer_task_queue[timer_task_queue_length];

typedef struct
{
	socket_t		socket;
	socket_state_t	state;
	string_t		receive_buffer;
	string_t		send_buffer;
} socket_data_t;

string_new(attr_flash_align, flash_sector_buffer, 4096);
static char _socket_cmd_send_buffer[4096 + 8];

static socket_data_t socket_cmd =
{
	.state = socket_state_idle,
	.receive_buffer =
	{
		.length = 0,
		.size = 0,
		.buffer = (char *)0
	},
	.send_buffer =
	{
		.length = 0,
		.size = sizeof(_socket_cmd_send_buffer),
		.buffer = _socket_cmd_send_buffer
	}
};

static char _socket_uart_send_buffer[1024];

static socket_data_t socket_uart =
{
	.state = socket_state_idle,

	.receive_buffer =
	{
		.length = 0,
		.size = 0,
		.buffer = (char *)0
	},
	.send_buffer =
	{
		.length = 0,
		.size = sizeof(_socket_uart_send_buffer),
		.buffer = _socket_uart_send_buffer
	}
};

static bool_t uart_bridge_active = false;

static struct
{
	unsigned int disconnect:1;
	unsigned int init_i2c_sensors:1;
	unsigned int init_displays:1;
	unsigned int preparing_reset:1;
} bg_action =
{
	.disconnect = 0,
	.init_i2c_sensors = 0,
	.init_displays = 0,
	.preparing_reset = 0,
};

static ETSTimer fast_timer;
static ETSTimer slow_timer;

iram void dispatch_post_uart(task_command_t command)
{
	if(system_os_post(uart_task_id, command, 0))
		stat_task_uart_posted++;
	else
		stat_task_uart_failed++;
}

iram void dispatch_post_command(task_command_t command)
{
	if(system_os_post(command_task_id, command, 0))
		stat_task_command_posted++;
	else
		stat_task_command_failed++;
}

iram void dispatch_post_timer(task_command_t command)
{
	if(system_os_post(timer_task_id, command, 0))
		stat_task_timer_posted++;
	else
		stat_task_timer_failed++;
}

irom static bool_t background_task_bridge_uart(void)
{
	if(socket_uart.state == socket_state_idle)
	{
		while(!uart_empty(0) && string_space(&socket_uart.send_buffer))
			string_append_char(&socket_uart.send_buffer, uart_receive(0));

		if(!string_empty(&socket_uart.send_buffer))
		{
			socket_uart.state = socket_state_sending;

			if(socket_send(&socket_uart.socket, &socket_uart.send_buffer))
				return(true);
			else
			{
				string_clear(&socket_uart.send_buffer);
				socket_uart.state = socket_state_idle;
				stat_uart_send_buffer_overflow++;
				return(false);
			}
		}
	}

	return(false);
}

irom static void background_task_command_handler(void)
{
	socket_cmd.state = socket_state_processing;

	string_clear(&socket_cmd.send_buffer);

	switch(application_content(&socket_cmd.receive_buffer, &socket_cmd.send_buffer))
	{
		case(app_action_normal):
		case(app_action_error):
		case(app_action_http_ok):
		{
			/* no special action for now */
			break;
		}
		case(app_action_empty):
		{
			string_clear(&socket_cmd.send_buffer);
			string_append(&socket_cmd.send_buffer, "> empty command\n");
			break;
		}
		case(app_action_disconnect):
		{
			string_clear(&socket_cmd.send_buffer);
			string_append(&socket_cmd.send_buffer, "> disconnect\n");
			bg_action.disconnect = 1;
			break;
		}
	}

	socket_cmd.state = socket_state_sending;

	if(!socket_send(&socket_cmd.socket, &socket_cmd.send_buffer))
	{
		stat_cmd_send_buffer_overflow++;
		socket_cmd.state = socket_state_idle;
	}
}

irom static void command_task(os_event_t *event)
{
	switch(event->sig)
	{
		case(command_task_command_reset):
		{
			bg_action.preparing_reset = 1;

			if((socket_proto(&socket_cmd.socket) == proto_udp) && !socket_send_busy(&socket_cmd.socket))
			{
				msleep(100);
				dispatch_post_command(command_task_command_reset_finish);
			}

			socket_disconnect_accepted(&socket_cmd.socket);
			break;
		}

		case(command_task_command_reset_finish):
		{
			if(bg_action.preparing_reset)
				reset();
			break;
		}

		case(command_task_command_uart_bridge):
		{
			stat_update_uart++;

			if(background_task_bridge_uart())
				dispatch_post_command(command_task_command_uart_bridge);
			break;
		}

		case(command_task_command_disconnect):
		{
			socket_disconnect_accepted(&socket_cmd.socket);
			bg_action.disconnect = 0;
			stat_update_longop++;
			break;
		}

		case(command_task_command_init_i2c_sensors):
		{
			uint32_t now = system_get_time();
			i2c_sensor_init_all();
			bg_action.init_i2c_sensors = 0;
			stat_i2c_init_time_us = system_get_time() - now;
			stat_update_longop++;
			break;
		}

		case(command_task_command_init_displays):
		{
			uint32_t now = system_get_time();
			display_init();
			bg_action.init_displays = 0;
			stat_display_init_time_us = system_get_time() - now;
			stat_update_longop++;
			break;
		}

		case(command_task_command_received_command):
		{
			if(socket_proto(&socket_cmd.socket) == proto_tcp)
				stat_update_command_tcp++;
			else
				stat_update_command_udp++;

			background_task_command_handler();

			break;
		}

		case(command_task_command_display_update):
		{
			stat_update_display++;

			if(display_periodic())
				dispatch_post_command(command_task_command_display_update);

			break;
		}

		case(command_task_command_fallback_wlan):
		{
			config_wlan_mode_t wlan_mode;
			int wlan_mode_int;
			string_init(varname_wlan_mode, "wlan.mode");

			if(config_get_int(&varname_wlan_mode, -1, -1, &wlan_mode_int))
				wlan_mode = (config_wlan_mode_t)wlan_mode_int;
			else
				wlan_mode = config_wlan_mode_client;

			if(wlan_mode == config_wlan_mode_client)
			{
				wlan_mode_int = (int)config_wlan_mode_ap;
				config_set_int(&varname_wlan_mode, -1, -1, wlan_mode_int);
				config_get_int(&varname_wlan_mode, -1, -1, &wlan_mode_int);
				wlan_init();
			}

			break;
		}

		case(command_task_command_update_time):
		{
			time_periodic();
			break;
		}

		case(command_task_command_run_sequencer):
		{
			sequencer_run();
			break;
		}
	}
}

iram static void timer_task(os_event_t *event)
{
	switch(event->sig)
	{
		case(timer_task_io_periodic_fast):
		{
			io_periodic_fast();
			break;
		}

		case(timer_task_io_periodic_slow):
		{
			io_periodic_slow();
			break;
		}
	}
}

iram attr_speed static void fast_timer_callback(void *arg)
{
	// timer runs every 10 ms = 100 Hz

	stat_fast_timer++;
	dispatch_post_timer(timer_task_io_periodic_fast);
}

iram attr_speed static void slow_timer_callback(void *arg)
{
	// run background task every ~100 ms = ~10 Hz

	time_periodic();

	stat_slow_timer++;

	dispatch_post_command(command_task_command_update_time);

	if(uart_bridge_active)
		dispatch_post_command(command_task_command_uart_bridge);

	if(bg_action.disconnect)
		dispatch_post_command(command_task_command_disconnect);

	if(bg_action.init_i2c_sensors)
		dispatch_post_command(command_task_command_init_i2c_sensors);

	if(bg_action.init_displays)
		dispatch_post_command(command_task_command_init_displays);

	if(display_detected())
		dispatch_post_command(command_task_command_display_update);

	// fallback to config-ap-mode when not connected or no ip within 30 seconds

	if((stat_slow_timer == 300) && (wifi_station_get_connect_status() != STATION_GOT_IP))
		dispatch_post_command(command_task_command_fallback_wlan);

	dispatch_post_timer(timer_task_io_periodic_slow);
}

irom static void wlan_event_handler(System_Event_t *event)
{
	int trigger_io, trigger_pin;
	io_trigger_t trigger = io_trigger_none;
	struct ip_info info;
	ip_addr_to_bytes_t local_ip;
	ip_addr_to_bytes_t mc_ip;
	string_init(varname_assoc_io, "trigger.assoc.io");
	string_init(varname_assoc_pin, "trigger.assoc.pin");

	switch(event->event)
	{
		case(EVENT_STAMODE_GOT_IP):
		{
			wifi_get_ip_info(STATION_IF, &info);
			local_ip.ip_addr = info.ip;
			mc_ip.byte[0] = 239;
			mc_ip.byte[1] = 255;
			mc_ip.byte[2] = 255;
			mc_ip.byte[3] = 254;
			espconn_igmp_join(&local_ip.ip_addr, &mc_ip.ip_addr);

			logfmt("* join mc from %d.%d.%d.%d", local_ip.byte[0], local_ip.byte[1], local_ip.byte[2], local_ip.byte[3]);

			// fall through
		}
		case(EVENT_SOFTAPMODE_STACONNECTED):
		{
			trigger = io_trigger_on;
			break;
		}

		case(EVENT_STAMODE_DISCONNECTED):
		{
			// fall through
		}
		case(EVENT_SOFTAPMODE_STADISCONNECTED):
		{
			trigger = io_trigger_off;
			break;
		}
	}

	if((trigger != io_trigger_none) &&
			(config_get_int(&varname_assoc_io, -1, -1, &trigger_io) &&
			config_get_int(&varname_assoc_pin, -1, -1, &trigger_pin) &&
			(trigger_io >= 0) && (trigger_pin >= 0)))
		io_trigger_pin((string_t *)0, trigger_io, trigger_pin, trigger);
}

// SOCKET CALLBACKS

// received

irom static void callback_received_cmd(socket_t *socket, const string_t *buffer, void *userdata)
{
	if(socket_cmd.state != socket_state_idle)
	{
		stat_cmd_receive_buffer_overflow++;
		return;
	}

	socket_cmd.receive_buffer = *buffer;
	socket_cmd.state = socket_state_received;
	dispatch_post_command(command_task_command_received_command);
}

irom static void callback_received_uart(socket_t *socket, const string_t *buffer, void *userdata)
{
	int current, length;
	uint8_t byte;
	bool_t strip_telnet;
	telnet_strip_state_t telnet_strip_state;

	length = string_length(buffer);

	strip_telnet = config_flags_get().flag.strip_telnet;
	telnet_strip_state = ts_copy;

	for(current = 0; current < length; current++)
	{
		byte = string_at(buffer, current);

		switch(telnet_strip_state)
		{
			case(ts_copy):
			{
				if(strip_telnet && (byte == 0xff))
					telnet_strip_state = ts_dodont;
				else
				{
					if(uart_full(0))
						stat_uart_receive_buffer_overflow++;
					else
						uart_send(0, byte);
				}

				break;
			}
			case(ts_dodont):
			{
				telnet_strip_state = ts_data;
				break;
			}
			case(ts_data):
			{
				telnet_strip_state = ts_copy;
				break;
			}
		}
	}

	uart_flush(0);
}

// sent

irom attr_speed static void callback_sent_cmd(socket_t *socket, void *userdata)
{
	if(bg_action.preparing_reset && socket_proto(socket) == proto_udp)
		dispatch_post_command(command_task_command_reset_finish);

	socket_cmd.state = socket_state_idle;
}

irom attr_speed static void callback_sent_uart(socket_t *socket, void *userdata)
{
	if(!uart_empty(0))
		dispatch_post_command(command_task_command_uart_bridge); // retry to send data still in the fifo

	string_clear(&socket_uart.send_buffer);
	socket_uart.state = socket_state_idle;
}

// error

irom static void callback_error_cmd(socket_t *socket, int error, void *userdata)
{
	socket_cmd.state = socket_state_idle;
}

irom static void callback_error_uart(socket_t *socket, int error, void *userdata)
{
	string_clear(&socket_uart.send_buffer);
	socket_uart.state = socket_state_idle;
}

// disconnect

irom static void callback_disconnect_cmd(socket_t *socket, void *userdata)
{
	if(bg_action.preparing_reset)
		dispatch_post_command(command_task_command_reset_finish);

	socket_cmd.state = socket_state_idle;
}

irom static void callback_disconnect_uart(socket_t *socket, void *userdata)
{
	string_clear(&socket_uart.send_buffer);
	socket_uart.state = socket_state_idle;
}

// accept

irom attr_speed static void callback_accept_cmd(socket_t *socket, void *userdata)
{
	socket_cmd.state = socket_state_idle;
}

irom attr_speed static void callback_accept_uart(socket_t *socket, void *userdata)
{
	uart_clear_send_queue(0);
	uart_clear_receive_queue(0);

	string_clear(&socket_uart.send_buffer);
	socket_uart.state = socket_state_idle;
}

irom void dispatch_init1(void)
{
	bg_action.disconnect = 0;
	bg_action.init_i2c_sensors = 1;
	bg_action.init_displays = 1;
}

irom void dispatch_init2(void)
{
	int cmd_port, cmd_timeout;
	int uart_port, uart_timeout;
	string_init(varname_cmd_port, "cmd.port");
	string_init(varname_cmd_timeout, "cmd.timeout");
	string_init(varname_bridge_port, "bridge.port");
	string_init(varname_bridge_timeout, "bridge.timeout");

	if(!config_get_int(&varname_cmd_port, -1, -1, &cmd_port))
		cmd_port = 24;

	if(!config_get_int(&varname_cmd_timeout, -1, -1, &cmd_timeout))
		cmd_timeout = 90;

	if(!config_get_int(&varname_bridge_port, -1, -1, &uart_port))
		uart_port = 0;

	if(!config_get_int(&varname_bridge_timeout, -1, -1, &uart_timeout))
		uart_timeout = 90;

	wifi_set_event_handler_cb(wlan_event_handler);

	socket_create(true, true, &socket_cmd.socket, cmd_port, cmd_timeout,
			callback_received_cmd, callback_sent_cmd, callback_error_cmd, callback_disconnect_cmd, callback_accept_cmd, (void *)&socket_cmd);

	if(uart_port > 0)
	{
		socket_create(true, true, &socket_uart.socket, uart_port, uart_timeout,
				callback_received_uart, callback_sent_uart, callback_error_uart, callback_disconnect_uart, callback_accept_uart, (void *)&socket_uart);

		uart_bridge_active = true;
	}

	system_os_task(uart_task, uart_task_id, uart_task_queue, uart_task_queue_length);
	system_os_task(command_task, command_task_id, command_task_queue, command_task_queue_length);
	system_os_task(timer_task, timer_task_id, timer_task_queue, timer_task_queue_length);

	os_timer_setfn(&slow_timer, slow_timer_callback, (void *)0);
	os_timer_arm(&slow_timer, 100, 1); // slow system timer / 10 Hz / 100 ms

	os_timer_setfn(&fast_timer, fast_timer_callback, (void *)0);
	os_timer_arm(&fast_timer, 10, 1); // fast system timer / 100 Hz / 10 ms
}