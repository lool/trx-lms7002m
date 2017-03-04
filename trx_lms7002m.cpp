/*
 * LimeMicroSystem transceiver driver
 *
 * Copyright (C) 2015 Amarisoft/LimeMicroSystems
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <getopt.h>
#include <math.h>
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <iostream>


#define TRX_BUFFER_COUNT     32      //number of in-flight tranfer buffers

#define TRX_TRANSFER_SIZE    4096  //size of a single transfer buffer

#define SAMPLES_PER_PACKET  1360    //number of complex 12 bit samples in one packet


extern "C" {
#include "trx_driver.h"
};

#include <lime/LimeSuite.h>


using namespace std;
typedef struct TRXLmsState          TRXLmsState;


struct TRXLmsState {
    lms_device_t *device;
    lms_stream_t rx_stream[4];
    lms_stream_t tx_stream[4];
    int tcxo_calc;          /* values from 0 to 255*/
    int   dec_inter;
    int started;
    int sample_rate;
    int tx_channel_count;
    int rx_channel_count;
    char* calibration;
};

static int64_t trx_lms_t0 = 0;

static int64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + (ts.tv_nsec / 1000U) - trx_lms_t0;
}

static void trx_lms7002m_end(TRXState *s1)
{
    TRXLmsState *s = (TRXLmsState*)s1->opaque;
    for (int ch = 0; ch < s->rx_channel_count; ch++)
    {
	LMS_StopStream(&s->rx_stream[ch]);
	LMS_DestroyStream(s->device,&s->rx_stream[ch]);
    }

    for (int ch = 0; ch < s->tx_channel_count; ch++)
    {
	LMS_StopStream(&s->tx_stream[ch]);
	LMS_DestroyStream(s->device,&s->tx_stream[ch]);
    }

    LMS_Close(s->device);
    free(s);
}

static void trx_lms7002m_write(TRXState *s1, trx_timestamp_t timestamp,
                               const void **samples, int count, int flags,
                               int rf_port_index)
{
    TRXLmsState *s = (TRXLmsState*)s1->opaque;

    // Nothing to transmit
    if (!samples)
        return;
    lms_stream_meta_t meta;
    meta.waitForTimestamp = true;
    meta.flushPartialPacket = false;
    meta.timestamp = timestamp;

    for (int ch = 0; ch < s->tx_channel_count; ch++)
    	LMS_SendStream(&s->tx_stream[ch],(const void*)samples[ch],count,&meta,30);

}

static int trx_lms7002m_read(TRXState *s1, trx_timestamp_t *ptimestamp, void **psamples, int count, int port)
{
    TRXLmsState *s = (TRXLmsState*)s1->opaque;
    lms_stream_meta_t meta;
    meta.waitForTimestamp = false;
    meta.flushPartialPacket = false;
    static uint64_t next_ts = 0;

    // First shot ?
    if (!s->started) {
    	for (int ch = 0; ch < s->rx_channel_count; ch++)
		LMS_StartStream(&s->rx_stream[ch]);
    	for (int ch = 0; ch < s->tx_channel_count; ch++)
		LMS_StartStream(&s->tx_stream[ch]);
        s->started = 1;
        printf("START\n");
    }

    int ret;
    for (int ch = 0; ch < s->rx_channel_count; ch++)
    	ret = LMS_RecvStream(&s->rx_stream[ch],psamples[ch],count,&meta,30);

    *ptimestamp = meta.timestamp;

    return ret;
}

#define SAMPLE_RATE_COUNT 8
static int trx_lms7002m_get_sample_rate(TRXState *s1, TRXFraction *psample_rate,
                                     int *psample_rate_num, int sample_rate_min)
{
    TRXLmsState *s = (TRXLmsState*)s1->opaque;

    // sample rate not specified, align on 1.92Mhz
    if (!s->sample_rate) {
        int i, n;
        static const int sample_rate_num_tab[SAMPLE_RATE_COUNT] = {
            1,
            2,
            3,
            6,
            9,
            12,
            16,
            18,
        };
        // leave some space for the low pass filter
        sample_rate_min = lround(sample_rate_min * 1.125);
        for(i = 0; i < SAMPLE_RATE_COUNT; i++) {
            n = sample_rate_num_tab[i];
            if (sample_rate_min <= n * 1920000) {
                *psample_rate_num = n;
                psample_rate->num = n * 1920000;
                psample_rate->den = 1;
                return 0;
            }
        }

    } else {
        int sr;

        for (sr = (int)(s->sample_rate); sr >= sample_rate_min && ((sr % 1000) == 0); sr >>= 1) {
            psample_rate->num = sr;
        }

        psample_rate->den = 1;
        *psample_rate_num = 0;
        return 0;
    }
    return -1;
}

