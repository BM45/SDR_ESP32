#include "iqreader.h"
#include "board.h"
#include "globals.h"

#include "messages.h"
#include "esp_log.h"

#include "audio_pipeline.h"

#include "i2s_stream.h"
#include "raw_stream.h"
#include "wav_encoder.h"
#include "fft_audioelement.h"
#include "filter_audioelement.h"
#include "demodulator_audioelement.h"
#include "agc_audioelement.h"

#include "es8388.h"


static const char *TAG = "IQREADER";

void iqreader(void *pvParameters) {

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_reader, fft_audioelement , filter_audioelement, demodulator_audioelement,  i2s_stream_writer;
    audio_element_handle_t agc_audioelement; 

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "[ 1 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    // es8388.c::es8388_start must be fixed!
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_LINE_IN, AUDIO_HAL_CTRL_START);
    audio_hal_set_volume(board_handle->audio_hal, global_volume_setting);

/*
#ifdef CONFIG_ESP32_S2_KALUGA_1_V1_2_BOARD
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
#else
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_LINE_IN, AUDIO_HAL_CTRL_START);
#endif
*/

    ESP_LOGI(TAG, "[ 2 ] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);

    ESP_LOGI(TAG, "[3.1] Create i2s stream to write data to codec chip");

    i2s_stream_cfg_t i2s_cfg_write = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg_write.type = AUDIO_STREAM_WRITER;
    i2s_cfg_write.i2s_config.sample_rate = 44100;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg_write);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to read data from codec chip");
    i2s_stream_cfg_t i2s_cfg_read = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg_read.type = AUDIO_STREAM_READER;
    i2s_cfg_read.i2s_config.sample_rate = 44100;
    i2s_stream_reader = i2s_stream_init(&i2s_cfg_read);

    ESP_LOGI(TAG, "[3.3] Init FFT Audioelement");
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = _open_fft;
    cfg.process = _process_fft;
    cfg.tag = "fft";
    fft_audioelement = audio_element_init(&cfg);

    ESP_LOGI(TAG, "[3.4] Init Filter Audioelement");
    audio_element_cfg_t cfg_filter = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg_filter.open = _open_filter;
    cfg_filter.process = _process_filter;
    cfg_filter.tag = "filter";
    filter_audioelement = audio_element_init(&cfg_filter);

    ESP_LOGI(TAG, "[3.5] Init Demodulator Audioelement");
    audio_element_cfg_t cfg_demod = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg_demod.open = _open_demodulator;
    cfg_demod.process = _process_demodulator;
    cfg_demod.tag = "demodulator";
    demodulator_audioelement = audio_element_init(&cfg_demod);

    ESP_LOGI(TAG, "[3.6] Init AGC Audioelement");
    audio_element_cfg_t cfg_agc = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg_agc.open = _open_agc;
    cfg_agc.process = _process_agc;
    cfg_agc.tag = "demodulator";
    agc_audioelement = audio_element_init(&cfg_agc);

    ESP_LOGI(TAG, "[3.7] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s_read");
    audio_pipeline_register(pipeline, agc_audioelement, "agc");
    audio_pipeline_register(pipeline, fft_audioelement, "fft");
    audio_pipeline_register(pipeline, filter_audioelement, "filter");
    audio_pipeline_register(pipeline, demodulator_audioelement, "demodulator");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s_write");

    ESP_LOGI(TAG, "[3.8] Link it together [codec_chip]-->i2s_stream_reader-->[do DSP stuff]-->i2s_stream_writer-->[codec_chip]");
    const char *link_tag[6] = {"i2s_read", "agc", "fft", "filter", "demodulator", "i2s_write"};
    audio_pipeline_link(pipeline, &link_tag[0], 6);

    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "[ 6 ] Listen for all pipeline events");
    while (1) {
        audio_hal_set_volume(board_handle->audio_hal, global_volume_setting); // set volume in realtime

        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }

    }

    ESP_LOGI(TAG, "[ 7 ] Stop audio_pipeline");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    audio_pipeline_unregister(pipeline, i2s_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(i2s_stream_reader);
    audio_element_deinit(i2s_stream_writer);
}
