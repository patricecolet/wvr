#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp32-hal-log.h"
#include "midi_in.h"
#include "ws_log.h"
#include "wav_player.h"
#include "file_system.h"
#include "midi.h"

#define MIDI_UART_NUM UART_NUM_2
#define USB_MIDI_UART_NUM UART_NUM_1
#define BUF_SIZE (1024)
#define RD_BUF_SIZE (BUF_SIZE)

#define MIDI_RX_PIN 23 // wrv midi in
#define USB_MIDI_RX_PIN 21

static const char *TAG = "midi";

// from server.cpp
void sendWSMsg(char* msg);

esp_err_t ret;

QueueHandle_t wav_player_queue;
QueueHandle_t uart_queue; // uart Events queue
QueueHandle_t uart_queue_usb; // usb uart Events queue

struct wav_player_event_t wav_player_event;
uint8_t *msg;
uint8_t *usb_msg;
struct metadata_t metadata;

void(*midi_hook)(uint8_t *in);

void init_gpio()
{
  gpio_config_t io_conf;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pin_bit_mask = (1 << GPIO_NUM_23); // WVR MIDI IN
  io_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io_conf);
}

void init_gpio_usb()
{
  gpio_config_t io_conf;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pin_bit_mask = (1 << GPIO_NUM_21); // USB MIDI
  io_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io_conf);
}

