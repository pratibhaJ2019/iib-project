#include <ctime>
#include <chrono>
#include <thread>
#include <math.h>
#include <fstream>
#include <stdio.h>
#include <iostream>
#include <iomanip> 
#include "string.h"
#include "lime/LimeSuite.h"
#include "tranciever_setup.h"

using namespace std;
void tune_DAC(uint64_t freq);
void init_DAC(void);

const uint16_t init_dac_val = 3125;

// g++ main.cpp tranciever_setup.cpp -std=c++11 -lLimeSuite -o tuned-tx.out

/* Entry Point */
int main(int argc, char** argv){
    
    /* Hardware Config */
    tranciever_configuration config;
    config.rx_centre_frequency = 2.4e9;                 // RX Center Freuency    
    config.rx_antenna = LMS_PATH_LNAH;                  // RX RF Path = 2GHz - 3GHz
    config.rx_gain = 40;                                // RX Gain 0 to 73 dB
    config.enable_rx_LPF = false;                       // Disable RX Low Pass Filter
    config.rx_LPF_bandwidth = 10e6;                     // RX Analog Low Pass Filter Bandwidth
    config.enable_rx_cal = false;                       // Disable RX Calibration
    config.rx_cal_bandwidth = 25e6;                     // Automatic Calibration Bandwidth
    
    config.tx_centre_frequency = 2.4e9;                 // TX Center Freuency
    config.tx_antenna = LMS_PATH_TX1;                   // TX RF Path = 2GHz - 3GHz
    config.tx_gain = 50;                                // TX Gain 0 to 73 dB
    config.enable_tx_LPF = false;                       // Disable TX Low Pass Filter
    config.tx_LPF_bandwidth = 10e6;                     // TX Analog Low Pass Filter Bandwidth
    config.enable_tx_cal = false;                       // Disable TX Calibration
    config.tx_cal_bandwidth = 25e6;                     // Automatic Calibration Bandwidth
    
    config.sample_rate = 30.72e6;                       // Device Sample Rate 
    config.rf_oversample_ratio = 4;                     // ADC Oversample Ratio
    
    configure_tranciever(config);

    /* Share TX & RX PLL */
    LMS_WriteParam(device, LMS7_MAC, 2);
    LMS_WriteParam(device, LMS7_PD_LOCH_T2RBUF, 0);
    LMS_WriteParam(device, LMS7_MAC, 1);
    LMS_WriteParam(device, LMS7_PD_VCO, 1);

    /* VCTCXO */
    init_DAC();
    
    /* RX Stream Config  */
    lms_stream_t rx_stream;
    rx_stream.channel = 0;                              // Channel Number
    rx_stream.fifoSize = 1360 * 4096;                   // Fifo Size in Samples
    rx_stream.throughputVsLatency = 1.0;                // Optimize Throughput (1.0) or Latency (0)
    rx_stream.isTx = false;                             // TX/RX Channel
    rx_stream.dataFmt = lms_stream_t::LMS_FMT_I12;      // Data Format - 12-bit sample stored as int16_t
    LMS_SetupStream(device, &rx_stream);
    
    /* RX Data Buffer */
    const int num_rx_samples = 1360;
    const int rx_buffer_size = num_rx_samples * 2;
    int16_t rx_buffer[rx_buffer_size];
    
    /* RX Stream Metadata */
    lms_stream_meta_t rx_metadata;
     
    /* TX Stream Config */
    lms_stream_t tx_stream;
    tx_stream.channel = 0;                              // Channel Number
    tx_stream.fifoSize = 1360 * 4096;                   // Fifo Size in Samples
    tx_stream.throughputVsLatency = 0;                  // Optimize Throughput (1.0) or Latency (0)
    tx_stream.isTx = true;                              // TX/RX Channel
    tx_stream.dataFmt = lms_stream_t::LMS_FMT_I12;      // Data Format - 12-bit sample stored as int16_t
    LMS_SetupStream(device, &tx_stream);
    
    /* TX Data Buffer */
    const int waveform_samples = 1360 * 8;
    const int num_tx_samples = 1360 * (8+1);
    const int tx_buffer_size = num_tx_samples * 2;
    int16_t tx_buffer[tx_buffer_size];
    int16_t delayed_tx_buffer[tx_buffer_size];

    memset(tx_buffer, 0, tx_buffer_size*sizeof(int16_t));
    memset(delayed_tx_buffer, 0, tx_buffer_size*sizeof(int16_t));
    
    /* TX Stream Metadata */
    lms_stream_meta_t tx_metadata;
    tx_metadata.flushPartialPacket = true;             // Force sending of incomplete packets
    tx_metadata.waitForTimestamp = true;               // Enable synchronization to HW timestamp
     
    /* Read Waveform */
    ifstream wfm_file; 
    wfm_file.open("wfm.bin", ios::binary | ios::in);
    for(int i = 0; i < waveform_samples; i++) {
        wfm_file.read((char*)&tx_buffer[2*i], sizeof(int16_t));
        wfm_file.read((char*)&tx_buffer[(2*i)+1], sizeof(int16_t));
    }

    /* Generate TX Buffer Delayed 1 Sample - e.g. (0,0, tx_buffer, 0,0, ... 0,0) */
    memcpy(&delayed_tx_buffer[2], tx_buffer, waveform_samples * 2 * sizeof(int16_t));

    /* Output File */
    ofstream outfile;
    file_header file_metadata;
    int file_number = 1;
    const string out_path = "data/";

    /* Output Buffer */
    const int file_length = 50;
    int16_t file_buffer[rx_buffer_size * file_length];

    /* Book Keeping Indicies */
    uint64_t curr_buff_idx = 0;
    uint64_t pps_sync_idx = 0;
    uint64_t prev_pps_sync_idx = 0;

    uint64_t tx_schedule_event = 10e6;
    uint64_t tx_start_event = 0;
    uint64_t tx_capture_event = 10e6;
    uint64_t tx_stop_event = 10e6;
    
    bool gps_lock = false;
    bool tx_waiting = false;
    cout << "[GPS] Waiting for lock..." << endl;


    /* Start RX Stream */
    LMS_StartStream(&rx_stream);

    /* Process Stream for 45s */
    auto t1 = chrono::high_resolution_clock::now();
    auto t2 = t1;
    while (chrono::high_resolution_clock::now() - t1 < chrono::seconds(600)){

               
        /* Read Samples into Buffer */
        if(LMS_RecvStream(&rx_stream, rx_buffer, num_rx_samples, &rx_metadata, 1000) != num_rx_samples){
            LMS_StopStream(&rx_stream);
            LMS_DestroyStream(device, &rx_stream);
            error();
        };


        /* Check PPS Sync Flag - MSB Set */
        if((rx_metadata.timestamp & 0x8000000000000000) == 0x8000000000000000){
            
            /* Extract PPS Sync Index - Clear MSB */
            prev_pps_sync_idx = pps_sync_idx;
            pps_sync_idx = rx_metadata.timestamp ^ 0x8000000000000000;
            curr_buff_idx += num_rx_samples;
            
            /* Ignore Repeated Timestamp */
            if (pps_sync_idx != prev_pps_sync_idx){
                
                /* UNIQUE PPS EVENT DETCETED */

                /* Setup TX with GPS Lock */
                if(gps_lock){

                    if(!tx_waiting){
                        tx_schedule_event = curr_buff_idx + 1360 * 1500;         // Send TX samples in 1500 buffers time
                        tx_capture_event = curr_buff_idx + 1360 * (1800 - 15);   // Begin recording ~15 buffers prior to TX
                        tx_start_event = pps_sync_idx + 1360 * 1800;             // TX scheduled for 1800 x 1360 samples after PPS
                        tx_stop_event = curr_buff_idx + 1360 * (1800 + 20);      // Close TX stream ~15 buffers after start of TX
                        tx_waiting = true;
                    }
        
                    cout << "\n[RX]  Current buffer = " << curr_buff_idx << endl;
                    cout << "[RX]  PPS event occured at " << pps_sync_idx << endl;

                    tune_DAC(pps_sync_idx - prev_pps_sync_idx);
                }
            }
        } else {
            curr_buff_idx = rx_metadata.timestamp;
        }

        
        /* Infer GPS Lock Status */
        if((curr_buff_idx > pps_sync_idx + 40e6) && (gps_lock)){
            gps_lock = false;
            cout << "[GPS] GPS Lock Lost" << endl;
        } else if((pps_sync_idx > curr_buff_idx) && !(gps_lock)){
            gps_lock = true;
            cout << "[GPS] GPS Lock Aquired" << endl;
        }

        
        /* Setup TX with No GPS Lock */
        if(!gps_lock){

            uint64_t n = 0;
            n = ((curr_buff_idx - pps_sync_idx) / 30720000) + 1;

            /* Detect Buffer Containing Pseudo PPS */
            if(((curr_buff_idx - pps_sync_idx) > 30720000*n - 1360) && ((curr_buff_idx - pps_sync_idx) < (30720000*n))){
                
                /* Schedule TX as Before */
                if(!tx_waiting){
                    tx_schedule_event = curr_buff_idx + 1360 * 1500;            // Send TX samples in 1500 buffers time
                    tx_capture_event = curr_buff_idx + 1360 * (1800 - 15);      // Begin recording ~15 buffers prior to TX
                    tx_start_event = pps_sync_idx + 30720000*n + 1360 * 1800;   // TX scheduled for 1800 x 1360 samples after Pseudo PPS
                    tx_stop_event = curr_buff_idx + 1360 * (1800 + 20);         // Close TX stream ~15 buffers after start of TX
                    tx_waiting = true;
                }
        
                cout << "\n[HLD]  Current buffer = " << curr_buff_idx << endl;
                cout << "[HLD]  PPS event occured at " << pps_sync_idx << "     [" << n << "]" << endl;
            }
        }


        /* Schedule TX Event */
        if(curr_buff_idx == tx_schedule_event){
            
            /* Start TX Stream */
            LMS_StartStream(&tx_stream);

            /* Send TX Buffer - Handle Odd/Even TX Start Event */
            if(tx_start_event % 2 == 0){
                tx_metadata.timestamp = tx_start_event;
                if (LMS_SendStream(&tx_stream, tx_buffer, num_tx_samples, &tx_metadata, 1000) != num_tx_samples){
                    error();
                }
            } else {
                cout << "[TX]  Zero padding TX buffer" << endl;
                tx_metadata.timestamp = tx_start_event - 1;
                if (LMS_SendStream(&tx_stream, delayed_tx_buffer, num_tx_samples, &tx_metadata, 1000) != num_tx_samples){
                    error();
                }
            }                      
            cout << "[TX]  Buffer sent, TX scheduled for " << tx_metadata.timestamp << endl;
        }

        
        /* Capture TX Event */
        if(curr_buff_idx == tx_capture_event){
            
            /* Save Current RX Buffer */
            memcpy(file_buffer, rx_buffer, sizeof(rx_buffer));

            cout << "[TX]  Capturing at " << curr_buff_idx << endl;
            
            /* Generate File Metadata */
            file_metadata.unix_stamp = std::time(NULL);
            file_metadata.buffer_index = curr_buff_idx;
            file_metadata.pps_index = pps_sync_idx;

            /* Save Subsequent RX Buffers */
            for(int k=1; k<file_length; k++){
    
                LMS_RecvStream(&rx_stream, rx_buffer, num_rx_samples, &rx_metadata, 1000);
                memcpy(&file_buffer[k*rx_buffer_size], rx_buffer, sizeof(rx_buffer));
                curr_buff_idx += num_rx_samples;

                /* Stop TX Stream */
                if(curr_buff_idx == tx_stop_event){
                    LMS_StopStream(&tx_stream);
                    cout << "[TX]  TX stopped at " << curr_buff_idx << endl;
                }  
            }
            
            /* Write to File */
            outfile.open(out_path + to_string(file_number) + ".bin", std::ofstream::binary);
            outfile.write((char*)&file_metadata, sizeof(file_metadata));
            outfile.write((char*)file_buffer, sizeof(file_buffer));
            outfile.close();
            file_number += 1;

            /* Release TX */
            tx_waiting = false;
        }
    
    
    }

    /* Stop RX Stream */
    LMS_StopStream(&rx_stream);
    
    /* Destroy Stream */
    LMS_DestroyStream(device, &rx_stream);
    LMS_DestroyStream(device, &tx_stream);   
    
    /* Disable TX/RX Channels */
    if (LMS_EnableChannel(device, LMS_CH_TX, 0, false)!=0)
        error();
    if (LMS_EnableChannel(device, LMS_CH_RX, 0, false)!=0)
        error();
       
    /* Close Device */
    if (LMS_Close(device)==0)
        cout << "Device closed" << endl;
    return 0;
}



