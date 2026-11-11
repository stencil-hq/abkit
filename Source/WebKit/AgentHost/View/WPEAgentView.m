/*
 * Copyright (C) 2026 Stencil
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// WPEAgentView puts a WPEAgentHost's blocks in windows (macOS). The host has no window of its own:
// a client that wants its pages on screen starts this program beside the host and presents blocks
// to it (Protocol.md, "Presentation" and "The viewer"). It is a presentation client like any other:
// it checks in a Mach service, takes the frames the host sends there and shows each block's in a
// window of its own. What the user does in a window is written to standard output as the host's
// `input` steps, for the client to send on; the viewer never talks to the host's standard streams.
//
// The window never takes the keyboard from the app the user is in: it comes to the front without
// becoming key, and takes keys once the user clicks it.

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>
#import <mach/mach.h>
#import <servers/bootstrap.h>

// Message kinds of the Mach channel (Protocol.md).
enum {
    MessageHello = 1,
    MessageBuffer = 2,
    MessageFrame = 3,
    MessageGone = 4,
    MessageBye = 5,
    MessageReleased = 16,
    MessageTick = 17,
};

// Where a complex message keeps its port and its words, and a simple one its words.
enum {
    ComplexPortOffset = 28,
    ComplexWordsOffset = 40,
    SimpleWordsOffset = 24,
};

// WebKit scrolls 40 px for a wheel line.
static const CGFloat pixelsPerWheelLine = 40;

static mach_port_t servicePort = MACH_PORT_NULL;
static mach_port_t hostPort = MACH_PORT_NULL;

// Writes one line of JSON to the client.
static void emit(NSDictionary *message)
{
    NSData *data = [NSJSONSerialization dataWithJSONObject:message options:0 error:nil];
    if (!data)
        return;
    fwrite(data.bytes, 1, data.length, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

// Sends the host a simple message of `count` words; a host that does not take it within the timeout is gone.
static void sendToHost(mach_msg_id_t kind, const uint32_t *words, size_t count)
{
    if (hostPort == MACH_PORT_NULL)
        return;
    struct {
        mach_msg_header_t header;
        uint32_t words[2];
    } message = { };
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    message.header.msgh_size = (mach_msg_size_t)(sizeof(mach_msg_header_t) + count * sizeof(uint32_t));
    message.header.msgh_remote_port = hostPort;
    message.header.msgh_id = kind;
    for (size_t index = 0; index < count; index++)
        message.words[index] = words[index];
    mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, message.header.msgh_size, 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
}

static NSArray<NSString *> *modifiersOf(NSEvent *event)
{
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    NSEventModifierFlags flags = event.modifierFlags;
    if (flags & NSEventModifierFlagShift)
        [names addObject:@"shift"];
    if (flags & NSEventModifierFlagControl)
        [names addObject:@"ctrl"];
    if (flags & NSEventModifierFlagOption)
        [names addObject:@"alt"];
    if (flags & NSEventModifierFlagCommand)
        [names addObject:@"meta"];
    return names;
}

// The DOM `key` of a key that types no character, or nil.
static NSString *namedKey(unsigned short keyCode)
{
    switch (keyCode) {
    case kVK_Return:
    case kVK_ANSI_KeypadEnter:
        return @"Enter";
    case kVK_Tab:
        return @"Tab";
    case kVK_Delete:
        return @"Backspace";
    case kVK_ForwardDelete:
        return @"Delete";
    case kVK_Escape:
        return @"Escape";
    case kVK_LeftArrow:
        return @"ArrowLeft";
    case kVK_RightArrow:
        return @"ArrowRight";
    case kVK_UpArrow:
        return @"ArrowUp";
    case kVK_DownArrow:
        return @"ArrowDown";
    case kVK_Home:
        return @"Home";
    case kVK_End:
        return @"End";
    case kVK_PageUp:
        return @"PageUp";
    case kVK_PageDown:
        return @"PageDown";
    case kVK_F1:
        return @"F1";
    case kVK_F2:
        return @"F2";
    case kVK_F3:
        return @"F3";
    case kVK_F4:
        return @"F4";
    case kVK_F5:
        return @"F5";
    case kVK_F6:
        return @"F6";
    case kVK_F7:
        return @"F7";
    case kVK_F8:
        return @"F8";
    case kVK_F9:
        return @"F9";
    case kVK_F10:
        return @"F10";
    case kVK_F11:
        return @"F11";
    case kVK_F12:
        return @"F12";
    default:
        return nil;
    }
}

// The DOM `key` of a modifier key and the flag it holds while down, or nil.
static NSString *modifierKey(unsigned short keyCode, NSEventModifierFlags *flag)
{
    switch (keyCode) {
    case kVK_Shift:
    case kVK_RightShift:
        *flag = NSEventModifierFlagShift;
        return @"Shift";
    case kVK_Control:
    case kVK_RightControl:
        *flag = NSEventModifierFlagControl;
        return @"Control";
    case kVK_Option:
    case kVK_RightOption:
        *flag = NSEventModifierFlagOption;
        return @"Alt";
    case kVK_Command:
    case kVK_RightCommand:
        *flag = NSEventModifierFlagCommand;
        return @"Meta";
    default:
        return nil;
    }
}

// The view a block's frames show in. It reports the user's mouse, wheel and keys as input steps in
// the block's CSS pixels, whatever size the window shows the page at.
@interface PageView : NSView
@property (nonatomic) uint32_t block;
// The block's size in CSS pixels.
@property (nonatomic) CGSize pageSize;
@end

@implementation PageView

- (BOOL)isFlipped
{
    return YES;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

// The click that brings the window forward is also the page's.
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
    return YES;
}

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    for (NSTrackingArea *area in self.trackingAreas.copy)
        [self removeTrackingArea:area];
    NSTrackingAreaOptions options = NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect;
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect options:options owner:self userInfo:nil]];
}

- (void)send:(NSDictionary *)step
{
    emit(@{ @"input": @{ @"block": @(self.block), @"events": @[ step ] } });
}

- (NSPoint)pagePoint:(NSEvent *)event
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    NSSize size = self.bounds.size;
    if (size.width <= 0 || size.height <= 0)
        return NSZeroPoint;
    return NSMakePoint(point.x * self.pageSize.width / size.width, point.y * self.pageSize.height / size.height);
}

- (void)mouse:(NSEvent *)event action:(NSString *)action button:(NSString *)button
{
    NSPoint point = [self pagePoint:event];
    NSMutableDictionary *step = [@{
        @"type": @"mouse",
        @"action": action,
        @"x": @(point.x),
        @"y": @(point.y),
        @"mods": modifiersOf(event),
    } mutableCopy];
    if (button)
        step[@"button"] = button;
    if (![action isEqualToString:@"move"])
        step[@"clicks"] = @(event.clickCount);
    [self send:step];
}

- (void)mouseDown:(NSEvent *)event
{
    [self.window makeFirstResponder:self];
    [self mouse:event action:@"down" button:@"left"];
}

- (void)mouseUp:(NSEvent *)event
{
    [self mouse:event action:@"up" button:@"left"];
}

- (void)mouseDragged:(NSEvent *)event
{
    [self mouse:event action:@"move" button:@"left"];
}

- (void)rightMouseDown:(NSEvent *)event
{
    [self mouse:event action:@"down" button:@"right"];
}

- (void)rightMouseUp:(NSEvent *)event
{
    [self mouse:event action:@"up" button:@"right"];
}

- (void)rightMouseDragged:(NSEvent *)event
{
    [self mouse:event action:@"move" button:@"right"];
}

// The host plays three buttons; the others are not the page's.
- (void)otherMouseDown:(NSEvent *)event
{
    if (event.buttonNumber == 2)
        [self mouse:event action:@"down" button:@"middle"];
}

- (void)otherMouseUp:(NSEvent *)event
{
    if (event.buttonNumber == 2)
        [self mouse:event action:@"up" button:@"middle"];
}

- (void)otherMouseDragged:(NSEvent *)event
{
    if (event.buttonNumber == 2)
        [self mouse:event action:@"move" button:@"middle"];
}

- (void)mouseMoved:(NSEvent *)event
{
    [self mouse:event action:@"move" button:nil];
}

- (void)scrollWheel:(NSEvent *)event
{
    CGFloat unit = event.hasPreciseScrollingDeltas ? 1 : pixelsPerWheelLine;
    NSPoint point = [self pagePoint:event];
    // AppKit's deltas say where the content moves; the page's wheel deltas say where the view scrolls.
    [self send:@{
        @"type": @"wheel",
        @"x": @(point.x),
        @"y": @(point.y),
        @"dx": @(-event.scrollingDeltaX * unit),
        @"dy": @(-event.scrollingDeltaY * unit),
        @"mods": modifiersOf(event),
    }];
}

- (void)key:(NSEvent *)event action:(NSString *)action
{
    NSString *key = namedKey(event.keyCode);
    if (!key) {
        // With Control held, `characters` is a control character, not the key.
        BOOL control = (event.modifierFlags & NSEventModifierFlagControl) != 0;
        NSString *typed = control ? event.charactersIgnoringModifiers : event.characters;
        if (!typed.length)
            return;
        unichar character = [typed characterAtIndex:0];
        if (typed.length == 1 && character >= 0x20 && character <= 0x7e)
            key = typed;
        else {
            // What a key press cannot say (an accented or composed character) goes in as text, once.
            if ([action isEqualToString:@"down"] && !(event.modifierFlags & (NSEventModifierFlagControl | NSEventModifierFlagCommand)))
                [self send:@{ @"type": @"text", @"text": typed }];
            return;
        }
    }
    [self send:@{ @"type": @"key", @"action": action, @"key": key, @"mods": modifiersOf(event) }];
}

- (void)keyDown:(NSEvent *)event
{
    [self key:event action:@"down"];
}

- (void)keyUp:(NSEvent *)event
{
    [self key:event action:@"up"];
}

- (void)flagsChanged:(NSEvent *)event
{
    NSEventModifierFlags flag = 0;
    NSString *key = modifierKey(event.keyCode, &flag);
    if (!key)
        return;
    NSString *action = (event.modifierFlags & flag) ? @"down" : @"up";
    [self send:@{ @"type": @"key", @"action": action, @"key": key, @"mods": modifiersOf(event) }];
}

@end

// One presented block: what the client said about it, its buffers and, once a frame came, its window.
@interface Page : NSObject <NSWindowDelegate>
@property (nonatomic) uint32_t block;
@property (nonatomic, copy) NSString *title;
// Device pixels per CSS pixel of the block, as the client opened it.
@property (nonatomic) CGFloat scale;
// IOSurfaces by buffer id.
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, id> *buffers;
// The buffer the window shows, which the host must not draw into.
@property (nonatomic, strong) NSNumber *shown;
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) PageView *view;
// The user closed the window: frames are let go of until the client stops presenting.
@property (nonatomic) BOOL dismissed;
@end

@implementation Page

- (instancetype)initWithBlock:(uint32_t)block
{
    if (!(self = [super init]))
        return nil;
    _block = block;
    _title = @"Browser";
    _scale = 1;
    _buffers = [NSMutableDictionary dictionary];
    return self;
}

- (void)setTitle:(NSString *)title
{
    _title = [title copy];
    self.window.title = _title;
}

// Sizes the window for a page of `size` CSS pixels: one point per CSS pixel, smaller where the screen is.
- (void)fit:(CGSize)size
{
    self.view.pageSize = size;
    NSScreen *screen = self.window.screen ?: NSScreen.mainScreen;
    NSRect available = [NSWindow contentRectForFrameRect:screen.visibleFrame styleMask:self.window.styleMask];
    CGFloat shrink = MIN(1, MIN(available.size.width / size.width, available.size.height / size.height));
    NSSize content = NSMakeSize(floor(size.width * shrink), floor(size.height * shrink));
    if (!NSEqualSizes(self.window.contentLayoutRect.size, content))
        [self.window setContentSize:content];
}

- (void)open:(CGSize)size
{
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height) styleMask:style backing:NSBackingStoreBuffered defer:NO];
    self.window.releasedWhenClosed = NO;
    self.window.delegate = self;
    self.window.title = self.title;
    self.view = [[PageView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
    self.view.block = self.block;
    self.view.wantsLayer = YES;
    self.view.layer.contentsGravity = kCAGravityResize;
    self.view.layer.backgroundColor = NSColor.whiteColor.CGColor;
    self.window.contentView = self.view;
    [self fit:size];
    // Each window a step from the last one, so two pages do not hide each other.
    static NSPoint corner;
    if (NSEqualPoints(corner, NSZeroPoint)) {
        [self.window center];
        corner = NSMakePoint(NSMinX(self.window.frame), NSMaxY(self.window.frame));
    }
    corner = [self.window cascadeTopLeftFromPoint:corner];
    // In front, but the keyboard stays with the app the user is in.
    [self.window orderFrontRegardless];
}

// Shows `buffer`, and lets the host have the one shown before.
- (void)show:(NSNumber *)buffer
{
    id surface = self.buffers[buffer];
    if (!surface)
        return;
    if (self.dismissed) {
        uint32_t words[2] = { self.block, buffer.unsignedIntValue };
        sendToHost(MessageReleased, words, 2);
        return;
    }
    IOSurfaceRef pixels = (__bridge IOSurfaceRef)surface;
    CGSize size = CGSizeMake(IOSurfaceGetWidth(pixels) / self.scale, IOSurfaceGetHeight(pixels) / self.scale);
    if (!self.window)
        [self open:size];
    else if (!CGSizeEqualToSize(self.view.pageSize, size))
        [self fit:size];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    // A layer latches a surface's pixels when it gets it: the same surface again has to be set anew.
    if ([self.shown isEqual:buffer])
        self.view.layer.contents = nil;
    self.view.layer.contents = surface;
    [CATransaction commit];
    if (self.shown && ![self.shown isEqual:buffer]) {
        uint32_t words[2] = { self.block, self.shown.unsignedIntValue };
        sendToHost(MessageReleased, words, 2);
    }
    self.shown = buffer;
}

- (void)forget:(NSNumber *)buffer
{
    [self.buffers removeObjectForKey:buffer];
    if ([self.shown isEqual:buffer])
        self.shown = nil;
}

- (void)close
{
    self.window.delegate = nil;
    [self.window close];
    self.window = nil;
    self.view = nil;
    [self.buffers removeAllObjects];
    self.shown = nil;
}

// The user closed the window: the client hears it and stops presenting the block.
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    self.dismissed = YES;
    if (self.shown) {
        uint32_t words[2] = { self.block, self.shown.unsignedIntValue };
        sendToHost(MessageReleased, words, 2);
        self.shown = nil;
    }
    emit(@{ @"closed": @{ @"block": @(self.block) } });
    return YES;
}

@end

@interface Viewer : NSObject <NSApplicationDelegate>
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, Page *> *pages;
@property (nonatomic, strong) CADisplayLink *link;
@property (nonatomic, strong) dispatch_source_t source;
@property (nonatomic, strong) NSMutableData *pending;
@end

@implementation Viewer

- (Page *)page:(uint32_t)block
{
    Page *page = self.pages[@(block)];
    if (!page) {
        page = [[Page alloc] initWithBlock:block];
        self.pages[@(block)] = page;
    }
    return page;
}

// The host paces its pages by the ticks of a display showing them: one link, on a window that is open.
- (void)pace
{
    NSView *shown = nil;
    for (Page *page in self.pages.allValues) {
        if (page.window.visible) {
            shown = page.view;
            break;
        }
    }
    if (self.link && (!shown || self.link.paused)) {
        [self.link invalidate];
        self.link = nil;
    }
    if (shown && !self.link) {
        self.link = [shown displayLinkWithTarget:self selector:@selector(refreshed:)];
        [self.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
}

- (void)refreshed:(CADisplayLink *)link
{
    sendToHost(MessageTick, NULL, 0);
}

- (void)handle:(const uint8_t *)bytes size:(mach_msg_size_t)size
{
    const mach_msg_header_t *header = (const mach_msg_header_t *)bytes;
    BOOL complex = (header->msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0;
    size_t wordsOffset = complex ? ComplexWordsOffset : SimpleWordsOffset;
    uint32_t words[4] = { 0, 0, 0, 0 };
    if (size > wordsOffset)
        memcpy(words, bytes + wordsOffset, MIN(sizeof(words), size - wordsOffset));
    mach_port_t port = MACH_PORT_NULL;
    if (complex && size >= ComplexWordsOffset)
        memcpy(&port, bytes + ComplexPortOffset, sizeof(port));

    switch (header->msgh_id) {
    case MessageHello:
        if (hostPort != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), hostPort);
        hostPort = port;
        return;
    case MessageBuffer: {
        IOSurfaceRef surface = port == MACH_PORT_NULL ? NULL : IOSurfaceLookupFromMachPort(port);
        if (port != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), port);
        if (surface)
            [self page:words[0]].buffers[@(words[1])] = (__bridge_transfer id)surface;
        return;
    }
    case MessageFrame:
        [[self page:words[0]] show:@(words[1])];
        [self pace];
        return;
    case MessageGone:
        [self.pages[@(words[0])] forget:@(words[1])];
        return;
    case MessageBye:
        [self.pages[@(words[0])] close];
        [self.pages removeObjectForKey:@(words[0])];
        [self pace];
        emit(@{ @"ended": @{ @"block": @(words[0]) } });
        return;
    default:
        if (port != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), port);
        return;
    }
}

- (void)receive
{
    for (;;) {
        union {
            mach_msg_header_t header;
            uint8_t bytes[512];
        } message;
        kern_return_t result = mach_msg(&message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(message), servicePort, 0, MACH_PORT_NULL);
        if (result != KERN_SUCCESS)
            return;
        [self handle:message.bytes size:message.header.msgh_size];
    }
}

// The client's commands, one JSON object a line: `show` names a block's window (its title, the
// block's scale) before the block is presented, and renames it later; `hide` closes one.
- (void)command:(NSDictionary *)command
{
    NSDictionary *show = command[@"show"];
    if ([show isKindOfClass:NSDictionary.class] && [show[@"block"] isKindOfClass:NSNumber.class]) {
        Page *page = [self page:[show[@"block"] unsignedIntValue]];
        page.dismissed = NO;
        if ([show[@"title"] isKindOfClass:NSString.class])
            page.title = show[@"title"];
        if ([show[@"scale"] isKindOfClass:NSNumber.class] && [show[@"scale"] doubleValue] > 0)
            page.scale = [show[@"scale"] doubleValue];
    }
    NSDictionary *hide = command[@"hide"];
    if ([hide isKindOfClass:NSDictionary.class] && [hide[@"block"] isKindOfClass:NSNumber.class]) {
        NSNumber *block = @([hide[@"block"] unsignedIntValue]);
        [self.pages[block] close];
        [self.pages removeObjectForKey:block];
        [self pace];
    }
}

- (void)read:(NSData *)data
{
    // The client closed its end: it is gone, or wants no window any more.
    if (!data.length)
        exit(0);
    [self.pending appendData:data];
    for (;;) {
        const uint8_t *bytes = self.pending.bytes;
        const uint8_t *newline = memchr(bytes, '\n', self.pending.length);
        if (!newline)
            return;
        NSUInteger length = (NSUInteger)(newline - bytes);
        id command = [NSJSONSerialization JSONObjectWithData:[self.pending subdataWithRange:NSMakeRange(0, length)] options:0 error:nil];
        [self.pending replaceBytesInRange:NSMakeRange(0, length + 1) withBytes:NULL length:0];
        if ([command isKindOfClass:NSDictionary.class])
            [self command:command];
    }
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    self.pages = [NSMutableDictionary dictionary];
    self.pending = [NSMutableData data];

    NSString *name = [NSString stringWithFormat:@"org.webkit.WPEAgentView.%d", getpid()];
    kern_return_t result = bootstrap_check_in(bootstrap_port, name.UTF8String, &servicePort);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "WPEAgentView: the Mach service %s cannot be checked in (%d)\n", name.UTF8String, result);
        exit(1);
    }
    // The host sends with a timeout: a full queue would end the presentations.
    mach_port_limits_t limits = { .mpl_qlimit = MACH_PORT_QLIMIT_LARGE };
    mach_port_set_attributes(mach_task_self(), servicePort, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT);

    self.source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, servicePort, 0, dispatch_get_main_queue());
    __weak Viewer *weak = self;
    dispatch_source_set_event_handler(self.source, ^{
        [weak receive];
    });
    dispatch_resume(self.source);

    NSFileHandle.fileHandleWithStandardInput.readabilityHandler = ^(NSFileHandle *handle) {
        NSData *data = handle.availableData;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weak read:data];
        });
    };

    emit(@{ @"ready": @{ @"mach": name } });
}

@end

int main(void)
{
    @autoreleasepool {
        NSApplication *application = NSApplication.sharedApplication;
        // No Dock icon and no menu bar: the windows belong to the client's session, not to an app of their own.
        application.activationPolicy = NSApplicationActivationPolicyAccessory;
        Viewer *viewer = [[Viewer alloc] init];
        application.delegate = viewer;
        [application run];
    }
    return 0;
}