static int trx_lms7002m_get_tx_samples_per_packet_func(TRXState *s1)
{
    TRXLmsState *s = (TRXLmsState*)s1->opaque;

    return SAMPLES_PER_PACKET/s->tx_channel_count;
}

static int trx_lms7002m_start(TRXState *s1, const TRXDriverParams *p)
{
    TRXLmsState *s = (TRXLmsState*)s1->opaque;
    int calibrate=1;

    if (p->rf_port_count != 1) {
        fprintf(stderr, "Only one port allowed\n");
        return -1;
    }

    LMS_EnableCalibCache(s->device,false);
    if (s->calibration) {
        if (!strcasecmp(s->calibration, "force")) {
            printf("Force calibration\n");
        } else if (!strcasecmp(s->calibration, "none")) {
            printf("Skip calibration\n");
            calibrate = 0;
        }
    }

    s->sample_rate = p->sample_rate[0].num / p->sample_rate[0].den;
    s->tx_channel_count = p->tx_channel_count;
    s->rx_channel_count = p->rx_channel_count;
    double refCLK;
    printf ("CH RX %d; TX %d\n",s->rx_channel_count,s->tx_channel_count);

    for(int ch=0; ch< s->rx_channel_count; ++ch)
    {
	    printf ("setup RX stream %d\n",ch);
	    s->rx_stream[ch].channel = ch;
	    s->rx_stream[ch].fifoSize = 128*1024;
	    s->rx_stream[ch].throughputVsLatency = 0.3;
	    s->rx_stream[ch].dataFmt = lms_stream_t::LMS_FMT_F32;
	    s->rx_stream[ch].isTx = false;
    	    LMS_SetupStream(s->device, &s->rx_stream[ch]);

    }

    for(int ch=0; ch< s->tx_channel_count; ++ch)
    {
	    printf ("setup TX stream %d\n",ch);
	    s->tx_stream[ch].channel = ch;
	    s->tx_stream[ch].fifoSize = 128*1024;
	    s->tx_stream[ch].throughputVsLatency = 0.3;
	    s->tx_stream[ch].dataFmt = lms_stream_t::LMS_FMT_F32;
	    s->tx_stream[ch].isTx = true;
	    LMS_SetupStream(s->device, &s->tx_stream[ch]);
    }

    printf("SR:   %.3f MHz\n", (float)s->sample_rate / 1e6);
    printf("DEC/INT: %d\n", s->dec_inter);

    if (LMS_SetSampleRate(s->device,s->sample_rate,s->dec_inter)!=0)
    {
        fprintf(stderr, "Failed to set sample rate %s\n",LMS_GetLastErrorMessage());
        return -1;
    }

    if (LMS_SetLOFrequency(s->device,LMS_CH_RX, 0, (double)p->rx_freq[0])!=0)
    {
        fprintf(stderr, "Failed to Set Rx frequency: %s\n", LMS_GetLastErrorMessage());
        return -1;
    }

    if (LMS_SetLOFrequency(s->device,LMS_CH_TX, 0,(double)p->tx_freq[0])!=0)
    {
        fprintf(stderr, "Failed to Set Tx frequency: %s\n", LMS_GetLastErrorMessage());
        return -1;
    }

    if (s->rx_channel_count > 2)
    {
	    if (LMS_SetLOFrequency(s->device,LMS_CH_RX, 2, (double)p->rx_freq[0])!=0)
	    {
		fprintf(stderr, "Failed to Set Rx frequency: %s\n", LMS_GetLastErrorMessage());
		return -1;
	    }
     }

    if (s->tx_channel_count > 2)
    {
	    if (LMS_SetLOFrequency(s->device,LMS_CH_TX, 2,(double)p->tx_freq[0])!=0)
	    {
		fprintf(stderr, "Failed to Set Tx frequency: %s\n", LMS_GetLastErrorMessage());
		return -1;
	    }
    }

    if (calibrate)
    {
        for(int ch=0; ch< s->tx_channel_count; ++ch) {
            printf("Calibrating Tx channel :%i\n", ch+1);
            if (LMS_Calibrate(s->device, LMS_CH_TX, ch,(double)p->tx_bandwidth[0],0)!=0)
            {
                fprintf(stderr, "Failed to calibrate Tx: %s\n", LMS_GetLastErrorMessage());
                return -1;
            }
        }

        for(int ch=0; ch< s->rx_channel_count; ++ch) {
            printf("Calibrating Rx channel :%i\n", ch+1);
            // Receiver calibration
            if (LMS_Calibrate(s->device, LMS_CH_RX, ch,(double)p->tx_bandwidth[0],0)!=0)
            {
                fprintf(stderr, "Failed to calibrate Rx: %s\n", LMS_GetLastErrorMessage());
                return -1;
            }
        }
    }

    fprintf(stderr, "Running\n");
    system("touch /dev/shm/LMSStreamingActive");
    return 0;
}