void init_uart()
{
    uart_config_t uart_config = {
        .baud_rate = 31250,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    ESP_ERROR_CHECK(uart_param_config(MIDI_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(MIDI_UART_NUM, UART_PIN_NO_CHANGE, MIDI_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(MIDI_UART_NUM,1024, 0, 1024, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_set_rx_timeout(MIDI_UART_NUM, 1));
}

void init_uart_usb()
{
    uart_config_t uart_config = {
        .baud_rate = 31250,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    ESP_ERROR_CHECK(uart_param_config(USB_MIDI_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(USB_MIDI_UART_NUM, UART_PIN_NO_CHANGE, USB_MIDI_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(USB_MIDI_UART_NUM,1024, 0, 1024, &uart_queue_usb, 0));
    ESP_ERROR_CHECK(uart_set_rx_timeout(USB_MIDI_UART_NUM, 1));
}

#define MIDI_BUFFER_SIZE 256

uint8_t channel_lut_default[16] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
uint8_t channel_lut[16];

const struct pan_t channel_pan_default[16] = {FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE,FX_NONE};
struct pan_t channel_pan[16];

const uint8_t channel_vol_default[16] = {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127 };
uint8_t channel_vol[16];

const uint8_t channel_exp_default[16] = {100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100 };
uint8_t channel_exp[16];

uint8_t *get_channel_lut(void)
{
    return &channel_lut[0];
}

static void read_uart_task()
{
    uart_event_t event;
    int bytes_read;
    uint8_t* tmp = (uint8_t*)malloc(MIDI_BUFFER_SIZE);
    if(tmp == NULL)
    {
        log_e("failed to malloc tmp");
    }

    log_i("midi task running on core %u",xPortGetCoreID());
    // metadata = get_metadata();
    for(;;) {
        if(xQueueReceive(uart_queue, (void *)&event, (portTickType)portMAX_DELAY)) {
            bzero(tmp, MIDI_BUFFER_SIZE);
            if(event.type==UART_DATA)
            {
                bytes_read = uart_read_bytes(MIDI_UART_NUM, tmp, event.size, portMAX_DELAY);
                for(int i=0;i<bytes_read;i++)
                {
                    // returns uint8_t* or NULL
                    msg = midi_parse(tmp[i]);
                    if(msg)
                    {
                        handle_midi(msg);
                    }
                }
            }
            else if(event.type==UART_FIFO_OVF)
            {
                log_e("UART_FIFO_OVF");
            }
            else if(event.type==UART_BUFFER_FULL)
            {
                log_e("UART_BUFFER_FULL");
            }
            else if(event.type==UART_BREAK)
            {
                log_e("UART_BREAK");
            }
            else if(event.type==UART_PARITY_ERR)
            {
                log_e("UART_PARITY_ERR");
            }
            else if(event.type==UART_FRAME_ERR)
            {
                log_e("UART_FRAME_ERR");
            }
            else
            {
                log_e("other uart event %d", event.type);
            }
        }
    }
}

static void read_usb_uart_task()
{
    uart_event_t event;
    int bytes_read;
    uint8_t* tmp = (uint8_t*)malloc(MIDI_BUFFER_SIZE);
    if(tmp == NULL)
    {
        log_e("failed to malloc tmp");
    }

    log_i("midi usb task running on core %u",xPortGetCoreID());

    for(;;) {
        if(xQueueReceive(uart_queue_usb, (void *)&event, (portTickType)portMAX_DELAY)) {
            bzero(tmp, MIDI_BUFFER_SIZE);
            if(event.type==UART_DATA)
            {
                bytes_read = uart_read_bytes(USB_MIDI_UART_NUM, tmp, event.size, portMAX_DELAY);
                for(int i=0;i<bytes_read;i++)
                {
                    usb_msg = usb_midi_parse(tmp[i]);
                    if(usb_msg)
                    {
                        handle_midi(usb_msg);
                    }
                }
            }
            else if(event.type==UART_FIFO_OVF)
            {
                log_e("UART_USB_FIFO_OVF");
            }
            else if(event.type==UART_BUFFER_FULL)
            {
                log_e("UART_USB_BUFFER_FULL");
            }
            else if(event.type==UART_BREAK)
            {
                log_e("UART_USB_BREAK");
            }
            else if(event.type==UART_PARITY_ERR)
            {
                log_e("UART_USB_PARITY_ERR");
            }
            else if(event.type==UART_FRAME_ERR)
            {
                log_e("UART_USB_FRAME_ERR");
            }
            else
            {
                log_e("other uart usb event %d", event.type);
            }
        }
    }
}

static void handle_midi(uint8_t *msg)
{
    // send it through the midi filter hook
    // log_i("midi %d %d %d",msg[0],msg[1],msg[2]);
    midi_hook(msg);
    if(msg)
    {
        uint8_t channel = msg[0] & 0b00001111;
        // log_i("chan %d listening on %d", channel, metadata.midi_channel);
        if(
            (metadata.midi_channel != 0) && // WVR is not in OMNI mode
            // have to add one to channel here, because midi data 0 means midi channel 1 (eye roll)
            (metadata.midi_channel != (channel + 1)) // this is not the channel WVR is listening on
        )
        {
            msg = NULL;
        }
    }
    if(msg)
    {
        uint8_t channel = msg[0] & 0b00001111;
        // log_i("chan %d", channel);
        uint8_t code = (msg[0] >> 4) & 0b00001111;
        switch (code)
        {
        case MIDI_NOTE_ON:
        case MIDI_NOTE_OFF:
            {
                struct wav_player_event_t wav_player_event;
                wav_player_event.code = (msg[0] >> 4) & 0b00001111;
                wav_player_event.voice = channel_lut[channel];
                wav_player_event.note = msg[1] & 0b01111111;
                wav_player_event.velocity = msg[2]  & 0b01111111;
                wav_player_event.channel = channel;
                xQueueSendToBack(wav_player_queue,(void *) &wav_player_event, portMAX_DELAY);                  
                // log_i("%d: note:%d velocity:%d channel:%d voice:%d code:%d",
                //     i,
                //     wav_player_event.note,
                //     wav_player_event.velocity,
                //     wav_player_event.channel,
                //     wav_player_event.voice,
                //     wav_player_event.code
                // );
            }
            break;
        case MIDI_PROGRAM_CHANGE:
            {
                uint8_t voice = msg[1] & 0b01111111;
                // log_e("prog chng %d on channel %d",voice, channel);
                // if the program change is out of range set it to the highest voice
                voice = voice < NUM_VOICES ? voice : NUM_VOICES - 1;
                channel_lut[channel] = voice;
                break;
            }
        case MIDI_CC:
            {
                uint8_t CC = msg[1] & 0b01111111;
                uint8_t val = msg[2]  & 0b01111111;
                // log_i("MIDI_CC:#%d val:%d on channel %d", CC, val, channel);
                switch (CC)
                {
                case MIDI_CC_PAN:
                    // log_e("Pan %d",val);
                    if(val == 64)
                    {
                        channel_pan[channel].left_vol = 127;
                        channel_pan[channel].right_vol = 127;
                    }
                    else if(val == 0)
                    {
                        channel_pan[channel].right_vol = 0;
                        channel_pan[channel].left_vol = 127;
                    }
                    else if(val == 127)
                    {
                        channel_pan[channel].right_vol = 127;
                        channel_pan[channel].left_vol = 0;
                    }
                    else if(val > 64)
                    {
                        channel_pan[channel].right_vol = 127;
                        channel_pan[channel].left_vol = 127 - ((val - 64) * 2);
                    }
                    else if(val < 64)
                    {
                        channel_pan[channel].right_vol = 127 - ((64 - val) * 2);
                        channel_pan[channel].left_vol = 127;
                    }
                    break;
                case MIDI_CC_VOLUME:
                    channel_vol[channel] = val;
                    break;
                case MIDI_CC_EXP:
                    channel_exp[channel] = val;
                    break;
                default:
                    break;
                }
            }
        default:
            break;
        }
    }
}
}

void reset_midi_controllers(void)
{
    for(in i=0; i<16; i++)
    {
        channel_lut[i] = channel_lut_default[i];
        channel_pan[i] = channel_pan_default[i];
        channel_vol[i] = channel_vol_default[i];
        channel_exp[i] = channel_exp_default[i];
    }
}

void midi_init(bool useUsbMidi)
{
    reset_midi_controllers();
    init_gpio();
    init_uart();
    midi_hook = midi_hook_default;
    xTaskCreatePinnedToCore(read_uart_task, "read_uart_task", 4096, NULL, 3, NULL, 0);
    if(useUsbMidi)
    {
        init_gpio_usb();
        init_uart_usb();
        xTaskCreatePinnedToCore(read_usb_uart_task, "read_usb_uart_task", 4096, NULL, 3, NULL, 0);
    }
    // xTaskCreatePinnedToCore(read_uart_task, "read_uart_task", 4096, NULL, 3, NULL, 1);
}

void midi_hook_default(uint8_t* in)
{
    return;
}

void set_midi_hook(void(*fn)(uint8_t *in))
{
    midi_hook = fn;
}