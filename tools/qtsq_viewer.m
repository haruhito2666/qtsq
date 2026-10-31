/*
 * QTSQ Native Viewer — Custom Renderers
 * Built from scratch: Image, Text, Audio, Video, Hex renderers.
 * Uses CoreGraphics for pixel rendering, AudioToolbox for audio playback.
 * Compile: cc -ObjC -Iinclude tools/qtsq_viewer.m -L. -lqtsq -lm
 *          -framework Cocoa -framework AudioToolbox -o bin/QTSQViewer
 */

#import <Cocoa/Cocoa.h>
#import <AudioToolbox/AudioToolbox.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "qtsq.h"
#include "qtsq_vault.h"

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  GLOBALS
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static qtsq_context_t g_ctx;
static uint8_t *g_data      = NULL;
static size_t   g_data_size = 0;
static const char *g_filename = NULL;
static char g_filepath[1024] = {0};

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  COLOR PALETTE — Black Hole Theme
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static NSColor *C(uint32_t hex, CGFloat a) {
    return [NSColor colorWithRed:((hex>>16)&0xFF)/255.0
                           green:((hex>>8)&0xFF)/255.0
                            blue:(hex&0xFF)/255.0 alpha:a];
}
#define BG_DARK    C(0x0A0A0F, 1.0)
#define BG_CARD    C(0x12121A, 1.0)
#define BG_HEADER  C(0x0D0D1A, 1.0)
#define ACCENT     C(0xFF8C00, 1.0)
#define ACCENT_DIM C(0xFF8C00, 0.3)
#define TEXT_PRI   [NSColor whiteColor]
#define TEXT_SEC   C(0xC8C8C8, 1.0)
#define TEXT_DIM   C(0x888888, 1.0)
#define TEXT_MUTED C(0x555555, 1.0)
#define BORDER     C(0x222222, 1.0)
#define HEX_BLUE   C(0x8888FF, 1.0)
#define HEX_GREEN  C(0x88CC88, 1.0)
#define LINE_NUM   C(0x444444, 1.0)

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  DATA CLASSIFICATION
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
typedef enum { REND_IMAGE, REND_TEXT, REND_AUDIO, REND_VIDEO, REND_HEX } RendererType;

static int magic_is_image(void) {
    if (!g_data || g_data_size < 4) return 0;
    if (g_data[0]==0xFF && g_data[1]==0xD8 && g_data[2]==0xFF) return 1;
    if (g_data[0]==0x89 && g_data[1]=='P' && g_data[2]=='N' && g_data[3]=='G') return 1;
    if (g_data[0]=='G' && g_data[1]=='I' && g_data[2]=='F') return 1;
    if (g_data[0]=='B' && g_data[1]=='M') return 1;
    if (g_data_size>=12 && g_data[0]=='R' && g_data[1]=='I' && g_data[2]=='F' &&
        g_data[3]=='F' && g_data[8]=='W' && g_data[9]=='E') return 1;
    return 0;
}

static int is_text_type(uint8_t dt) {
    return dt==QTSQ_TYPE_TEXT_ASCII || dt==QTSQ_TYPE_TEXT_UTF8 ||
           dt==QTSQ_TYPE_JSON || dt==QTSQ_TYPE_CSV;
}

static int heuristic_is_text(void) {
    if (!g_data || g_data_size==0) return 0;
    size_t n = g_data_size < 512 ? g_data_size : 512;
    size_t p = 0;
    for (size_t i=0; i<n; i++)
        if ((g_data[i]>=32 && g_data[i]<127) || g_data[i]=='\n' ||
            g_data[i]=='\r' || g_data[i]=='\t') p++;
    return (p*100/n) > 85;
}

