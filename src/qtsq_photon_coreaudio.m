/*
 * Quantum Tensor Sequence — Photon Core Audio Platform Shim (macOS)
 *
 * Bridges Apple Core Audio, CoreWLAN, and CoreBluetooth into the
 * Photon sensing engine's C-native pipeline.
 *
 * Responsibilities:
 *   1. Multi-mic capture via AVAudioEngine (MacBook Pro has 3 mics)
 *   2. Speaker chirp emission via AVAudioPlayerNode
 *   3. Full-duplex I/O (simultaneous capture + playback)
 *   4. WiFi AP scanning via CoreWLAN
 *   5. BLE device scanning via CoreBluetooth
 *   6. Thread-safe frame buffer for the sensing loop
 *
 * Author: Priyanshu Boruah / QTSQ Project
 */

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <CoreAudio/CoreAudio.h>
#import <AudioUnit/AudioUnit.h>

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include "qtsq_photon.h"

/* ═══════════════════════════════════════════════════════════════
 *  Constants
 * ═══════════════════════════════════════════════════════════════ */

#define FRAME_RING_SIZE     4        /* Ring buffer depth (power of 2) */
#define MAX_FRAME_SAMPLES   8192     /* Max samples per frame capture */

/* ═══════════════════════════════════════════════════════════════
 *  Internal Frame Ring Buffer
 *
 *  Lock-free producer-consumer ring for audio frames.
 *  The audio render callback writes frames, the sensing loop reads them.
 * ═══════════════════════════════════════════════════════════════ */

typedef struct {
    float    data[PHOTON_MAX_MICS][MAX_FRAME_SAMPLES];
    uint32_t num_channels;
    uint32_t num_samples;
    uint64_t timestamp_us;
    int      valid;
} photon_ring_frame_t;

static photon_ring_frame_t  g_frame_ring[FRAME_RING_SIZE];
static volatile uint32_t    g_write_idx = 0;
static volatile uint32_t    g_read_idx  = 0;
static pthread_mutex_t      g_ring_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ═══════════════════════════════════════════════════════════════
 *  BLE Scanner Delegate
 * ═══════════════════════════════════════════════════════════════ */

@interface PhotonBLEScanner : NSObject <CBCentralManagerDelegate>
@property (nonatomic, strong) CBCentralManager *centralManager;
@property (nonatomic, strong) NSMutableArray<NSDictionary *> *discoveredDevices;
@property (nonatomic, assign) BOOL isScanning;
@end

@implementation PhotonBLEScanner

- (instancetype)init {
    self = [super init];
    if (self) {
        _discoveredDevices = [NSMutableArray new];
        _isScanning = NO;
        /* Initialize on a background queue to avoid blocking main thread */
        dispatch_queue_t bleQueue = dispatch_queue_create("com.qtsq.photon.ble", DISPATCH_QUEUE_SERIAL);
        _centralManager = [[CBCentralManager alloc] initWithDelegate:self queue:bleQueue];
    }
    return self;
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    if (central.state == CBManagerStatePoweredOn && self.isScanning) {
        [central scanForPeripheralsWithServices:nil options:@{
            CBCentralManagerScanOptionAllowDuplicatesKey: @YES
        }];
    }
}

- (void)centralManager:(CBCentralManager *)central
  didDiscoverPeripheral:(CBPeripheral *)peripheral
      advertisementData:(NSDictionary<NSString *,id> *)advertisementData
                   RSSI:(NSNumber *)RSSI {
    @synchronized (self.discoveredDevices) {
        /* Keep only the latest 64 observations */
        if (self.discoveredDevices.count >= 64) {
            [self.discoveredDevices removeObjectAtIndex:0];
        }
        [self.discoveredDevices addObject:@{
            @"name": peripheral.name ?: peripheral.identifier.UUIDString,
            @"rssi": RSSI,
            @"uuid": peripheral.identifier.UUIDString,
            @"time": @([[NSDate date] timeIntervalSince1970] * 1e6)
        }];
    }
}

