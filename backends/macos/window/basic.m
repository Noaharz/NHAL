/*
 * window.basic – macOS (Cocoa, Objective-C mit ARC).
 *
 * Jedes Fenster hat eine eigene NSView (NHALView), die Tastatur, Maus, Rad,
 * Stift, Text (NSTextInputClient, also auch IME) und Drops in
 * nhal_input_event übersetzt. Alles läuft im Hauptthread.
 *
 * Tasten: keyCode von NSEvent ist die Lage auf der Tastatur (wie nhal_key),
 * unabhängig von der Belegung. Text kommt über interpretKeyEvents.
 *
 * Eigenheit von AppKit: Bei gedrückter Cmd-Taste stellt NSApp KeyUp nicht
 * zu. poll_events reicht diese Ereignisse deshalb selbst ans Fenster.
 */
#include "nhal/features/window/basic.h"
#include "common/window/queue.h"
#include "nhal/nhal_backend.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <string.h>

#define MAX_WINDOWS 16u

typedef struct mac_window {
    uint32_t id;
    __strong NSWindow* window;
    __strong NSView* view;
    __strong id delegate;
    bool text_input;
    bool high_dpi;
    bool cursor_hidden;
    uint32_t buttons;
} mac_window;

static mac_window g_win[MAX_WINDOWS];
static uint32_t g_next_id;
static nhal_evq g_q;
static bool g_eraser;
static __strong id g_app_delegate;

/* ── Tasten ────────────────────────────────────────────────────────────── */

/* macOS keyCode (kVK_*) → nhal_key */
static const uint8_t k_keymap[128] = {
    [0x00] = NHAL_KEY_A, [0x01] = NHAL_KEY_S, [0x02] = NHAL_KEY_D, [0x03] = NHAL_KEY_F, [0x04] = NHAL_KEY_H,
    [0x05] = NHAL_KEY_G, [0x06] = NHAL_KEY_Z, [0x07] = NHAL_KEY_X, [0x08] = NHAL_KEY_C, [0x09] = NHAL_KEY_V,
    [0x0A] = NHAL_KEY_INTL_BACKSLASH, [0x0B] = NHAL_KEY_B, [0x0C] = NHAL_KEY_Q, [0x0D] = NHAL_KEY_W,
    [0x0E] = NHAL_KEY_E, [0x0F] = NHAL_KEY_R, [0x10] = NHAL_KEY_Y, [0x11] = NHAL_KEY_T, [0x12] = NHAL_KEY_1,
    [0x13] = NHAL_KEY_2, [0x14] = NHAL_KEY_3, [0x15] = NHAL_KEY_4, [0x16] = NHAL_KEY_6, [0x17] = NHAL_KEY_5,
    [0x18] = NHAL_KEY_EQUAL, [0x19] = NHAL_KEY_9, [0x1A] = NHAL_KEY_7, [0x1B] = NHAL_KEY_MINUS, [0x1C] = NHAL_KEY_8,
    [0x1D] = NHAL_KEY_0, [0x1E] = NHAL_KEY_RIGHT_BRACKET, [0x1F] = NHAL_KEY_O, [0x20] = NHAL_KEY_U,
    [0x21] = NHAL_KEY_LEFT_BRACKET, [0x22] = NHAL_KEY_I, [0x23] = NHAL_KEY_P, [0x24] = NHAL_KEY_ENTER,
    [0x25] = NHAL_KEY_L, [0x26] = NHAL_KEY_J, [0x27] = NHAL_KEY_APOSTROPHE, [0x28] = NHAL_KEY_K,
    [0x29] = NHAL_KEY_SEMICOLON, [0x2A] = NHAL_KEY_BACKSLASH, [0x2B] = NHAL_KEY_COMMA, [0x2C] = NHAL_KEY_SLASH,
    [0x2D] = NHAL_KEY_N, [0x2E] = NHAL_KEY_M, [0x2F] = NHAL_KEY_PERIOD, [0x30] = NHAL_KEY_TAB,
    [0x31] = NHAL_KEY_SPACE, [0x32] = NHAL_KEY_GRAVE, [0x33] = NHAL_KEY_BACKSPACE, [0x35] = NHAL_KEY_ESCAPE,
    [0x36] = NHAL_KEY_RIGHT_SUPER, [0x37] = NHAL_KEY_LEFT_SUPER, [0x38] = NHAL_KEY_LEFT_SHIFT,
    [0x39] = NHAL_KEY_CAPS_LOCK, [0x3A] = NHAL_KEY_LEFT_ALT, [0x3B] = NHAL_KEY_LEFT_CTRL,
    [0x3C] = NHAL_KEY_RIGHT_SHIFT, [0x3D] = NHAL_KEY_RIGHT_ALT, [0x3E] = NHAL_KEY_RIGHT_CTRL,
    [0x40] = NHAL_KEY_F17, [0x41] = NHAL_KEY_KP_DECIMAL, [0x43] = NHAL_KEY_KP_MULTIPLY, [0x45] = NHAL_KEY_KP_ADD,
    [0x47] = NHAL_KEY_NUM_LOCK, /* "Clear" auf Mac-Tastaturen sitzt dort */
    [0x4B] = NHAL_KEY_KP_DIVIDE, [0x4C] = NHAL_KEY_KP_ENTER, [0x4E] = NHAL_KEY_KP_SUBTRACT, [0x4F] = NHAL_KEY_F18,
    [0x50] = NHAL_KEY_F19, [0x51] = NHAL_KEY_KP_EQUAL, [0x52] = NHAL_KEY_KP_0, [0x53] = NHAL_KEY_KP_1,
    [0x54] = NHAL_KEY_KP_2, [0x55] = NHAL_KEY_KP_3, [0x56] = NHAL_KEY_KP_4, [0x57] = NHAL_KEY_KP_5,
    [0x58] = NHAL_KEY_KP_6, [0x59] = NHAL_KEY_KP_7, [0x5A] = NHAL_KEY_F20, [0x5B] = NHAL_KEY_KP_8,
    [0x5C] = NHAL_KEY_KP_9, [0x60] = NHAL_KEY_F5, [0x61] = NHAL_KEY_F6, [0x62] = NHAL_KEY_F7, [0x63] = NHAL_KEY_F3,
    [0x64] = NHAL_KEY_F8, [0x65] = NHAL_KEY_F9, [0x67] = NHAL_KEY_F11, [0x69] = NHAL_KEY_F13, [0x6A] = NHAL_KEY_F16,
    [0x6B] = NHAL_KEY_F14, [0x6D] = NHAL_KEY_F10, [0x6E] = NHAL_KEY_MENU, [0x6F] = NHAL_KEY_F12,
    [0x71] = NHAL_KEY_F15, [0x72] = NHAL_KEY_INSERT, /* "Help" sitzt auf PC-Tastaturen als Einfg */
    [0x73] = NHAL_KEY_HOME, [0x74] = NHAL_KEY_PAGE_UP, [0x75] = NHAL_KEY_DELETE, [0x76] = NHAL_KEY_F4,
    [0x77] = NHAL_KEY_END, [0x78] = NHAL_KEY_F2, [0x79] = NHAL_KEY_PAGE_DOWN, [0x7A] = NHAL_KEY_F1,
    [0x7B] = NHAL_KEY_LEFT, [0x7C] = NHAL_KEY_RIGHT, [0x7D] = NHAL_KEY_DOWN, [0x7E] = NHAL_KEY_UP,
};