static RendererType classify(void) {
    uint8_t dt = g_ctx.header.data_type;
    if (dt==QTSQ_TYPE_IMAGE_GRAY || dt==QTSQ_TYPE_IMAGE_RGB ||
        dt==QTSQ_TYPE_IMAGE_RGBA || magic_is_image()) return REND_IMAGE;
    if (dt==QTSQ_TYPE_AUDIO_MONO || dt==QTSQ_TYPE_AUDIO_STEREO ||
        dt==QTSQ_TYPE_AUDIO_MULTI) return REND_AUDIO;
    if (dt==QTSQ_TYPE_VIDEO_FRAMES) return REND_VIDEO;
    if (is_text_type(dt) || heuristic_is_text()) return REND_TEXT;
    return REND_HEX;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  LOAD .qtsq FILE
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static int load_qtsq(const char *path) {
    qtsq_init(&g_ctx);
    int r = qtsq_read(&g_ctx, path);
    if (r != QTSQ_OK) return r;
    if (qtsq_is_encrypted(&g_ctx)) {
        /* H15 fix: prompt for password and decrypt instead of refusing to open. */
        NSAlert *alert = [NSAlert new];
        alert.messageText = @"Password Required";
        alert.informativeText = @"This .qtsq file is encrypted. Enter the password:";
        alert.alertStyle = NSAlertStyleInformational;
        NSSecureTextField *pwField = [[NSSecureTextField alloc]
            initWithFrame:NSMakeRect(0, 0, 300, 24)];
        pwField.placeholderString = @"Password";
        alert.accessoryView = pwField;
        alert.window.initialFirstResponder = pwField;
        [alert addButtonWithTitle:@"Decrypt"];
        [alert addButtonWithTitle:@"Cancel"];
        if ([alert runModal] != NSAlertFirstButtonReturn) return -99;
        NSString *pw = [pwField.stringValue
            stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if (pw.length == 0) return -99;
        r = qtsq_vault_decrypt_password(&g_ctx, pw.UTF8String);
        if (r != QTSQ_OK) {
            fprintf(stderr, "Decryption failed (wrong password?)\n");
            return r;
        }
    }
    r = qtsq_decompress(&g_ctx, &g_data, &g_data_size);
    return r;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  CUSTOM WAVEFORM VIEW — draws audio waveform
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
@interface WaveformView : NSView
@property int16_t *samples;
@property size_t   numSamples;
@property double   playhead;  /* 0.0 to 1.0 */
@end

@implementation WaveformView

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSRect b = self.bounds;

    /* Background */
    [C(0x0E0E16, 1.0) setFill];
    NSRectFill(b);

    if (!_samples || _numSamples == 0) return;

    /* Center line */
    [C(0x333333, 1.0) setStroke];
    NSBezierPath *center = [NSBezierPath bezierPath];
    [center moveToPoint:NSMakePoint(0, b.size.height/2)];
    [center lineToPoint:NSMakePoint(b.size.width, b.size.height/2)];
    [center stroke];

    /* Waveform */
    CGFloat midY = b.size.height / 2;
    size_t samplesPerPixel = _numSamples / (size_t)b.size.width;
    if (samplesPerPixel < 1) samplesPerPixel = 1;

    NSBezierPath *wave = [NSBezierPath bezierPath];
    [wave setLineWidth:1.0];

    for (CGFloat x = 0; x < b.size.width; x++) {
        size_t start = (size_t)x * _numSamples / (size_t)b.size.width;
        int16_t minVal = 0, maxVal = 0;
        for (size_t j = start; j < start + samplesPerPixel && j < _numSamples; j++) {
            if (_samples[j] < minVal) minVal = _samples[j];
            if (_samples[j] > maxVal) maxVal = _samples[j];
        }
        CGFloat yMin = midY + (minVal / 32768.0) * midY;
        CGFloat yMax = midY + (maxVal / 32768.0) * midY;
        [wave moveToPoint:NSMakePoint(x, yMin)];
        [wave lineToPoint:NSMakePoint(x, yMax)];
    }

    [ACCENT setStroke];
    [wave stroke];

    /* Playhead indicator */
    if (_playhead > 0.0) {
        CGFloat px = _playhead * b.size.width;
        [TEXT_PRI setStroke];
        NSBezierPath *ph = [NSBezierPath bezierPath];
        [ph setLineWidth:2.0];
        [ph moveToPoint:NSMakePoint(px, 0)];
        [ph lineToPoint:NSMakePoint(px, b.size.height)];
        [ph stroke];
    }
}

@end

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  CUSTOM PIXEL VIEW — renders raw pixel data via CGBitmapContext
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
@interface PixelView : NSView
@property NSImage *image;
@property CGFloat scaleMultiplier;
@property NSPoint panOffset;
@end

@implementation PixelView

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        _scaleMultiplier = 1.0;
        _panOffset = NSMakePoint(0, 0);
    }
    return self;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (!_image) return;
    NSRect b = self.bounds;
    NSSize imgSz = _image.size;
    CGFloat baseScale = fmin(b.size.width / imgSz.width, b.size.height / imgSz.height);
    CGFloat scale = baseScale * _scaleMultiplier;
    CGFloat w = imgSz.width * scale, h = imgSz.height * scale;
    CGFloat x = (b.size.width - w) / 2 + _panOffset.x;
    CGFloat y = (b.size.height - h) / 2 + _panOffset.y;
    
    // Nearest neighbor interpolation when zoomed in deeply
    if (_scaleMultiplier > 3.0) {
        [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationNone];
    } else {
        [[NSGraphicsContext currentContext] setImageInterpolation:NSImageInterpolationHigh];
    }

    [_image drawInRect:NSMakeRect(x, y, w, h)];
}