/* Driver initialization called at eNB startup */
int trx_driver_init(TRXState *s1)
{
    double val;
    char *configFile, *configFile1;
    int lms7002_index;
    int stream_index;
    TRXLmsState *s;
    lms_info_str_t list[16]={0};

    if (s1->trx_api_version != TRX_API_VERSION) {
        fprintf(stderr, "ABI compatibility mismatch between LTEENB and TRX driver (LTEENB ABI version=%d, TRX driver ABI version=%d)\n",
                s1->trx_api_version, TRX_API_VERSION);
        return -1;
    }

    s = (TRXLmsState*)malloc(sizeof(TRXLmsState));
    memset(s, 0, sizeof(*s));

    /* Few parameters */
    s->sample_rate = 0;
    if (trx_get_param_double(s1, &val, "sample_rate") >= 0)
        s->sample_rate = val*1e6;

    s->dec_inter = 2;
    if (trx_get_param_double(s1, &val, "dec_inter") >= 0)
        s->dec_inter = val;

    configFile = trx_get_param_string(s1, "config_file");
    if (!configFile) {
        fprintf(stderr, "Config file is mandatory to configure LMS7002\n");
        return -1;
    }

    /* Get device index */
    lms7002_index = 0;
    if (trx_get_param_double(s1, &val, "lms7002_index") >= 0)
        lms7002_index = val;

    /* Get device index */
    stream_index = -1;
    if (trx_get_param_double(s1, &val, "streamboard_index") >= 0)
        stream_index = val;

    // Open LMS7002 port
    int n= LMS_GetDeviceList(list);

    if (n <= lms7002_index || lms7002_index < 0) {
        fprintf(stderr, "No LMS7002 board found: %d\n", n);
        return -1;
    }

    if (n <= stream_index) {
        fprintf(stderr, "No Stream board found: %d\n", n);
        return -1;
    }

    if (LMS_Open(&(s->device),list[lms7002_index],stream_index>=0?list[stream_index]:nullptr)!=0) {
        fprintf(stderr, "Can't open lms port\n");
        return -1;
    }

    s->tcxo_calc = -1;
    if (trx_get_param_double(s1, &val, "tcxo_calc") >= 0)
    {
        s->tcxo_calc = val;
        LMS_VCTCXOWrite(s->device,val);
    }

    if ( LMS_Init(s->device)!=0)
    {
        fprintf(stderr, "LMS Init failed: %s\n",LMS_GetLastErrorMessage());
        return -1;
    }

    configFile1 = (char*)malloc(strlen(s1->path) + strlen(configFile) + 2);
    sprintf(configFile1, "%s/%s", s1->path, configFile);

    fprintf(stderr, "Config file: %s\n", configFile1);
    if  (LMS_LoadConfig(s->device,configFile1)!=0) //load registers configuration from file
    {
        fprintf(stderr, "Can't open %s\n", configFile1);
        return -1;
    }
    free(configFile);
    free(configFile1);

    /* Auto calibration */
    s->calibration = trx_get_param_string(s1, "calibration");

    /* Set callbacks */
    s1->opaque = s;
    s1->trx_end_func = trx_lms7002m_end;
    s1->trx_write_func = trx_lms7002m_write;
    s1->trx_read_func = trx_lms7002m_read;
    s1->trx_start_func = trx_lms7002m_start;
    s1->trx_get_sample_rate_func = trx_lms7002m_get_sample_rate;
    s1->trx_get_tx_samples_per_packet_func = trx_lms7002m_get_tx_samples_per_packet_func;
    return 0;
}