static int keycode_for(nhal_key key) {
    for (int i = 0; i < 128; ++i) {
        if (k_keymap[i] == key) return i;
    }
    return -1;
}

/* Gerätespezifische Bits von NSEvent.modifierFlags (links/rechts getrennt). */
enum { DEV_LCTRL = 0x0001, DEV_LSHIFT = 0x0002, DEV_RSHIFT = 0x0004, DEV_LCMD = 0x0008, DEV_RCMD = 0x0010,
       DEV_LALT = 0x0020, DEV_RALT = 0x0040, DEV_RCTRL = 0x2000 };

static uint32_t device_bit(nhal_key k) {
    switch (k) {
    case NHAL_KEY_LEFT_SHIFT: return DEV_LSHIFT;
    case NHAL_KEY_RIGHT_SHIFT: return DEV_RSHIFT;
    case NHAL_KEY_LEFT_CTRL: return DEV_LCTRL;
    case NHAL_KEY_RIGHT_CTRL: return DEV_RCTRL;
    case NHAL_KEY_LEFT_ALT: return DEV_LALT;
    case NHAL_KEY_RIGHT_ALT: return DEV_RALT;
    case NHAL_KEY_LEFT_SUPER: return DEV_LCMD;
    case NHAL_KEY_RIGHT_SUPER: return DEV_RCMD;
    default: return 0;
    }
}

