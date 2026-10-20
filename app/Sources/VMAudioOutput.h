#import <Foundation/Foundation.h>
#import "VMAudioBuffer.h"

/* Main-thread AVAudioSession owner; lifecycle setters dispatch to main. */
@interface VMAudioOutput : NSObject
@property (nonatomic, readonly) vm_audio_buffer_t *buffer;
@property (nonatomic, readonly) BOOL microphoneEnabled;
@property (nonatomic, copy, readonly) NSString *status;
- (void)setRunning:(BOOL)running;
- (void)setGuestRecording:(BOOL)recording;
- (void)setMicrophoneEnabled:(BOOL)enabled completion:(void (^)(BOOL, NSString *))completion;
@end
