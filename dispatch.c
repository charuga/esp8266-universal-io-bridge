#include "dispatch.h"

#include "util.h"
#include "application.h"
#include "io.h"
#include "stats.h"
#include "i2c.h"
#include "display.h"
#include "time.h"
#include "i2c_sensor.h"
#include "sequencer.h"
#include "init.h"
#include "lwip-interface.h"

typedef enum
{
	ts_copy,
	ts_dodont,
	ts_data,
} telnet_strip_state_t;

_Static_assert(sizeof(telnet_strip_state_t) == 4, "sizeof(telnet_strip_state) != 4");

enum
{
	uart_task_id					= USER_TASK_PRIO_2,
	uart_task_queue_length			= 3,

	command_task_id					= USER_TASK_PRIO_1,
	command_task_queue_length		= 12,

	timer_task_id					= USER_TASK_PRIO_0,
	timer_task_queue_length			= 2,
};

static os_event_t uart_task_queue[uart_task_queue_length];
static os_event_t command_task_queue[command_task_queue_length];
static os_event_t timer_task_queue[timer_task_queue_length];

string_new(attr_flash_align, flash_sector_buffer, 4096);

string_new(static attr_flash_align, command_socket_receive_buffer, 4096 + 64);
string_new(static attr_flash_align, command_socket_send_buffer, 4096 + 64);
unsigned int command_left_to_read;
static lwip_if_socket_t command_socket;

string_new(static, uart_socket_receive_buffer, 128);
string_new(static, uart_socket_send_buffer, 128);
static lwip_if_socket_t uart_socket;

static _Bool uart_bridge_active = false;

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

static void background_task_bridge_uart(void)
{
	if(uart_empty(0) || lwip_if_send_buffer_locked(&uart_socket))
		return;

	string_clear(&uart_socket_send_buffer);

	while(!uart_empty(0) && string_space(&uart_socket_send_buffer))
		string_append_byte(&uart_socket_send_buffer, uart_receive(0));

	if(string_empty(&uart_socket_send_buffer))
		return;

	if(!lwip_if_send(&uart_socket))
	{
		stat_uart_send_buffer_overflow++;
		log("lwip uart send failed\n");
	}
}