static uint32_t mods_of(NSEventModifierFlags f) {
    uint32_t m = 0;
    if (f & NSEventModifierFlagShift) m |= NHAL_MOD_SHIFT;
    if (f & NSEventModifierFlagControl) m |= NHAL_MOD_CTRL;
    if (f & NSEventModifierFlagOption) m |= NHAL_MOD_ALT;
    if (f & NSEventModifierFlagCommand) m |= NHAL_MOD_SUPER;
    if (f & NSEventModifierFlagCapsLock) m |= NHAL_MOD_CAPS_LOCK;
    return m;
}

static NSEventModifierFlags flags_of(uint32_t m) {
    NSEventModifierFlags f = 0;
    if (m & NHAL_MOD_SHIFT) f |= NSEventModifierFlagShift | DEV_LSHIFT;
    if (m & NHAL_MOD_CTRL) f |= NSEventModifierFlagControl | DEV_LCTRL;
    if (m & NHAL_MOD_ALT) f |= NSEventModifierFlagOption | DEV_LALT;
    if (m & NHAL_MOD_SUPER) f |= NSEventModifierFlagCommand | DEV_LCMD;
    if (m & NHAL_MOD_CAPS_LOCK) f |= NSEventModifierFlagCapsLock;
    return f;
}

/* Zeichen für inject_key (US-Belegung; die echte Belegung kennt nur das System). */
static NSString* us_chars(nhal_key k, bool shift) {
    static const char* lower = "abcdefghijklmnopqrstuvwxyz";
    static const char* upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char* digits = "0123456789";
    static const char* shifted = ")!@#$%^&*(";
    char c[2] = { 0, 0 };
    if (k >= NHAL_KEY_A && k <= NHAL_KEY_Z) c[0] = (shift ? upper : lower)[k - NHAL_KEY_A];
    else if (k >= NHAL_KEY_0 && k <= NHAL_KEY_9) c[0] = (shift ? shifted : digits)[k - NHAL_KEY_0];
    else if (k == NHAL_KEY_SPACE) c[0] = ' ';
    else if (k == NHAL_KEY_ENTER || k == NHAL_KEY_KP_ENTER) c[0] = '\r';
    else if (k == NHAL_KEY_TAB) c[0] = '\t';
    else if (k == NHAL_KEY_BACKSPACE) c[0] = 0x7f;
    else if (k == NHAL_KEY_ESCAPE) c[0] = 0x1b;
    return [NSString stringWithUTF8String:c];
}

/* ── Hilfen ────────────────────────────────────────────────────────────── */

/* Strong-Felder über ARC freigeben, Rest nullen (kein memset über ARC-Felder). */
static void reset(mac_window* w) {
    w->window = nil;
    w->view = nil;
    w->delegate = nil;
    w->id = 0;
    w->text_input = false;
    w->high_dpi = false;
    w->cursor_hidden = false;
    w->buttons = 0;
}

static mac_window* find(uint32_t id) {
    for (uint32_t i = 0; i < MAX_WINDOWS; ++i) {
        if (id && g_win[i].id == id) return &g_win[i];
    }
    return NULL;
}

static nhal_input_event base_event(nhal_event_type t, uint32_t window, NSTimeInterval time) {
    nhal_input_event e;
    memset(&e, 0, sizeof e);
    e.type = t;
    e.window = window;
    e.time = time;
    return e;
}

static void push(const nhal_input_event* e) { nhal_evq_push(&g_q, e); }

static void fill_size(const mac_window* w, nhal_window_size* s) {
    const NSRect r = w->view.bounds;
    const CGFloat scale = w->high_dpi ? w->window.backingScaleFactor : 1.0;
    s->width = (uint32_t)r.size.width;
    s->height = (uint32_t)r.size.height;
    s->pixel_width = (uint32_t)(r.size.width * scale + 0.5);
    s->pixel_height = (uint32_t)(r.size.height * scale + 0.5);
    s->scale = (float)scale;
}

static void push_resize(const mac_window* w) {
    nhal_input_event e = base_event(NHAL_EVENT_WINDOW_RESIZE, w->id, NSProcessInfo.processInfo.systemUptime);
    nhal_window_size s;
    fill_size(w, &s);
    e.resize.width = s.width;
    e.resize.height = s.height;
    e.resize.pixel_width = s.pixel_width;
    e.resize.pixel_height = s.pixel_height;
    e.resize.scale = s.scale;
    push(&e);
}

/* ── View ──────────────────────────────────────────────────────────────── */

@interface NHALView : NSView <NSTextInputClient, NSDraggingDestination>
@property(nonatomic) uint32_t wid;
@property(nonatomic, strong) NSString* marked;
@end

@implementation NHALView

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { (void)e; return YES; }
- (BOOL)isFlipped { return YES; } /* Ursprung oben links */

