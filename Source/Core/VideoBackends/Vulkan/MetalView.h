#pragma once

#import <Cocoa/Cocoa.h>
#include <AppKit/AppKit.h>
#include <Metal/Metal.h>
#include <QuartzCore/CAMetalLayer.h>

@interface MetalView : NSView {
}

- (id)initWithFrame:(NSRect)frame;

@end

void* CreateMetalView(NSView* parent);
void DestroyMetalView(void* view);