static void command_task(os_event_t *event)
{
	int trigger_io, trigger_pin;
	string_init(varname_alert_assoc_io, "trigger.assoc.io");
	string_init(varname_alert_assoc_pin, "trigger.assoc.pin");
	string_init(varname_alert_status_io, "trigger.status.io");
	string_init(varname_alert_status_pin, "trigger.status.pin");

	switch(event->sig)
	{
		case(command_task_reset):
		{
			reset();
			break;
		}

		case(command_task_uart_bridge):
		{
			background_task_bridge_uart();
			stat_update_uart++;
			break;
		}

		case(command_task_init_i2c_sensors):
		{
			if(i2c_sensors_init())
				dispatch_post_command(command_task_init_i2c_sensors);
			break;
		}

		case(command_task_init_displays):
		{
			uint32_t now = system_get_time();
			display_init();
			stat_display_init_time_us = system_get_time() - now;
			break;
		}

		case(command_task_received_command):
		{
			app_action_t action;

			if(lwip_if_received_tcp(&command_socket))
				stat_update_command_tcp++;

			if(lwip_if_received_udp(&command_socket))
				stat_update_command_udp++;

			if(lwip_if_send_buffer_locked(&command_socket))
			{
				stat_cmd_send_buffer_overflow++;
				string_clear(&command_socket_receive_buffer);
				lwip_if_receive_buffer_unlock(&command_socket);
				break;
			}

			string_clear(&command_socket_send_buffer);

			action = application_content(&command_socket_receive_buffer, &command_socket_send_buffer);

			string_clear(&command_socket_receive_buffer);
			lwip_if_receive_buffer_unlock(&command_socket);

			if(action == app_action_empty)
			{
				string_clear(&command_socket_send_buffer);
				string_append(&command_socket_send_buffer, "> empty command\n");
			}

			if(action == app_action_disconnect)
			{
				string_clear(&command_socket_send_buffer);
				string_append(&command_socket_send_buffer, "> disconnect\n");
			}

			if(action == app_action_reset)
			{
				string_clear(&command_socket_send_buffer);
				string_append(&command_socket_send_buffer, "> reset\n");
			}

			if(!lwip_if_send(&command_socket))
				log("lwip send failed\n");

			if(action == app_action_disconnect)
				lwip_if_close(&command_socket);

			/*
			 * === ugly workaround ===
			 *
			 * For tcp connections we can use the "sent" callback to make sure all
			 * of our data has been sent before rebooting. For udp there is no such
			 * callback and waiting for it to happen does not work (need a return to
			 * SDK code to achieve it). So lwip_if_reboot will take care for it itself
			 * when possible (tcp), otherwise (udp) it will return false here and the
			 * application needs to finish the operation via a task call.
			 */

			if(action == app_action_reset)
				if(!lwip_if_reboot(&command_socket))
					dispatch_post_command(command_task_reset);

			break;
		}

		case(command_task_display_update):
		{
			stat_update_display++;

			if(display_periodic())
				dispatch_post_command(command_task_display_update);

			break;
		}

		case(command_task_fallback_wlan):
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

		case(command_task_update_time):
		{
			time_periodic();
			break;
		}

		case(command_task_run_sequencer):
		{
			sequencer_run();
			break;
		}

		case(command_task_alert_association):
		{
			if((config_get_int(&varname_alert_assoc_io, -1, -1, &trigger_io) &&
					config_get_int(&varname_alert_assoc_pin, -1, -1, &trigger_pin) &&
					(trigger_io >= 0) && (trigger_pin >= 0)))
				io_trigger_pin((string_t *)0, trigger_io, trigger_pin, io_trigger_on);

			break;
		}

		case(command_task_alert_disassociation):
		{
			if((config_get_int(&varname_alert_assoc_io, -1, -1, &trigger_io) &&
					config_get_int(&varname_alert_assoc_pin, -1, -1, &trigger_pin) &&
					(trigger_io >= 0) && (trigger_pin >= 0)))
				io_trigger_pin((string_t *)0, trigger_io, trigger_pin, io_trigger_off);

			break;
		}

		case(command_task_alert_status):
		{
			if((config_get_int(&varname_alert_status_io, -1, -1, &trigger_io) &&
					config_get_int(&varname_alert_status_pin, -1, -1, &trigger_pin) &&
					(trigger_io >= 0) && (trigger_pin >= 0)))
				io_trigger_pin((string_t *)0, trigger_io, trigger_pin, io_trigger_on);

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

iram static void fast_timer_callback(void *arg)
{
	// timer runs every 10 ms = 100 Hz

	stat_fast_timer++;
	dispatch_post_timer(timer_task_io_periodic_fast);
}

iram static void slow_timer_callback(void *arg)
{
	// run background task every ~100 ms = ~10 Hz

	stat_slow_timer++;

	dispatch_post_command(command_task_update_time);

	if(uart_bridge_active)
		dispatch_post_command(command_task_uart_bridge);

	if(display_detected())
		dispatch_post_command(command_task_display_update);

	// fallback to config-ap-mode when not connected or no ip within 30 seconds

	if((stat_slow_timer == 300) && (wifi_station_get_connect_status() != STATION_GOT_IP))
		dispatch_post_command(command_task_fallback_wlan);

	dispatch_post_timer(timer_task_io_periodic_slow);
}

static void wlan_event_handler(System_Event_t *event)
{
	string_init(varname_alert_assoc_io, "trigger.assoc.io");
	string_init(varname_alert_assoc_pin, "trigger.assoc.pin");

	switch(event->event)
	{
		case(EVENT_STAMODE_GOT_IP):
		{
			if(!lwip_if_join_mc(239, 255, 255, 254))
				log("join mc group failed\n");

			fallthrough;
		}
		case(EVENT_SOFTAPMODE_STACONNECTED):
		{
			dispatch_post_command(command_task_alert_association);
			dispatch_post_command(command_task_init_i2c_sensors);
			break;
		}

		case(EVENT_STAMODE_DISCONNECTED):
		{
			fallthrough;
		}
		case(EVENT_SOFTAPMODE_STADISCONNECTED):
		{
			dispatch_post_command(command_task_alert_disassociation);
			break;
		}
	}
}

static void socket_command_callback_data_received(lwip_if_socket_t *socket, unsigned int length)
{
	static const uint8_t command_string[] = "flash-send ";
	uint32_t chunk_length;
	int chunk_offset;

	if((command_left_to_read == 0) &&
			string_nmatch_cstr(&command_socket_receive_buffer, command_string, sizeof(command_string) - 1) &&
			(parse_uint(2, &command_socket_receive_buffer, &chunk_length, 10, ' ') == parse_ok) &&
			((chunk_offset = string_sep(&command_socket_receive_buffer, 0, 3, ' ')) >= 0))
		command_left_to_read = chunk_offset + chunk_length;

	if(command_left_to_read > 0)
	{
		if(length > command_left_to_read)
			command_left_to_read = 0;
		else
			command_left_to_read -= length;
	}

	if((command_left_to_read == 0) && (string_trim_nl(&command_socket_receive_buffer) || lwip_if_received_udp(socket)))
		dispatch_post_command(command_task_received_command);
	else
		lwip_if_receive_buffer_unlock(&command_socket);
}

static void socket_uart_callback_data_received(lwip_if_socket_t *socket, unsigned int received)
{
	int current, length;
	uint8_t byte;
	_Bool strip_telnet;
	telnet_strip_state_t telnet_strip_state;

	length = string_length(&uart_socket_receive_buffer);

	strip_telnet = config_flags_match(flag_strip_telnet);
	telnet_strip_state = ts_copy;

	for(current = 0; current < length; current++)
	{
		byte = string_at(&uart_socket_receive_buffer, current);

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

	string_clear(&uart_socket_receive_buffer);
	lwip_if_receive_buffer_unlock(&uart_socket);
	uart_flush(0);
}

void dispatch_init1(void)
{
	system_os_task(uart_task, uart_task_id, uart_task_queue, uart_task_queue_length);
	system_os_task(command_task, command_task_id, command_task_queue, command_task_queue_length);
	system_os_task(timer_task, timer_task_id, timer_task_queue, timer_task_queue_length);
}

void dispatch_init2(void)
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

	command_left_to_read = 0;

	lwip_if_socket_create(&command_socket, &command_socket_receive_buffer, &command_socket_send_buffer, cmd_port,
			config_flags_match(flag_udp_term_empty), socket_command_callback_data_received);

	if(uart_port > 0)
	{
		lwip_if_socket_create(&uart_socket, &uart_socket_receive_buffer, &uart_socket_send_buffer, uart_port,
			config_flags_match(flag_udp_term_empty), socket_uart_callback_data_received);

		uart_bridge_active = true;
	}

	os_timer_setfn(&slow_timer, slow_timer_callback, (void *)0);
	os_timer_arm(&slow_timer, 100, 1); // slow system timer / 10 Hz / 100 ms

	os_timer_setfn(&fast_timer, fast_timer_callback, (void *)0);
	os_timer_arm(&fast_timer, 10, 1); // fast system timer / 100 Hz / 10 ms

	dispatch_post_command(command_task_init_displays);
}