- (void)updateTrackingAreas {
    for (NSTrackingArea* a in self.trackingAreas) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
                              initWithRect:NSZeroRect
                                   options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                                           NSTrackingInVisibleRect
                                     owner:self
                                  userInfo:nil]];
    [super updateTrackingAreas];
}

- (mac_window*)win { return find(self.wid); }

- (NSPoint)pos:(NSEvent*)ev { return [self convertPoint:ev.locationInWindow fromView:nil]; }

/* Tastatur */

- (void)keyEvent:(NSEvent*)ev down:(bool)down {
    nhal_input_event e = base_event(down ? NHAL_EVENT_KEY_DOWN : NHAL_EVENT_KEY_UP, self.wid, ev.timestamp);
    e.key.native = ev.keyCode;
    e.key.key = ev.keyCode < 128 ? (nhal_key)k_keymap[ev.keyCode] : NHAL_KEY_UNKNOWN;
    e.key.mods = mods_of(ev.modifierFlags);
    e.key.repeat = down && ev.isARepeat;
    push(&e);
}

- (void)keyDown:(NSEvent*)ev {
    [self keyEvent:ev down:true];
    mac_window* w = [self win];
    if (w && w->text_input) [self interpretKeyEvents:@[ ev ]];
}

- (void)keyUp:(NSEvent*)ev { [self keyEvent:ev down:false]; }

- (void)flagsChanged:(NSEvent*)ev {
    const nhal_key k = ev.keyCode < 128 ? (nhal_key)k_keymap[ev.keyCode] : NHAL_KEY_UNKNOWN;
    nhal_input_event e = base_event(NHAL_EVENT_KEY_DOWN, self.wid, ev.timestamp);
    e.key.key = k;
    e.key.native = ev.keyCode;
    e.key.mods = mods_of(ev.modifierFlags);
    if (k == NHAL_KEY_CAPS_LOCK) { /* meldet nur den Wechsel: als Druck und Loslassen */
        push(&e);
        e.type = NHAL_EVENT_KEY_UP;
        push(&e);
        return;
    }
    const uint32_t bit = device_bit(k);
    if (!bit) return;
    e.type = (ev.modifierFlags & bit) ? NHAL_EVENT_KEY_DOWN : NHAL_EVENT_KEY_UP;
    push(&e);
}

/* Maus */

static nhal_mouse_button button_of(NSEvent* ev) {
    switch (ev.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseUp:
    case NSEventTypeLeftMouseDragged: return NHAL_MOUSE_LEFT;
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseUp:
    case NSEventTypeRightMouseDragged: return NHAL_MOUSE_RIGHT;
    default: break;
    }
    switch (ev.buttonNumber) {
    case 2: return NHAL_MOUSE_MIDDLE;
    case 3: return NHAL_MOUSE_X1;
    case 4: return NHAL_MOUSE_X2;
    default: return NHAL_MOUSE_MIDDLE;
    }
}

- (void)pen:(NSEvent*)ev phase:(nhal_pointer_phase)phase contact:(bool)contact {
    if (ev.subtype != NSEventSubtypeTabletPoint) return;
    const NSPoint p = [self pos:ev];
    nhal_input_event e = base_event(NHAL_EVENT_PEN, self.wid, ev.timestamp);
    e.pen.id = ev.deviceID;
    e.pen.phase = phase;
    e.pen.x = (float)p.x;
    e.pen.y = (float)p.y;
    e.pen.pressure = ev.pressure;
    e.pen.tilt_x = (float)ev.tilt.x;
    e.pen.tilt_y = (float)ev.tilt.y;
    e.pen.eraser = g_eraser;
    e.pen.contact = contact;
    e.pen.buttons = (uint32_t)ev.buttonMask;
    push(&e);
}

- (void)mouse:(NSEvent*)ev down:(bool)down {
    mac_window* w = [self win];
    if (!w) return;
    const nhal_mouse_button b = button_of(ev);
    if (down) w->buttons |= 1u << b;
    else w->buttons &= ~(1u << b);
    const NSPoint p = [self pos:ev];
    nhal_input_event e = base_event(down ? NHAL_EVENT_MOUSE_DOWN : NHAL_EVENT_MOUSE_UP, self.wid, ev.timestamp);
    e.mouse.x = (float)p.x;
    e.mouse.y = (float)p.y;
    e.mouse.button = b;
    e.mouse.buttons = w->buttons;
    e.mouse.clicks = (uint32_t)ev.clickCount;
    e.mouse.mods = mods_of(ev.modifierFlags);
    push(&e);
    [self pen:ev phase:down ? NHAL_POINTER_DOWN : NHAL_POINTER_UP contact:down];
}

