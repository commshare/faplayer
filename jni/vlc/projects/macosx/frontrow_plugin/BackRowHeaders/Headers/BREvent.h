/*
 *     Generated by class-dump 3.1.1.
 *
 *     class-dump is Copyright (C) 1997-1998, 2000-2001, 2004-2006 by Steve Nygard.
 */

#import <Foundation/Foundation.h>

typedef enum {
    BREventMenuUsage	= 134,
    BREventOKUsage		= 137,
    BREventRightUsage	= 138,
    BREventLeftUsage	= 139,
    BREventUpUsage		= 140,
    BREventDownUsage	= 141
} BREventUsage;

typedef enum {
    BREventUpValue		= 0,
    BREventDownValue	= 1
} BREventValue;

@interface BREvent : NSObject
{
    int _action;
    unsigned short _page;
    unsigned short _usage;
    int _value;
    BOOL _retrigger;
    double _timeStamp;
}

- (id)initWithPage:(unsigned short)fp8 usage:(unsigned short)fp12 value:(int)fp16;
- (id)initWithPage:(unsigned short)fp8 usage:(unsigned short)fp12 value:(int)fp16 atTime:(double)fp20;
- (id)description;
- (BOOL)isEqual:(id)fp8;
- (int)remoteAction;
- (unsigned int)pageUsageHash;
- (unsigned short)page;
- (BREventUsage)usage;
- (BREventValue)value;
- (BOOL)retriggerEvent;
- (void)makeRetriggerEvent;
- (double)timeStamp;

@end

