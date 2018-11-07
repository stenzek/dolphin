#import "MetalView.h"
#include <cstdio>

@implementation MetalView

- (id)initWithFrame:(NSRect)frame {
  self = [super initWithFrame:frame];
  return self;
}

- (void)dealloc {
  [super dealloc];
}

/** Indicates that the view wants to draw using the backing layer instead of using drawRect:.  */
//-(BOOL) wantsUpdateLayer { return YES; }
//-(BOOL) wantsLayer { return YES; }

/** Returns a Metal-compatible layer. */
+(Class) layerClass { return [CAMetalLayer class]; }

/** If the wantsLayer property is set to YES, this method will be invoked to return a layer instance. */
-(CALayer*) makeBackingLayer {
  CALayer* layer = [self.class.layerClass layer];
  CGSize viewScale = [self convertSizeToBacking: CGSizeMake(1.0, 1.0)];
  layer.contentsScale = MIN(viewScale.width, viewScale.height);
  return layer;
}

- (void)drawRect:(NSRect)rect {
    //[[NSColor clearColor] set];
    NSRectFill(rect);
  //NSEraseRect(rect);
}

@end

void* CreateMetalView(void* parent, void** layer)
{
  NSView* parentView = (NSView*)parent;
  parentView.needsDisplay = YES;

  MetalView* metalView = [[MetalView alloc] initWithFrame: [parentView frame]];
  metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

  NSArray* subviews = [parentView subviews];
  std::printf("sb count: %d\n", [subviews count]);
  if ([subviews count] > 0)
    [parentView addSubview:metalView positioned:NSWindowAbove relativeTo:subviews[0]];
  else
    [parentView addSubview:metalView];

  [metalView setWantsLayer: YES];
  //[metalView setLayer:[metalView makeBackingLayer]];
  metalView.needsDisplay = YES;
  *layer = [metalView layer];
    [parentView display];

  return metalView;
}

void DestroyMetalView(void* parent)
{
}

 