- (void)moved:(NSEvent*)ev {
    mac_window* w = [self win];
    if (!w) return;
    const NSPoint p = [self pos:ev];
    nhal_input_event e = base_event(NHAL_EVENT_MOUSE_MOVE, self.wid, ev.timestamp);
    e.mouse.x = (float)p.x;
    e.mouse.y = (float)p.y;
    e.mouse.dx = (float)ev.deltaX;
    e.mouse.dy = (float)ev.deltaY;
    e.mouse.buttons = w->buttons;
    e.mouse.mods = mods_of(ev.modifierFlags);
    push(&e);
    [self pen:ev phase:NHAL_POINTER_MOVE contact:w->buttons != 0];
}

- (void)mouseDown:(NSEvent*)ev { [self mouse:ev down:true]; }
- (void)mouseUp:(NSEvent*)ev { [self mouse:ev down:false]; }
- (void)rightMouseDown:(NSEvent*)ev { [self mouse:ev down:true]; }
- (void)rightMouseUp:(NSEvent*)ev { [self mouse:ev down:false]; }
- (void)otherMouseDown:(NSEvent*)ev { [self mouse:ev down:true]; }
- (void)otherMouseUp:(NSEvent*)ev { [self mouse:ev down:false]; }
- (void)mouseMoved:(NSEvent*)ev { [self moved:ev]; }
- (void)mouseDragged:(NSEvent*)ev { [self moved:ev]; }
- (void)rightMouseDragged:(NSEvent*)ev { [self moved:ev]; }
- (void)otherMouseDragged:(NSEvent*)ev { [self moved:ev]; }

- (void)tabletProximity:(NSEvent*)ev {
    g_eraser = ev.pointingDeviceType == NSPointingDeviceTypeEraser;
}

- (void)scrollWheel:(NSEvent*)ev {
    const NSPoint p = [self pos:ev];
    nhal_input_event e = base_event(NHAL_EVENT_MOUSE_WHEEL, self.wid, ev.timestamp);
    e.wheel.dx = (float)ev.scrollingDeltaX;
    e.wheel.dy = (float)ev.scrollingDeltaY;
    e.wheel.precise = ev.hasPreciseScrollingDeltas;
    e.wheel.mods = mods_of(ev.modifierFlags);
    e.wheel.x = (float)p.x;
    e.wheel.y = (float)p.y;
    if (e.wheel.dx != 0.0f || e.wheel.dy != 0.0f) push(&e);
}

- (void)crossing:(NSEvent*)ev type:(nhal_event_type)t {
    const NSPoint p = [self pos:ev];
    nhal_input_event e = base_event(t, self.wid, ev.timestamp);
    e.mouse.x = (float)p.x;
    e.mouse.y = (float)p.y;
    push(&e);
}

- (void)mouseEntered:(NSEvent*)ev { [self crossing:ev type:NHAL_EVENT_MOUSE_ENTER]; }
- (void)mouseExited:(NSEvent*)ev { [self crossing:ev type:NHAL_EVENT_MOUSE_LEAVE]; }

/* Text (NSTextInputClient) */

static NSString* plain(id s) { return [s isKindOfClass:[NSAttributedString class]] ? [s string] : s; }

- (void)insertText:(id)string replacementRange:(NSRange)range {
    (void)range;
    self.marked = nil;
    const char* utf8 = plain(string).UTF8String;
    if (!utf8 || !utf8[0]) return;
    nhal_input_event e = base_event(NHAL_EVENT_TEXT_INPUT, self.wid, NSProcessInfo.processInfo.systemUptime);
    e.text.cursor = -1;
    nhal_evq_push_text(&g_q, e, utf8);
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)sel replacementRange:(NSRange)range {
    (void)range;
    NSString* s = plain(string);
    self.marked = s.length ? s : nil;
    nhal_input_event e = base_event(NHAL_EVENT_TEXT_EDITING, self.wid, NSProcessInfo.processInfo.systemUptime);
    const NSUInteger loc = sel.location <= s.length ? sel.location : s.length;
    const NSUInteger len = loc + sel.length <= s.length ? sel.length : s.length - loc;
    e.text.cursor = (int32_t)[[s substringToIndex:loc] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    e.text.selection_length =
        (int32_t)[[s substringWithRange:NSMakeRange(loc, len)] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    e.text.text = s.UTF8String ? s.UTF8String : "";
    push(&e);
}

- (void)unmarkText { self.marked = nil; }
- (BOOL)hasMarkedText { return self.marked != nil; }
- (NSRange)markedRange { return self.marked ? NSMakeRange(0, self.marked.length) : NSMakeRange(NSNotFound, 0); }
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText { return @[]; }
- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)a {
    (void)r;
    (void)a;
    return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)p { (void)p; return NSNotFound; }
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)a {
    (void)r;
    (void)a;
    const NSRect inWindow = [self convertRect:self.bounds toView:nil];
    return [self.window convertRectToScreen:inWindow];
}
- (void)doCommandBySelector:(SEL)sel { (void)sel; } /* kein Piepton bei Pfeilen, Enter, … */