- (void)startScanning {
    self.isScanning = YES;
    if (self.centralManager.state == CBManagerStatePoweredOn) {
        [self.centralManager scanForPeripheralsWithServices:nil options:@{
            CBCentralManagerScanOptionAllowDuplicatesKey: @YES
        }];
    }
}

- (void)stopScanning {
    self.isScanning = NO;
    [self.centralManager stopScan];
    @synchronized (self.discoveredDevices) {
        [self.discoveredDevices removeAllObjects];
    }
}

@end

/* ═══════════════════════════════════════════════════════════════
 *  Global State
 * ═══════════════════════════════════════════════════════════════ */

static AVAudioEngine       *g_audio_engine    = nil;
static AVAudioPlayerNode   *g_player_node     = nil;
static PhotonBLEScanner    *g_ble_scanner     = nil;
static photon_config_t      g_config;
static BOOL                 g_is_running      = NO;
static uint64_t             g_start_time_us   = 0;
static AudioDeviceID        g_aggregate_id    = kAudioDeviceUnknown;
static uint32_t             g_input_channels  = 0;

/* ═══════════════════════════════════════════════════════════════
 *  Multi-Mic: HAL Aggregate Device Helpers
 *
 *  TDOA needs ≥ 2 input channels; most built-in Mac inputs are mono.
 *  When the default input is mono and ≥ 2 input devices exist, build
 *  an aggregate combining them and point the engine's input unit at
 *  it. Every failure path falls back to the default input unchanged.
 * ═══════════════════════════════════════════════════════════════ */

static int device_input_channel_count(AudioDeviceID dev) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamConfiguration,
        kAudioObjectPropertyScopeInput,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, NULL, &size) != noErr || size == 0) return 0;
    AudioBufferList *list = (AudioBufferList *)malloc(size);
    if (!list) return 0;
    int channels = 0;
    if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, list) == noErr) {
        for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
            channels += (int)list->mBuffers[i].mNumberChannels;
        }
    }
    free(list);
    return channels;
}

static int find_input_devices(AudioDeviceID *out, int max_out) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr) return 0;
    int total = (int)(size / sizeof(AudioDeviceID));
    if (total <= 0) return 0;
    AudioDeviceID *all = (AudioDeviceID *)malloc(size);
    if (!all) return 0;
    int found = 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, all) == noErr) {
        for (int i = 0; i < total && found < max_out; i++) {
            if (device_input_channel_count(all[i]) > 0) {
                out[found++] = all[i];
            }
        }
    }
    free(all);
    return found;
}

static BOOL create_photon_aggregate(const AudioDeviceID *inputs, int n_inputs, AudioDeviceID *out_id) {
    NSMutableArray<NSString *> *subs = [NSMutableArray new];
    for (int i = 0; i < n_inputs; i++) {
        CFStringRef uid = NULL;
        UInt32 uidSize = (UInt32)sizeof(uid);
        AudioObjectPropertyAddress addr = {
            kAudioDevicePropertyDeviceUID,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        if (AudioObjectGetPropertyData(inputs[i], &addr, 0, NULL, &uidSize, &uid) == noErr && uid != NULL) {
            [subs addObject:(__bridge NSString *)uid];
        }
    }
    if (subs.count < 2) return NO;

    /* Unique UID per run — the HAL rejects aggregates whose UID already exists.
     * The kAudioAggregateDevice*Key constants are C-string macros on newer
     * SDKs, so they must be lifted into NSString via the @-macro form. */
    NSString *aggUID = [NSString stringWithFormat:@"com.qtsq.photon.aggregate.%u", arc4random()];
    NSDictionary *desc = @{
        @kAudioAggregateDeviceNameKey: @"QTSQ Photon Aggregate",
        @kAudioAggregateDeviceUIDKey: aggUID,
        @kAudioAggregateDeviceMainSubDeviceKey: subs[0],
        @kAudioAggregateDeviceSubDeviceListKey: subs,
        @kAudioAggregateDeviceIsStackedKey: @NO
    };

    AudioDeviceID agg = kAudioDeviceUnknown;
    OSStatus st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)desc, &agg);
    if (st != noErr || agg == kAudioDeviceUnknown) return NO;

    /* Wait briefly for the HAL to finish configuring the new device. */
    for (int spin = 0; spin < 50; spin++) {
        if (device_input_channel_count(agg) >= 2) {
            *out_id = agg;
            return YES;
        }
        struct timespec ts = { 0, 50 * 1000 * 1000 };  /* 50 ms */
        nanosleep(&ts, NULL);
    }
    AudioHardwareDestroyAggregateDevice(agg);
    return NO;
}