- (void)magnifyWithEvent:(NSEvent *)event {
    self.scaleMultiplier += event.magnification * self.scaleMultiplier;
    if (self.scaleMultiplier < 1.0) {
        self.scaleMultiplier = 1.0;
        self.panOffset = NSMakePoint(0, 0);
    } else if (self.scaleMultiplier > 50.0) {
        self.scaleMultiplier = 50.0;
    }
    [self setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent *)event {
    if (self.scaleMultiplier > 1.0) {
        NSPoint p = self.panOffset;
        p.x += event.scrollingDeltaX;
        p.y -= event.scrollingDeltaY;
        self.panOffset = p;
        [self setNeedsDisplay:YES];
    } else {
        [super scrollWheel:event];
    }
}
@end

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  CUSTOM IMAGE RENDERER — CGBitmapContext for raw pixels,
 *  NSImage for pre-compressed data (JPEG/PNG)
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static NSImage *render_image_from_data(void) {
    uint8_t dt = g_ctx.header.data_type;

    /* Pre-compressed: JPEG/PNG/GIF etc — use NSImage directly */
    if (magic_is_image()) {
        NSData *d = [NSData dataWithBytes:g_data length:g_data_size];
        return [[NSImage alloc] initWithData:d];
    }

    /* Raw pixel data from Accretion engine — build CGImage */
    uint32_t w = g_ctx.schema.dimensions[0];
    uint32_t h = g_ctx.schema.dimensions[1];
    uint8_t  ch = g_ctx.header.num_channels;
    if (w == 0 || h == 0) {
        /* Guess dimensions from data size */
        if (dt == QTSQ_TYPE_IMAGE_GRAY) { ch = 1; }
        else if (dt == QTSQ_TYPE_IMAGE_RGB) { ch = 3; }
        else if (dt == QTSQ_TYPE_IMAGE_RGBA) { ch = 4; }
        else { ch = ch > 0 ? ch : 3; }
        size_t pixels = g_data_size / ch;
        w = (uint32_t)sqrt((double)pixels);
        h = (uint32_t)(pixels / w);
        if (w == 0) w = 1;
        if (h == 0) h = 1;
    }
    if (ch == 0) {
        if (dt == QTSQ_TYPE_IMAGE_GRAY) ch = 1;
        else if (dt == QTSQ_TYPE_IMAGE_RGBA) ch = 4;
        else ch = 3;
    }

    /* Create RGBA buffer for CoreGraphics */
    size_t stride = w * 4;
    uint8_t *rgba = (uint8_t *)calloc(h * stride, 1);
    if (!rgba) return nil;

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            size_t srcIdx = (y * w + x) * ch;
            size_t dstIdx = y * stride + x * 4;
            if (srcIdx + ch > g_data_size) break;
            if (ch == 1) {
                rgba[dstIdx+0] = g_data[srcIdx];
                rgba[dstIdx+1] = g_data[srcIdx];
                rgba[dstIdx+2] = g_data[srcIdx];
            } else if (ch >= 3) {
                rgba[dstIdx+0] = g_data[srcIdx+0];
                rgba[dstIdx+1] = g_data[srcIdx+1];
                rgba[dstIdx+2] = g_data[srcIdx+2];
            }
            rgba[dstIdx+3] = (ch == 4 && srcIdx+3 < g_data_size) ?
                              g_data[srcIdx+3] : 255;
        }
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba, w, h, 8, stride,
        cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    if (!ctx) { free(rgba); return nil; }

    CGImageRef cgImg = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    free(rgba);
    if (!cgImg) return nil;

    NSImage *img = [[NSImage alloc] initWithCGImage:cgImg
                                               size:NSMakeSize(w, h)];
    CGImageRelease(cgImg);
    return img;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  CUSTOM AUDIO ENGINE — AudioQueue playback
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
#define AQ_BUFFERS     3
#define AQ_BUF_SAMPLES 4096

typedef struct {
    AudioQueueRef            queue;
    AudioQueueBufferRef      buffers[AQ_BUFFERS];
    AudioStreamBasicDescription fmt;
    int16_t *samples;
    size_t   numSamples;
    size_t   readPos;
    int      playing;
    WaveformView *waveView;
} AudioEngine;

static AudioEngine g_audio = {0};

static void audio_callback(void *userData, AudioQueueRef queue,
                           AudioQueueBufferRef buf) {
    AudioEngine *eng = (AudioEngine *)userData;
    size_t remain = eng->numSamples - eng->readPos;
    size_t toCopy = AQ_BUF_SAMPLES;
    if (toCopy > remain) toCopy = remain;

    if (toCopy == 0) {
        /* Loop back to start */
        eng->readPos = 0;
        remain = eng->numSamples;
        toCopy = AQ_BUF_SAMPLES;
        if (toCopy > remain) toCopy = remain;
    }

    memcpy(buf->mAudioData, eng->samples + eng->readPos,
           toCopy * sizeof(int16_t));
    buf->mAudioDataByteSize = (UInt32)(toCopy * sizeof(int16_t));
    eng->readPos += toCopy;

    AudioQueueEnqueueBuffer(queue, buf, 0, NULL);

    /* Update playhead */
    if (eng->waveView && eng->numSamples > 0) {
        eng->waveView.playhead = (double)eng->readPos / eng->numSamples;
        dispatch_async(dispatch_get_main_queue(), ^{
            [eng->waveView setNeedsDisplay:YES];
        });
    }
}

static void audio_init(int16_t *samples, size_t numSamples,
                       uint32_t sampleRate, uint8_t channels) {
    g_audio.samples = samples;
    g_audio.numSamples = numSamples;
    g_audio.readPos = 0;
    g_audio.playing = 0;

    if (sampleRate == 0) sampleRate = 44100;
    if (channels == 0) channels = 1;

    g_audio.fmt.mSampleRate = sampleRate;
    g_audio.fmt.mFormatID = kAudioFormatLinearPCM;
    g_audio.fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger |
                               kLinearPCMFormatFlagIsPacked;
    g_audio.fmt.mBitsPerChannel = 16;
    g_audio.fmt.mChannelsPerFrame = channels;
    g_audio.fmt.mBytesPerFrame = 2 * channels;
    g_audio.fmt.mFramesPerPacket = 1;
    g_audio.fmt.mBytesPerPacket = 2 * channels;

    AudioQueueNewOutput(&g_audio.fmt, audio_callback, &g_audio,
                        NULL, NULL, 0, &g_audio.queue);

    for (int i = 0; i < AQ_BUFFERS; i++) {
        AudioQueueAllocateBuffer(g_audio.queue,
            AQ_BUF_SAMPLES * sizeof(int16_t), &g_audio.buffers[i]);
        audio_callback(&g_audio, g_audio.queue, g_audio.buffers[i]);
    }
}

static void audio_play(void)  {
    AudioQueueStart(g_audio.queue, NULL);
    g_audio.playing = 1;
}
static void audio_pause(void) {
    AudioQueuePause(g_audio.queue);
    g_audio.playing = 0;
}
static void audio_stop(void)  {
    AudioQueueStop(g_audio.queue, true);
    g_audio.readPos = 0;
    g_audio.playing = 0;
    if (g_audio.waveView) {
        g_audio.waveView.playhead = 0;
        [g_audio.waveView setNeedsDisplay:YES];
    }
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  VIDEO FRAME VIEW — cycles through frames
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
@interface FrameView : NSView
@property uint8_t    *pixels;
@property uint32_t    width, height, channels, numFrames, currentFrame;
@property NSTimer    *timer;
@property NSTextField *frameLabel;
@end

@implementation FrameView

- (NSImage *)imageForFrame:(uint32_t)idx {
    if (!_pixels || idx >= _numFrames) return nil;
    size_t frameSize = (size_t)_width * _height * _channels;
    uint8_t *src = _pixels + idx * frameSize;
    size_t stride = _width * 4;
    uint8_t *rgba = (uint8_t *)calloc(_height * stride, 1);
    if (!rgba) return nil;

    for (uint32_t y = 0; y < _height; y++) {
        for (uint32_t x = 0; x < _width; x++) {
            size_t si = (y * _width + x) * _channels;
            size_t di = y * stride + x * 4;
            if (_channels == 1) {
                rgba[di]=rgba[di+1]=rgba[di+2] = src[si];
            } else if (_channels >= 3) {
                rgba[di] = src[si]; rgba[di+1] = src[si+1]; rgba[di+2] = src[si+2];
            }
            rgba[di+3] = (_channels==4) ? src[si+3] : 255;
        }
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(rgba, _width, _height, 8, stride,
        cs, kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);
    CGImageRef cg = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx); free(rgba);
    NSImage *img = [[NSImage alloc] initWithCGImage:cg
                                               size:NSMakeSize(_width,_height)];
    CGImageRelease(cg);
    return img;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    NSImage *img = [self imageForFrame:_currentFrame];
    if (!img) return;
    NSRect b = self.bounds;
    NSSize sz = img.size;
    CGFloat sc = fmin(b.size.width/sz.width, b.size.height/sz.height);
    CGFloat w=sz.width*sc, h=sz.height*sc;
    [img drawInRect:NSMakeRect((b.size.width-w)/2, (b.size.height-h)/2, w, h)];
}

- (void)tick:(NSTimer *)t {
    (void)t;
    _currentFrame = (_currentFrame + 1) % _numFrames;
    [self setNeedsDisplay:YES];
    if (_frameLabel) {
        _frameLabel.stringValue = [NSString stringWithFormat:
            @"Frame %u / %u", _currentFrame+1, _numFrames];
    }
}

- (void)togglePlay {
    if (_timer) { [_timer invalidate]; _timer = nil; }
    else {
        CGFloat fps = g_ctx.schema.frame_rate > 0 ? g_ctx.schema.frame_rate : 24;
        _timer = [NSTimer scheduledTimerWithTimeInterval:1.0/fps
            target:self selector:@selector(tick:) userInfo:nil repeats:YES];
    }
}

- (void)nextFrame {
    _currentFrame = (_currentFrame + 1) % _numFrames;
    [self setNeedsDisplay:YES];
    if (_frameLabel) _frameLabel.stringValue = [NSString stringWithFormat:
        @"Frame %u / %u", _currentFrame+1, _numFrames];
}

- (void)prevFrame {
    _currentFrame = (_currentFrame == 0) ? _numFrames-1 : _currentFrame-1;
    [self setNeedsDisplay:YES];
    if (_frameLabel) _frameLabel.stringValue = [NSString stringWithFormat:
        @"Frame %u / %u", _currentFrame+1, _numFrames];
}

@end

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  UI HELPER — create a styled label
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
static NSTextField *makeLabel(NSString *text, CGFloat sz, NSFontWeight wt,
                              NSColor *color, NSRect frame) {
    NSTextField *l = [NSTextField labelWithString:text];
    l.frame = frame;
    l.font = [NSFont systemFontOfSize:sz weight:wt];
    l.textColor = color;
    return l;
}

static NSButton *makeButton(NSString *title, NSRect frame, id target, SEL action) {
    NSButton *btn = [[NSButton alloc] initWithFrame:frame];
    btn.title = title;
    btn.bezelStyle = NSBezelStyleRounded;
    btn.target = target;
    btn.action = action;
    btn.wantsLayer = YES;
    btn.layer.backgroundColor = C(0x1A1A2A, 1.0).CGColor;
    btn.layer.cornerRadius = 6;
    btn.contentTintColor = ACCENT;
    return btn;
}

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  APP DELEGATE — Viewer window builder
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
@interface QTSQAppDelegate : NSObject <NSApplicationDelegate>
@property (strong) NSWindow *window;
@end

@implementation QTSQAppDelegate

- (void)playAudio:(id)sender   { (void)sender; audio_play(); }
- (void)pauseAudio:(id)sender  { (void)sender; audio_pause(); }
- (void)stopAudio:(id)sender   { (void)sender; audio_stop(); }

- (void)applicationDidFinishLaunching:(NSNotification *)notif {
    (void)notif;
    RendererType rtype = classify();
    CGFloat winW = 900, winH = 700;

    self.window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(200, 100, winW, winH)
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|
                  NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO];
    [self.window setTitle:[NSString stringWithFormat:@"QTSQ Viewer — %s",
        g_filename ? g_filename : ""]];
    [self.window setBackgroundColor:BG_DARK];
    [self.window setMinSize:NSMakeSize(600, 400)];

    NSView *root = self.window.contentView;
    CGFloat pad = 20, y = winH;

    /* ── HEADER ── */
    y -= 60;
    NSView *header = [[NSView alloc] initWithFrame:NSMakeRect(0, y, winW, 60)];
    header.wantsLayer = YES;
    header.layer.backgroundColor = BG_HEADER.CGColor;
    header.autoresizingMask = NSViewWidthSizable;

    /* Logo */
    NSView *logo = [[NSView alloc] initWithFrame:NSMakeRect(16, 8, 44, 44)];
    logo.wantsLayer = YES;
    logo.layer.cornerRadius = 11;
    logo.layer.backgroundColor = C(0xFF8C00, 0.15).CGColor;
    logo.layer.borderColor = C(0xFF8C00, 0.5).CGColor;
    logo.layer.borderWidth = 2;
    NSView *hole = [[NSView alloc] initWithFrame:NSMakeRect(10, 10, 24, 24)];
    hole.wantsLayer = YES;
    hole.layer.cornerRadius = 12;
    hole.layer.backgroundColor = [NSColor blackColor].CGColor;
    hole.layer.borderColor = C(0xFF8C00, 0.8).CGColor;
    hole.layer.borderWidth = 1;
    [logo addSubview:hole];
    [header addSubview:logo];

    [header addSubview:makeLabel(
        [NSString stringWithFormat:@"%s", g_filename ?: ""],
        17, NSFontWeightSemibold, TEXT_PRI, NSMakeRect(72, 28, 700, 22))];
    [header addSubview:makeLabel(@"QUANTUM TENSOR SEQUENCE",
        10, NSFontWeightMedium, ACCENT, NSMakeRect(72, 10, 400, 14))];
    [root addSubview:header];

    /* ── METADATA CARDS ── */
    y -= 10;
    NSArray *labs = @[@"DATA TYPE", @"STRATEGY", @"ORIGINAL SIZE",
                      @"COMPRESSED", @"RATIO", @"VERSION"];
    NSArray *vals = @[
        [NSString stringWithFormat:@"%s", qtsq_type_name(g_ctx.header.data_type)],
        [NSString stringWithFormat:@"%s", qtsq_strategy_name(g_ctx.header.strategy)],
        [NSString stringWithFormat:@"%llu B", (unsigned long long)g_ctx.header.original_size],
        [NSString stringWithFormat:@"%u B", g_ctx.header.singularity_size],
        [NSString stringWithFormat:@"%.1f:1", qtsq_compression_ratio(&g_ctx)],
        [NSString stringWithFormat:@"%u", g_ctx.header.version]];

    CGFloat cardW = (winW - pad*2 - 10) / 3;
    for (NSUInteger i = 0; i < 6; i++) {
        NSUInteger col = i % 3, row = i / 3;
        CGFloat cx = pad + col*(cardW+5);
        CGFloat cy = y - 52 - row*57;
        NSView *c = [[NSView alloc] initWithFrame:NSMakeRect(cx, cy, cardW, 50)];
        c.wantsLayer = YES;
        c.layer.backgroundColor = BG_CARD.CGColor;
        c.layer.cornerRadius = 7;
        c.layer.borderColor = BORDER.CGColor;
        c.layer.borderWidth = 1;
        [c addSubview:makeLabel(labs[i], 9, NSFontWeightMedium, TEXT_DIM,
            NSMakeRect(10, 28, cardW-20, 12))];
        NSColor *vc = (i==2||i==4) ? ACCENT : TEXT_PRI;
        [c addSubview:makeLabel(vals[i], 14, NSFontWeightMedium, vc,
            NSMakeRect(10, 6, cardW-20, 20))];
        [root addSubview:c];
    }
    y -= 52 + 57 + 10;

    /* ── CONTENT AREA ── */
    CGFloat contentH = y - 30;  /* leave room for footer */

    NSView *box = [[NSView alloc] initWithFrame:
        NSMakeRect(pad, 30, winW - pad*2, contentH)];
    box.wantsLayer = YES;
    box.layer.backgroundColor = BG_CARD.CGColor;
    box.layer.cornerRadius = 10;
    box.layer.borderColor = BORDER.CGColor;
    box.layer.borderWidth = 1;
    box.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

    CGFloat bW = winW - pad*2, bH = contentH;

    /* Content header bar */
    NSView *contentHdr = [[NSView alloc] initWithFrame:
        NSMakeRect(0, bH - 32, bW, 32)];
    contentHdr.wantsLayer = YES;
    contentHdr.layer.backgroundColor = C(0x0E0E16, 0.6).CGColor;

    NSString *typeStr, *badgeStr;
    switch (rtype) {
        case REND_IMAGE: typeStr=@"Image Preview";  badgeStr=@"IMAGE"; break;
        case REND_TEXT:  typeStr=@"Text Content";    badgeStr=@"TEXT"; break;
        case REND_AUDIO: typeStr=@"Audio Playback";  badgeStr=@"AUDIO"; break;
        case REND_VIDEO: typeStr=@"Video Player";    badgeStr=@"VIDEO"; break;
        case REND_HEX:   typeStr=@"Binary Data";     badgeStr=@"HEX"; break;
    }
    [contentHdr addSubview:makeLabel(typeStr, 12, NSFontWeightRegular, TEXT_DIM,
        NSMakeRect(12, 6, 300, 18))];
    [contentHdr addSubview:makeLabel(badgeStr, 10, NSFontWeightBold, ACCENT,
        NSMakeRect(bW - 80, 6, 70, 18))];
    [box addSubview:contentHdr];

    CGFloat bodyH = bH - 32;

    /* ══════ IMAGE RENDERER ══════ */
    if (rtype == REND_IMAGE) {
        NSImage *img = render_image_from_data();
        if (img) {
            PixelView *pv = [[PixelView alloc]
                initWithFrame:NSMakeRect(4, 4, bW-8, bodyH-8)];
            pv.image = img;
            pv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            [box addSubview:pv];
        }
    }
    /* ══════ TEXT RENDERER with line numbers ══════ */
    else if (rtype == REND_TEXT) {
        NSString *text = [[NSString alloc] initWithBytes:g_data
            length:g_data_size encoding:NSUTF8StringEncoding];
        if (!text) text = [[NSString alloc] initWithBytes:g_data
            length:g_data_size encoding:NSASCIIStringEncoding];
        if (!text) text = @"[decode error]";

        /* Build line-numbered text */
        NSArray *lines = [text componentsSeparatedByString:@"\n"];
        NSMutableAttributedString *numbered = [[NSMutableAttributedString alloc] init];
        NSFont *mono = [NSFont fontWithName:@"Menlo" size:12];
        NSDictionary *numAttrs = @{NSForegroundColorAttributeName: LINE_NUM,
                                   NSFontAttributeName: mono};
        NSDictionary *txtAttrs = @{NSForegroundColorAttributeName: TEXT_SEC,
                                   NSFontAttributeName: mono};

        for (NSUInteger i = 0; i < lines.count; i++) {
            NSString *num = [NSString stringWithFormat:@"%4lu │ ", (unsigned long)(i+1)];
            [numbered appendAttributedString:
                [[NSAttributedString alloc] initWithString:num attributes:numAttrs]];
            [numbered appendAttributedString:
                [[NSAttributedString alloc] initWithString:lines[i] attributes:txtAttrs]];
            if (i < lines.count - 1)
                [numbered appendAttributedString:
                    [[NSAttributedString alloc] initWithString:@"\n" attributes:txtAttrs]];
        }

        NSScrollView *scroll = [[NSScrollView alloc]
            initWithFrame:NSMakeRect(0, 0, bW, bodyH)];
        scroll.hasVerticalScroller = YES;
        scroll.drawsBackground = NO;
        scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        NSTextView *tv = [[NSTextView alloc]
            initWithFrame:NSMakeRect(0, 0, bW-20, bodyH)];
        [tv.textStorage setAttributedString:numbered];
        tv.editable = NO;
        tv.backgroundColor = [NSColor clearColor];
        tv.textContainerInset = NSMakeSize(12, 8);
        tv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        scroll.documentView = tv;
        [box addSubview:scroll];
    }
    /* ══════ AUDIO ENGINE ══════ */
    else if (rtype == REND_AUDIO) {
        int16_t *samples = (int16_t *)g_data;
        size_t numSamples = g_data_size / sizeof(int16_t);
        uint32_t sr = g_ctx.schema.sample_rate;
        uint8_t  ch = g_ctx.header.num_channels;
        if (sr == 0) sr = 44100;
        if (ch == 0) ch = 1;

        /* Waveform display */
        WaveformView *wv = [[WaveformView alloc]
            initWithFrame:NSMakeRect(4, 60, bW-8, bodyH-68)];
        wv.samples = samples;
        wv.numSamples = numSamples;
        wv.playhead = 0;
        wv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [box addSubview:wv];

        /* Audio info */
        double dur = (double)numSamples / (sr * ch);
        [box addSubview:makeLabel(
            [NSString stringWithFormat:@"%u Hz • %s • %.1f sec • %zu samples",
                sr, ch==1?"Mono":(ch==2?"Stereo":"Multi"), dur, numSamples],
            11, NSFontWeightRegular, TEXT_DIM, NSMakeRect(12, 36, bW-24, 16))];

        /* Controls */
        NSButton *playBtn = makeButton(@"▶ Play",
            NSMakeRect(12, 4, 80, 28), self, @selector(playAudio:));
        NSButton *pauseBtn = makeButton(@"⏸ Pause",
            NSMakeRect(100, 4, 90, 28), self, @selector(pauseAudio:));
        NSButton *stopBtn = makeButton(@"⏹ Stop",
            NSMakeRect(198, 4, 80, 28), self, @selector(stopAudio:));
        [box addSubview:playBtn];
        [box addSubview:pauseBtn];
        [box addSubview:stopBtn];

        /* Init audio engine */
        g_audio.waveView = wv;
        audio_init(samples, numSamples, sr, ch);
    }
    /* ══════ VIDEO RENDERER ══════ */
    else if (rtype == REND_VIDEO) {
        uint32_t w = g_ctx.schema.dimensions[0];
        uint32_t h = g_ctx.schema.dimensions[1];
        uint8_t  ch = g_ctx.header.num_channels;
        if (w==0) w=64; if (h==0) h=64; if (ch==0) ch=3;
        size_t frameSize = (size_t)w * h * ch;
        uint32_t nf = frameSize > 0 ? (uint32_t)(g_data_size / frameSize) : 1;
        if (nf == 0) nf = 1;

        FrameView *fv = [[FrameView alloc]
            initWithFrame:NSMakeRect(4, 40, bW-8, bodyH-48)];
        fv.pixels = g_data;
        fv.width = w; fv.height = h;
        fv.channels = ch; fv.numFrames = nf;
        fv.currentFrame = 0;
        fv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        NSTextField *frameLbl = makeLabel(
            [NSString stringWithFormat:@"Frame 1 / %u", nf],
            11, NSFontWeightRegular, TEXT_DIM, NSMakeRect(12, 8, 200, 16));
        fv.frameLabel = frameLbl;

        [box addSubview:fv];
        [box addSubview:frameLbl];

        NSButton *playBtn = makeButton(@"▶/⏸",
            NSMakeRect(220, 4, 60, 28), fv, @selector(togglePlay));
        NSButton *prevBtn = makeButton(@"◀",
            NSMakeRect(288, 4, 40, 28), fv, @selector(prevFrame));
        NSButton *nextBtn = makeButton(@"▶",
            NSMakeRect(336, 4, 40, 28), fv, @selector(nextFrame));
        [box addSubview:playBtn];
        [box addSubview:prevBtn];
        [box addSubview:nextBtn];
    }
    /* ══════ HEX VIEWER ══════ */
    else {
        NSMutableAttributedString *hex = [[NSMutableAttributedString alloc] init];
        NSFont *mono = [NSFont fontWithName:@"Menlo" size:11];
        NSDictionary *offAttr = @{NSForegroundColorAttributeName: ACCENT,
                                  NSFontAttributeName: mono};
        NSDictionary *byteAttr = @{NSForegroundColorAttributeName: HEX_BLUE,
                                   NSFontAttributeName: mono};
        NSDictionary *ascAttr = @{NSForegroundColorAttributeName: HEX_GREEN,
                                  NSFontAttributeName: mono};

        size_t rows = g_data_size / 16;
        if (rows > 128) rows = 128;
        for (size_t r = 0; r <= rows && r*16 < g_data_size; r++) {
            NSString *off = [NSString stringWithFormat:@"%08zx  ", r*16];
            [hex appendAttributedString:
                [[NSAttributedString alloc] initWithString:off attributes:offAttr]];
            NSMutableString *bytes = [NSMutableString new];
            NSMutableString *ascii = [NSMutableString new];
            for (int c = 0; c < 16; c++) {
                size_t idx = r*16 + c;
                if (idx < g_data_size)
                    [bytes appendFormat:@"%02x ", g_data[idx]];
                else
                    [bytes appendString:@"   "];
                if (c == 7) [bytes appendString:@" "];
                if (idx < g_data_size) {
                    char ch2 = (char)g_data[idx];
                    [ascii appendFormat:@"%c", (ch2>=32&&ch2<127)?ch2:'.'];
                }
            }
            [hex appendAttributedString:
                [[NSAttributedString alloc] initWithString:bytes attributes:byteAttr]];
            [hex appendAttributedString:
                [[NSAttributedString alloc] initWithString:
                    [NSString stringWithFormat:@" %@\n", ascii] attributes:ascAttr]];
        }
        if (g_data_size > (rows+1)*16) {
            NSString *more = [NSString stringWithFormat:
                @"\n... %zu more bytes ...\n", g_data_size-(rows+1)*16];
            [hex appendAttributedString:
                [[NSAttributedString alloc] initWithString:more
                    attributes:@{NSForegroundColorAttributeName:TEXT_MUTED,
                                 NSFontAttributeName:mono}]];
        }

        NSScrollView *sc = [[NSScrollView alloc] initWithFrame:
            NSMakeRect(0, 0, bW, bodyH)];
        sc.hasVerticalScroller = YES;
        sc.drawsBackground = NO;
        sc.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        NSTextView *tv = [[NSTextView alloc] initWithFrame:
            NSMakeRect(0, 0, bW-20, bodyH)];
        [tv.textStorage setAttributedString:hex];
        tv.editable = NO;
        tv.backgroundColor = [NSColor clearColor];
        tv.textContainerInset = NSMakeSize(12, 8);
        tv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        sc.documentView = tv;
        [box addSubview:sc];
    }

    [root addSubview:box];

    /* ── FOOTER ── */
    char cksum[65];
    for (int i=0; i<32; i++) sprintf(cksum+i*2, "%02x", g_ctx.header.checksum[i]);
    cksum[64] = '\0';
    NSTextField *cs = makeLabel([NSString stringWithFormat:@"SHA-256: %s", cksum],
        8, NSFontWeightRegular, TEXT_MUTED, NSMakeRect(pad, 10, bW, 12));
    cs.font = [NSFont fontWithName:@"Menlo" size:8];
    [root addSubview:cs];

    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)s {
    (void)s;
    if (g_audio.queue) {
        AudioQueueDispose(g_audio.queue, true);
        g_audio.queue = NULL;
    }
    return YES;
}

@end

/* ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
 *  MAIN
 * ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━ */
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        const char *fp = NULL;
        for (int i = 1; i < argc; i++) {
            size_t len = strlen(argv[i]);
            if (len > 5 && strcmp(argv[i]+len-5, ".qtsq") == 0) { fp = argv[i]; break; }
        }
        if (!fp) { fprintf(stderr, "Usage: QTSQViewer <file.qtsq>\n"); return 1; }

        strncpy(g_filepath, fp, sizeof(g_filepath)-1);
        g_filename = strrchr(g_filepath, '/');
        g_filename = g_filename ? g_filename+1 : g_filepath;

        int r = load_qtsq(g_filepath);
        if (r == -99) {
            fprintf(stderr, "🔒 Encrypted. Decrypt first:\n  qtsq decrypt '%s' -p <pass>\n", fp);
            return 1;
        }
        if (r != QTSQ_OK) { fprintf(stderr, "Error: %s\n", qtsq_error_string(r)); return 1; }

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        /* Menu bar */
        NSMenu *menuBar = [NSMenu new];
        NSMenuItem *appMenuItem = [NSMenuItem new];
        [menuBar addItem:appMenuItem];
        NSMenu *appMenu = [NSMenu new];
        [appMenu addItemWithTitle:@"Quit QTSQ Viewer"
            action:@selector(terminate:) keyEquivalent:@"q"];
        appMenuItem.submenu = appMenu;
        [app setMainMenu:menuBar];

        QTSQAppDelegate *del = [QTSQAppDelegate new];
        app.delegate = del;
        [app run];

        if (g_data) free(g_data);
        qtsq_free(&g_ctx);
    }
    return 0;
}