/* Drops */

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info { (void)info; return NSDragOperationCopy; }

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    NSArray<NSURL*>* urls = [info.draggingPasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                                    options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    const NSPoint p = [self convertPoint:info.draggingLocation fromView:nil];
    uint32_t index = 0;
    for (NSURL* url in urls) {
        nhal_input_event e = base_event(NHAL_EVENT_FILE_DROP, self.wid, NSProcessInfo.processInfo.systemUptime);
        e.drop.path = url.fileSystemRepresentation;
        e.drop.x = (float)p.x;
        e.drop.y = (float)p.y;
        e.drop.index = index++;
        e.drop.count = (uint32_t)urls.count;
        push(&e);
    }
    return urls.count > 0;
}

@end

/* ── Fenster- und App-Delegate ─────────────────────────────────────────── */

@interface NHALWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) uint32_t wid;
@end

@implementation NHALWindowDelegate
- (void)simple:(nhal_event_type)t {
    nhal_input_event e = base_event(t, self.wid, NSProcessInfo.processInfo.systemUptime);
    push(&e);
}
- (BOOL)windowShouldClose:(NSWindow*)w {
    (void)w;
    [self simple:NHAL_EVENT_WINDOW_CLOSE];
    return NO; /* die App entscheidet */
}
- (void)windowDidResize:(NSNotification*)n {
    (void)n;
    mac_window* w = find(self.wid);
    if (w) push_resize(w);
}
- (void)windowDidChangeBackingProperties:(NSNotification*)n { [self windowDidResize:n]; }
- (void)windowDidBecomeKey:(NSNotification*)n {
    (void)n;
    [self simple:NHAL_EVENT_WINDOW_FOCUS];
}
- (void)windowDidResignKey:(NSNotification*)n {
    (void)n;
    [self simple:NHAL_EVENT_WINDOW_BLUR];
}
@end

@interface NHALAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation NHALAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)app {
    (void)app;
    nhal_input_event e = base_event(NHAL_EVENT_QUIT, 0, NSProcessInfo.processInfo.systemUptime);
    push(&e);
    return NSTerminateCancel; /* die App entscheidet */
}
@end

static void activate_app(void) {
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
}

static void ensure_app(void) {
    [NSApplication sharedApplication];
    if (NSApp.activationPolicy == NSApplicationActivationPolicyProhibited)
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if (!NSApp.mainMenu) {
        NSMenu* bar = [NSMenu new];
        NSMenuItem* appItem = [NSMenuItem new];
        [bar addItem:appItem];
        NSMenu* appMenu = [NSMenu new];
        NSString* name = NSProcessInfo.processInfo.processName;
        [appMenu addItemWithTitle:[@"Beenden: " stringByAppendingString:name]
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];
        appItem.submenu = appMenu;
        NSApp.mainMenu = bar;
    }
    if (!NSApp.delegate) {
        g_app_delegate = [NHALAppDelegate new];
        NSApp.delegate = g_app_delegate;
    }
    if (!NSRunningApplication.currentApplication.finishedLaunching) [NSApp finishLaunching];
}

/* ── Funktionen ────────────────────────────────────────────────────────── */

static nhal_status not_main(void) {
    return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "window.basic auf macOS nur im Hauptthread");
}

static nhal_status no_window(void) { return nhal_status_error(NHAL_ERR_NOT_FOUND, "Unbekanntes Fenster"); }