/* VCTCXO Tuning Algorithm */
void tune_DAC(uint64_t freq){

    /* DAC = 0 to 4095 -> 0v to 2.538v */

    const double target_freq = 30720000;
    const double freq_err = 0.15; 
    static bool dac_updated = false;
    static uint16_t dac_value = init_dac_val;   
    
    static int j = 0;
    static bool first_buff = true;
    static const int ma_len = 25;
    static uint64_t readings[ma_len];
    static double avg_freq = 0;


    /* Check if DAC Value Changed over Last Second */
    if(dac_updated){

        /* Zero Stats */
        first_buff = true;
        for (int f=0;f<ma_len;f++){
            readings[f] = 0;
        }
        j=0;
        dac_updated = false;

    } else {

        /* Calculate Moving Average */
        if(first_buff){
            readings[j] = freq;
            j++;
            avg_freq = 0;
            for(int k=0;k<j;k++){
                avg_freq += readings[k];
            }
            avg_freq = avg_freq/j;
            if(j==ma_len){
                first_buff = false;
            }
        } else {
            if(j==ma_len){
                j=0;
            }
            readings[j] = freq;
            j++;
            avg_freq = 0;
            for(int m=0;m<ma_len;m++){
                avg_freq += readings[m];
            }
            avg_freq = avg_freq/ma_len;
        }

        /* Display Frequency Stats */
        cout << "[VCO] Freq: " << freq << endl;
        // cout << "History: ";
        // for (int n=0;n<ma_len;n++){
        //     cout << readings[n] << " ";
        // }
        // cout << endl;
        cout << std::setprecision(12) << "[VCO] Avg: " << avg_freq << endl;


        /* Reduce Frequency */
        if(avg_freq > target_freq + freq_err){
            
            dac_value--;
            LMS_VCTCXOWrite(device, dac_value);
            LMS_VCTCXORead(device, &dac_value);
            cout << "[VCO] DAC = " << dac_value << endl;
            dac_updated = true;
        }

        /* Increase Frequency */
        if(avg_freq < target_freq - freq_err){
            
            dac_value++;
            LMS_VCTCXOWrite(device, dac_value);
            LMS_VCTCXORead(device, &dac_value);
            cout << "[VCO] DAC = " << dac_value << endl;
            dac_updated = true;
        }
    } 
}


/* Initilise 12-bit DAC */
void init_DAC(void){   
    
    uint16_t initial_dac_value = init_dac_val;   
    
    cout << endl << "[VCO] Writing " << initial_dac_value << " to DAC..." <<endl;
    LMS_VCTCXOWrite(device, initial_dac_value);
    
    LMS_VCTCXORead(device, &initial_dac_value);
    cout << "[VCO] DAC = " << initial_dac_value << endl;
}