/* ═══════════════════════════════════════════════════════════════
 *  Utility: Get current time in microseconds
 * ═══════════════════════════════════════════════════════════════ */

static uint64_t get_time_us(void) {
    return (uint64_t)([[NSDate date] timeIntervalSince1970] * 1e6);
}

/* ═══════════════════════════════════════════════════════════════
 *  Start Core Audio Engine
 *
 *  Sets up AVAudioEngine with:
 *   - Input tap on the input node (multi-mic capture)
 *   - Player node attached for chirp emission
 *   - Full-duplex mode
 * ═══════════════════════════════════════════════════════════════ */

int photon_coreaudio_start(const photon_config_t *cfg) {
    if (g_is_running) return 0;  /* Already started */
    if (!cfg) return -1;

    g_config = *cfg;
    g_start_time_us = get_time_us();

    /* Reset ring buffer */
    pthread_mutex_lock(&g_ring_mutex);
    g_write_idx = 0;
    g_read_idx  = 0;
    for (int i = 0; i < FRAME_RING_SIZE; i++) {
        g_frame_ring[i].valid = 0;
    }
    pthread_mutex_unlock(&g_ring_mutex);

    @autoreleasepool {
        /* Create audio engine */
        g_audio_engine = [[AVAudioEngine alloc] init];
        g_player_node  = [[AVAudioPlayerNode alloc] init];

        [g_audio_engine attachNode:g_player_node];

        /* Get input node format (this is the hardware mic format) */
        AVAudioInputNode *inputNode = [g_audio_engine inputNode];
        AVAudioFormat *hwFormat = [inputNode inputFormatForBus:0];

        NSLog(@"[Photon CoreAudio] Input format: %.0f Hz, %u channels",
              hwFormat.sampleRate, (unsigned)hwFormat.channelCount);

        /* Multi-mic upgrade: if the default input is mono, try to build an
         * aggregate of every input device so TDOA gets real stereo. Any
         * failure falls back to the default input exactly as before. */
        if (hwFormat.channelCount < 2) {
            AudioDeviceID inputs[16];
            int n_inputs = find_input_devices(inputs, 16);
            AudioDeviceID agg = kAudioDeviceUnknown;
            if (n_inputs >= 2 && create_photon_aggregate(inputs, n_inputs, &agg)) {
                AudioUnit inUnit = [inputNode audioUnit];
                BOOL switched = NO;
                if (inUnit != NULL) {
                    switched = (AudioUnitSetProperty(inUnit,
                                                     kAudioOutputUnitProperty_CurrentDevice,
                                                     kAudioUnitScope_Global, 0,
                                                     &agg, sizeof(agg)) == noErr);
                }
                int aggChans = device_input_channel_count(agg);
                if (switched && aggChans >= 2) {
                    g_aggregate_id = agg;
                    hwFormat = [[AVAudioFormat alloc]
                        initWithCommonFormat:AVAudioPCMFormatFloat32
                                    sampleRate:(double)cfg->sample_rate
                                     channels:(uint32_t)aggChans
                                  interleaved:NO];
                    NSLog(@"[Photon CoreAudio] 🎛 Aggregate multi-mic active: %d input devices → %d channels",
                          n_inputs, aggChans);
                } else {
                    AudioHardwareDestroyAggregateDevice(agg);
                    NSLog(@"[Photon CoreAudio] Aggregate device unusable — falling back to default mono input");
                }
            }
        }
        g_input_channels = (uint32_t)hwFormat.channelCount;

        /* Connect player node to main mixer for chirp emission */
        AVAudioFormat *playerFormat = [[AVAudioFormat alloc]
            initWithCommonFormat:AVAudioPCMFormatFloat32
            sampleRate:(double)cfg->sample_rate
            channels:1
            interleaved:NO];

        [g_audio_engine connect:g_player_node
                             to:[g_audio_engine mainMixerNode]
                         format:playerFormat];

        /* Install input tap for mic capture */
        uint32_t bufferSize = cfg->fft_size;  /* Capture in FFT-size chunks */

        AVAudioFormat *tapFormat = [[AVAudioFormat alloc]
            initWithCommonFormat:AVAudioPCMFormatFloat32
            sampleRate:(double)cfg->sample_rate
            channels:(uint32_t)hwFormat.channelCount
            interleaved:NO];

        [inputNode installTapOnBus:0
                        bufferSize:bufferSize
                            format:tapFormat
                             block:^(AVAudioPCMBuffer * _Nonnull buffer,
                                     AVAudioTime * _Nonnull when) {
            (void)when;  /* Suppress unused parameter warning */
            /* Audio render callback — runs on real-time thread */
            uint32_t frameCount = buffer.frameLength;
            uint32_t chanCount  = buffer.format.channelCount;
            float * const *channelData = buffer.floatChannelData;

            if (!channelData || frameCount == 0) return;

            /* Write into ring buffer */
            pthread_mutex_lock(&g_ring_mutex);

            uint32_t slot = g_write_idx & (FRAME_RING_SIZE - 1);
            photon_ring_frame_t *frame = &g_frame_ring[slot];

            uint32_t samplesToWrite = frameCount < MAX_FRAME_SAMPLES ? frameCount : MAX_FRAME_SAMPLES;
            uint32_t chansToWrite   = chanCount < PHOTON_MAX_MICS ? chanCount : PHOTON_MAX_MICS;

            for (uint32_t c = 0; c < chansToWrite; c++) {
                memcpy(frame->data[c], channelData[c], samplesToWrite * sizeof(float));
            }
            frame->num_channels = chansToWrite;
            frame->num_samples  = samplesToWrite;
            frame->timestamp_us = get_time_us();
            frame->valid = 1;

            g_write_idx++;

            pthread_mutex_unlock(&g_ring_mutex);
        }];

        /* Start the engine */
        NSError *error = nil;
        [g_audio_engine startAndReturnError:&error];
        if (error) {
            NSLog(@"[Photon CoreAudio] Engine start error: %@", error.localizedDescription);
            return -1;
        }

        /* Start BLE scanner */
        g_ble_scanner = [[PhotonBLEScanner alloc] init];
        [g_ble_scanner startScanning];

        g_is_running = YES;

        NSLog(@"[Photon CoreAudio] ✅ Engine started — %u Hz, %u mics, chirp: %u-%u Hz",
              cfg->sample_rate, (unsigned)hwFormat.channelCount,
              cfg->sonar_freq_lo, cfg->sonar_freq_hi);
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Get Latest Audio Frame
 *
 *  Called by the sensing loop (~10 Hz) to retrieve the latest
 *  captured multi-mic audio frame.
 *
 *  Returns 0 on success, -1 if no new frame is available.
 * ═══════════════════════════════════════════════════════════════ */

int photon_coreaudio_get_frame(photon_audio_frame_t *out_frame) {
    if (!out_frame || !g_is_running) return -1;

    pthread_mutex_lock(&g_ring_mutex);

    /* Check if there's unread data */
    if (g_read_idx >= g_write_idx) {
        pthread_mutex_unlock(&g_ring_mutex);
        return -1;  /* No new frame */
    }

    /* Read the latest frame (skip any buffered old frames) */
    g_read_idx = g_write_idx - 1;
    uint32_t slot = g_read_idx & (FRAME_RING_SIZE - 1);
    photon_ring_frame_t *ring = &g_frame_ring[slot];

    if (!ring->valid) {
        pthread_mutex_unlock(&g_ring_mutex);
        return -1;
    }

    /* Copy into caller's frame struct */
    out_frame->num_channels = ring->num_channels;
    out_frame->num_samples  = ring->num_samples;
    out_frame->timestamp_us = ring->timestamp_us;

    /* Allocate channel buffers (caller must free via photon_coreaudio_free_frame) */
    for (uint32_t c = 0; c < ring->num_channels; c++) {
        out_frame->channels[c] = (float *)malloc(ring->num_samples * sizeof(float));
        if (out_frame->channels[c]) {
            memcpy(out_frame->channels[c], ring->data[c], ring->num_samples * sizeof(float));
        }
    }
    /* Zero remaining channel pointers */
    for (uint32_t c = ring->num_channels; c < PHOTON_MAX_MICS; c++) {
        out_frame->channels[c] = NULL;
    }

    ring->valid = 0;
    g_read_idx++;

    pthread_mutex_unlock(&g_ring_mutex);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
 *  Free an allocated frame's channel buffers
 * ═══════════════════════════════════════════════════════════════ */

void photon_coreaudio_free_frame(photon_audio_frame_t *frame) {
    if (!frame) return;
    for (uint32_t c = 0; c < PHOTON_MAX_MICS; c++) {
        if (frame->channels[c]) {
            free(frame->channels[c]);
            frame->channels[c] = NULL;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Emit Chirp Through Speaker
 *
 *  Schedules the provided chirp waveform for playback through
 *  the device speaker. Used for ultrasonic sonar ranging.
 * ═══════════════════════════════════════════════════════════════ */

void photon_coreaudio_emit_chirp(const float *chirp, uint32_t num_samples) {
    if (!chirp || num_samples == 0 || !g_is_running || !g_player_node) return;

    @autoreleasepool {
        AVAudioFormat *format = [[AVAudioFormat alloc]
            initWithCommonFormat:AVAudioPCMFormatFloat32
            sampleRate:(double)g_config.sample_rate
            channels:1
            interleaved:NO];

        AVAudioPCMBuffer *buffer = [[AVAudioPCMBuffer alloc]
            initWithPCMFormat:format
            frameCapacity:num_samples];
        /* M14 fix: initWithPCMFormat can return nil on memory pressure.
         * Don't free(chirp) — the caller owns it (const float*). */
        if (!buffer) {
            return;
        }

        buffer.frameLength = num_samples;

        float *channelData = (float *)buffer.floatChannelData[0];
        memcpy(channelData, chirp, num_samples * sizeof(float));

        [g_player_node scheduleBuffer:buffer completionHandler:nil];
        [g_player_node play];
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  WiFi AP Scanning via CoreWLAN
 *
 *  CoreWLAN requires importing <CoreWLAN/CoreWLAN.h> which is
 *  macOS-only. We use dynamic loading to avoid hard linker deps
 *  on systems where CoreWLAN isn't available.
 * ═══════════════════════════════════════════════════════════════ */

int photon_coreaudio_scan_wifi(photon_rf_observation_t *out_obs, uint32_t max_obs) {
    if (!out_obs || max_obs == 0) return 0;

    @autoreleasepool {
        /* Dynamically load CWWiFiClient to avoid link-time CoreWLAN dependency */
        Class CWWiFiClientClass = NSClassFromString(@"CWWiFiClient");
        if (!CWWiFiClientClass) return 0;

        id client = [CWWiFiClientClass performSelector:@selector(sharedWiFiClient)];
        if (!client) return 0;

        id interface = [client performSelector:@selector(interface)];
        if (!interface) return 0;

        /* Scan for networks */
        NSError *error = nil;
        NSSet *networks = nil;

        SEL scanSel = NSSelectorFromString(@"scanForNetworksWithName:error:");
        if ([interface respondsToSelector:scanSel]) {
            /* Use NSInvocation for methods with multiple arguments */
            NSMethodSignature *sig = [interface methodSignatureForSelector:scanSel];
            NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
            [inv setTarget:interface];
            [inv setSelector:scanSel];
            id nilName = nil;
            [inv setArgument:&nilName atIndex:2];
            [inv setArgument:&error atIndex:3];
            [inv invoke];
            [inv getReturnValue:&networks];
        }

        if (error || !networks) return 0;

        int count = 0;
        for (id network in networks) {
            if ((uint32_t)count >= max_obs) break;

            NSString *ssid = [network performSelector:@selector(ssid)];
            NSNumber *rssi = [network performSelector:@selector(rssiValue)];

            if (ssid && rssi) {
                photon_rf_observation_t *obs = &out_obs[count];
                memset(obs, 0, sizeof(*obs));

                const char *ssidStr = [ssid UTF8String];
                strncpy(obs->identifier, ssidStr ? ssidStr : "unknown", 63);
                obs->identifier[63] = '\0';
                obs->rssi_dbm = [rssi floatValue];
                obs->timestamp_us = get_time_us();
                obs->type = PHOTON_RF_WIFI;

                count++;
            }
        }

        return count;
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  BLE Device Scanning
 *
 *  Returns the latest BLE observations collected by the scanner.
 * ═══════════════════════════════════════════════════════════════ */

int photon_coreaudio_scan_ble(photon_rf_observation_t *out_obs, uint32_t max_obs) {
    if (!out_obs || max_obs == 0 || !g_ble_scanner) return 0;

    int count = 0;

    @synchronized (g_ble_scanner.discoveredDevices) {
        for (NSDictionary *device in g_ble_scanner.discoveredDevices) {
            if ((uint32_t)count >= max_obs) break;

            photon_rf_observation_t *obs = &out_obs[count];
            memset(obs, 0, sizeof(*obs));

            NSString *name = device[@"name"];
            NSNumber *rssi = device[@"rssi"];
            NSNumber *time = device[@"time"];

            const char *nameStr = [name UTF8String];
            strncpy(obs->identifier, nameStr ? nameStr : "unknown", 63);
            obs->identifier[63] = '\0';
            obs->rssi_dbm = [rssi floatValue];
            obs->timestamp_us = [time unsignedLongLongValue];
            obs->type = PHOTON_RF_BLE;

            count++;
        }

        /* Clear after reading to avoid stale data */
        [g_ble_scanner.discoveredDevices removeAllObjects];
    }

    return count;
}

/* ═══════════════════════════════════════════════════════════════
 *  Stop Everything
 * ═══════════════════════════════════════════════════════════════ */

void photon_coreaudio_stop(void) {
    if (!g_is_running) return;

    @autoreleasepool {
        /* Stop BLE */
        if (g_ble_scanner) {
            [g_ble_scanner stopScanning];
            g_ble_scanner = nil;
        }

        /* Stop audio engine */
        if (g_audio_engine) {
            AVAudioInputNode *inputNode = [g_audio_engine inputNode];
            [inputNode removeTapOnBus:0];
            [g_player_node stop];
            [g_audio_engine stop];
            g_audio_engine = nil;
            g_player_node  = nil;
        }

        /* Tear down the multi-mic aggregate, if we built one */
        if (g_aggregate_id != kAudioDeviceUnknown) {
            AudioHardwareDestroyAggregateDevice(g_aggregate_id);
            g_aggregate_id = kAudioDeviceUnknown;
        }
        g_input_channels = 0;

        g_is_running = NO;

        NSLog(@"[Photon CoreAudio] ⏹ Engine stopped");
    }
}

/* ═══════════════════════════════════════════════════════════════
 *  Query State
 * ═══════════════════════════════════════════════════════════════ */

int photon_coreaudio_is_running(void) {
    return g_is_running ? 1 : 0;
}

uint32_t photon_coreaudio_get_mic_count(void) {
    if (!g_is_running) return 0;
    /* Prefer the channel count captured at start (reflects the aggregate
     * when multi-mic is active); query the node only as a fallback. */
    if (g_input_channels > 0) return g_input_channels;
    if (!g_audio_engine) return 0;
    @autoreleasepool {
        AVAudioInputNode *inputNode = [g_audio_engine inputNode];
        AVAudioFormat *hwFormat = [inputNode inputFormatForBus:0];
        return (uint32_t)hwFormat.channelCount;
    }
}

double photon_coreaudio_get_sample_rate(void) {
    if (!g_is_running || !g_audio_engine) return 0.0;
    @autoreleasepool {
        AVAudioInputNode *inputNode = [g_audio_engine inputNode];
        AVAudioFormat *hwFormat = [inputNode inputFormatForBus:0];
        return hwFormat.sampleRate;
    }
}