static nhal_status win_create(const nhal_window_desc* d, uint32_t* out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out_id ist NULL");
    *out = 0;
    if (!NSThread.isMainThread) return not_main();
    mac_window* w = NULL;
    for (uint32_t i = 0; i < MAX_WINDOWS && !w; ++i) {
        if (!g_win[i].id) w = &g_win[i];
    }
    if (!w) return nhal_status_error(NHAL_ERR_BUSY, "Zu viele Fenster (höchstens 16)");
    @autoreleasepool {
        ensure_app();
        const uint32_t flags = d ? d->flags : 0;
        const CGFloat width = d && d->width ? d->width : 1280;
        const CGFloat height = d && d->height ? d->height : 720;
        NSWindowStyleMask style = NSWindowStyleMaskBorderless;
        if (!(flags & NHAL_WINDOW_BORDERLESS)) {
            style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
            if (flags & NHAL_WINDOW_RESIZABLE) style |= NSWindowStyleMaskResizable;
        }
        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, width, height)
                                                       styleMask:style
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        if (!window) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "NSWindow ließ sich nicht anlegen");
        window.releasedWhenClosed = NO;
        window.acceptsMouseMovedEvents = YES;
        window.title = [NSString stringWithUTF8String:d && d->title ? d->title : ""];
        NHALView* view = [[NHALView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
        view.wid = ++g_next_id;
        if (!g_next_id) view.wid = g_next_id = 1;
        window.contentView = view;
        [view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
        NHALWindowDelegate* delegate = [NHALWindowDelegate new];
        delegate.wid = view.wid;
        window.delegate = delegate;
        [window makeFirstResponder:view];
        [window center];

        w->id = view.wid;
        w->window = window;
        w->view = view;
        w->delegate = delegate;
        w->text_input = true;
        w->high_dpi = (flags & NHAL_WINDOW_HIGH_DPI) != 0;
        w->buttons = 0;
        w->cursor_hidden = false;
        if (!(flags & NHAL_WINDOW_HIDDEN)) {
            [window makeKeyAndOrderFront:nil];
            activate_app();
        }
    }
    *out = w->id;
    return nhal_status_ok();
}

static void win_destroy(uint32_t id) {
    mac_window* w = find(id);
    if (!w || !NSThread.isMainThread) return;
    @autoreleasepool {
        if (w->cursor_hidden) [NSCursor unhide];
        [w->window orderOut:nil];
        w->window.delegate = nil;
        [w->window close];
        w->delegate = nil;
        w->view = nil;
        w->window = nil;
    }
    reset(w);
}

static nhal_status win_show(uint32_t id, bool visible) {
    if (!NSThread.isMainThread) return not_main();
    mac_window* w = find(id);
    if (!w) return no_window();
    if (visible) {
        [w->window makeKeyAndOrderFront:nil];
        activate_app();
    } else {
        [w->window orderOut:nil];
    }
    return nhal_status_ok();
}

static nhal_status win_set_title(uint32_t id, const char* title) {
    if (!NSThread.isMainThread) return not_main();
    mac_window* w = find(id);
    if (!w) return no_window();
    w->window.title = [NSString stringWithUTF8String:title ? title : ""];
    return nhal_status_ok();
}

static nhal_status win_set_size(uint32_t id, uint32_t width, uint32_t height) {
    if (!NSThread.isMainThread) return not_main();
    mac_window* w = find(id);
    if (!w) return no_window();
    if (!width || !height) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "Größe 0");
    [w->window setContentSize:NSMakeSize(width, height)];
    return nhal_status_ok();
}

static nhal_status win_size(uint32_t id, nhal_window_size* out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    if (!NSThread.isMainThread) return not_main();
    mac_window* w = find(id);
    if (!w) return no_window();
    fill_size(w, out);
    return nhal_status_ok();
}

static nhal_status win_native(uint32_t id, nhal_window_native* out) {
    if (!out) return nhal_status_error(NHAL_ERR_INVALID_ARGUMENT, "out ist NULL");
    mac_window* w = find(id);
    if (!w) return no_window();
    out->kind = NHAL_WINDOW_NATIVE_NSVIEW;
    out->handle = (__bridge void*)w->view;
    return nhal_status_ok();
}

static bool win_focused(uint32_t id) {
    mac_window* w = find(id);
    return w && w->window.isKeyWindow;
}

static void dispatch(NSEvent* ev) {
    /* AppKit stellt KeyUp mit Cmd nicht zu – selbst ans Fenster geben. */
    if (ev.type == NSEventTypeKeyUp && (ev.modifierFlags & NSEventModifierFlagCommand) && ev.window) {
        [ev.window sendEvent:ev];
        return;
    }
    [NSApp sendEvent:ev];
}

static void win_poll(void) {
    if (!NSThread.isMainThread || !NSApp) return;
    @autoreleasepool {
        NSEvent* ev;
        while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast]
                                           inMode:NSDefaultRunLoopMode dequeue:YES])) {
            dispatch(ev);
        }
        [NSApp updateWindows];
    }
}

