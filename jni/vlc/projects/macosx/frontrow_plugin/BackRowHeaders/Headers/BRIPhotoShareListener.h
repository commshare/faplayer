/*
 *     Generated by class-dump 3.1.1.
 *
 *     class-dump is Copyright (C) 1997-1998, 2000-2001, 2004-2006 by Steve Nygard.
 */

#import "NSObject.h"

@class NSMutableArray, NSNetServiceBrowser, NSTimer;

@interface BRIPhotoShareListener : NSObject
{
    NSNetServiceBrowser *_browser;
    NSMutableArray *_servers;
    NSTimer *_statusTimer;
}

- (id)init;
- (void)dealloc;
- (void)startListening;
- (void)stopListening;
- (id)servers;
- (void)netServiceBrowser:(id)fp8 didNotSearch:(id)fp12;
- (void)netServiceBrowser:(id)fp8 didFindService:(id)fp12 moreComing:(BOOL)fp16;
- (void)netServiceBrowser:(id)fp8 didRemoveService:(id)fp12 moreComing:(BOOL)fp16;

@end