static bool win_wait(uint32_t timeout_ms) {
    if (!NSThread.isMainThread || !NSApp) return false;
    const uint32_t before = g_q.count;
    @autoreleasepool {
        NSEvent* ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                         untilDate:[NSDate dateWithTimeIntervalSinceNow:timeout_ms / 1000.0]
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:YES];
        if (ev) dispatch(ev);
    }
    win_poll();
    return g_q.count > before;
}

static bool win_next(nhal_input_event* out) { return nhal_evq_pop(&g_q, out); }
static uint64_t win_dropped(void) { return g_q.dropped; }

static nhal_status win_text_input(uint32_t id, bool enabled) {
    mac_window* w = find(id);
    if (!w) return no_window();
    w->text_input = enabled;
    if (!enabled) [((NHALView*)w->view) unmarkText];
    return nhal_status_ok();
}

static nhal_status win_cursor(uint32_t id, bool visible) {
    if (!NSThread.isMainThread) return not_main();
    mac_window* w = find(id);
    if (!w) return no_window();
    if (visible && w->cursor_hidden) [NSCursor unhide];
    if (!visible && !w->cursor_hidden) [NSCursor hide];
    w->cursor_hidden = !visible;
    return nhal_status_ok();
}

static nhal_status win_push(const nhal_input_event* ev) {
    nhal_status s = nhal_evq_check_synthetic(ev);
    if (s.code) return s;
    nhal_input_event e = *ev;
    e.synthetic = true;
    if (e.time == 0.0) e.time = NSProcessInfo.processInfo.systemUptime;
    if (!nhal_evq_push(&g_q, &e)) return nhal_status_error(NHAL_ERR_BUSY, "Warteschlange voll oder String zu lang");
    return nhal_status_ok();
}

static nhal_status win_inject_key(uint32_t id, nhal_key key, bool down, uint32_t mods) {
    if (!NSThread.isMainThread) return not_main();
    mac_window* w = find(id);
    if (!w) return no_window();
    const int code = keycode_for(key);
    if (code < 0) return nhal_status_error(NHAL_ERR_UNSUPPORTED, "Taste hat auf macOS keinen keyCode");
    @autoreleasepool {
        NSEventModifierFlags flags = flags_of(mods);
        const uint32_t bit = device_bit(key);
        NSEventType type = down ? NSEventTypeKeyDown : NSEventTypeKeyUp;
        if (bit || key == NHAL_KEY_CAPS_LOCK) { /* Modifier kommen als flagsChanged */
            type = NSEventTypeFlagsChanged;
            if (down) flags |= bit;
            else flags &= ~(NSEventModifierFlags)bit;
        }
        NSString* chars = type == NSEventTypeFlagsChanged ? @"" : us_chars(key, (mods & NHAL_MOD_SHIFT) != 0);
        NSEvent* ev = [NSEvent keyEventWithType:type
                                       location:NSZeroPoint
                                  modifierFlags:flags
                                      timestamp:NSProcessInfo.processInfo.systemUptime
                                   windowNumber:w->window.windowNumber
                                        context:nil
                                     characters:chars
                    charactersIgnoringModifiers:us_chars(key, false)
                                      isARepeat:NO
                                        keyCode:(unsigned short)code];
        if (!ev) return nhal_status_error(NHAL_ERR_UNAVAILABLE, "NSEvent ließ sich nicht anlegen");
        [w->window sendEvent:ev];
    }
    return nhal_status_ok();
}

static nhal_status feature_init(const nhal_allocator* allocator) {
    for (uint32_t i = 0; i < MAX_WINDOWS; ++i) reset(&g_win[i]);
    g_next_id = 0;
    g_eraser = false;
    return nhal_evq_init(&g_q, allocator);
}

static void feature_shutdown(void) {
    for (uint32_t i = 0; i < MAX_WINDOWS; ++i) {
        if (g_win[i].id) win_destroy(g_win[i].id);
    }
    if (g_app_delegate && NSApp.delegate == g_app_delegate) NSApp.delegate = nil;
    g_app_delegate = nil;
    nhal_evq_free(&g_q);
}

static const nhal_window_basic_v1 table = {
    sizeof(nhal_window_basic_v1),
    win_create, win_destroy, win_show, win_set_title, win_set_size, win_size, win_native, win_focused,
    win_poll, win_wait, win_next, win_dropped,
    win_text_input, win_cursor,
    win_push, win_inject_key,
};

const nhal_feature_impl nhal_impl_window_basic = { 1u, feature_init, feature_shutdown, &table };
