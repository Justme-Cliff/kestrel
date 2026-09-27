#include "host/macos/mac_window.h"

#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

#include "host/macos/keycodes.h"
#include "host/macos/raw_mouse.h"
#include "app/instances.h"
#include "app/frame_stats.h"
#include <sys/resource.h>
#include <filesystem>
#include "keymap/keymapper.h"

using namespace kestrel;

namespace {
// The emulator window's input callbacks, for menu items (main thread only).
WindowInput* g_input = nullptr;
std::atomic<WindowInput*> g_raw_input{nullptr};  // read by the raw mouse thread
std::function<void(const std::string&)> g_open_files;
bool g_vm_running = false;
constexpr CGFloat kSidebarWidth = 52;

NSString* ns(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }
// Newer version available? Reads the feed named in Info.plist (KestrelUpdateURL): our
// own JSON {"version", "url", "notes"} or GitHub's latest-release API. Only opens the
// download page in the browser; nothing is installed behind the user's back.
bool newer_version(NSString* remote, NSString* local) {
  NSArray* r = [[remote stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"vV "]]
      componentsSeparatedByString:@"."];
  NSArray* l = [local componentsSeparatedByString:@"."];
  for (NSUInteger i = 0; i < std::max(r.count, l.count); ++i) {
    const NSInteger a = i < r.count ? [r[i] integerValue] : 0, b = i < l.count ? [l[i] integerValue] : 0;
    if (a != b) return a > b;
  }
  return false;
}

void check_for_updates(bool interactive) {
  NSString* local = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
  if (!local) local = @"0";
  NSString* feed = [NSBundle.mainBundle objectForInfoDictionaryKey:@"KestrelUpdateURL"];
  auto tell = ^(NSString* title, NSString* text) {
    dispatch_async(dispatch_get_main_queue(), ^{
      NSAlert* alert = [[NSAlert alloc] init];
      alert.messageText = title;
      alert.informativeText = text;
      [alert runModal];
    });
  };
  NSURL* url = feed.length ? [NSURL URLWithString:feed] : nil;
  if (!url || ![url.scheme isEqualToString:@"https"]) {
    if (interactive) {
      tell(@"Updates aren't set up for this copy",
           [NSString stringWithFormat:@"This is Kestrel %@. New versions come from wherever you got this one.", local]);
    }
    return;
  }
  [NSUserDefaults.standardUserDefaults setObject:NSDate.date forKey:@"LastUpdateCheck"];
  NSURLRequest* req = [NSURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:15];
  [[NSURLSession.sharedSession dataTaskWithRequest:req completionHandler:^(NSData* data, NSURLResponse*, NSError* error) {
    NSDictionary* j = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![j isKindOfClass:NSDictionary.class]) {
      if (interactive) tell(@"Couldn't check for updates", error ? error.localizedDescription : @"The update feed didn't answer as expected.");
      return;
    }
    auto pick = [&](NSString* a, NSString* b) -> id { return j[a] ? j[a] : j[b]; };
    NSString* version = pick(@"version", @"tag_name");
    NSString* page = pick(@"url", @"html_url");
    NSString* notes = pick(@"notes", @"body");
    if (![notes isKindOfClass:NSString.class]) notes = @"";
    if (notes.length > 500) notes = [[notes substringToIndex:500] stringByAppendingString:@"…"];  // the page has the rest
    if (![version isKindOfClass:NSString.class] || ![page isKindOfClass:NSString.class] || ![page hasPrefix:@"https://"]) {
      if (interactive) tell(@"Couldn't check for updates", @"The update feed is missing the version or download address.");
      return;
    }
    if (!newer_version(version, local)) {
      if (interactive) tell(@"Kestrel is up to date", [NSString stringWithFormat:@"You have the latest version (%@).", local]);
      return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      NSAlert* alert = [[NSAlert alloc] init];
      alert.messageText = [NSString stringWithFormat:@"Kestrel %@ is available", version];
      alert.informativeText = [NSString stringWithFormat:@"You have %@.%@%@", local,
                                                         [notes isKindOfClass:NSString.class] && notes.length ? @"\n\n" : @"",
                                                         [notes isKindOfClass:NSString.class] ? notes : @""];
      [alert addButtonWithTitle:@"Download"];
      [alert addButtonWithTitle:@"Later"];
      if ([alert runModal] == NSAlertFirstButtonReturn) [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:page]];
    });
  }] resume];
}

void send_action(WindowAction a, const std::string& arg = {}) {
  if (g_input && g_input->action) g_input->action(a, arg);
}
bool is_apk(NSURL* url) { return [url.pathExtension.lowercaseString isEqualToString:@"apk"]; }
}  // namespace

// Runs a block when a control fires (target/action without a dedicated class).
@interface KestrelAction : NSObject
@property(nonatomic, copy) void (^block)(id sender);
+ (instancetype)actionWithBlock:(void (^)(id sender))block;
- (void)fire:(id)sender;
@end
@implementation KestrelAction
+ (instancetype)actionWithBlock:(void (^)(id))block {
  KestrelAction* a = [[KestrelAction alloc] init];
  a.block = block;
  return a;
}
- (void)fire:(id)sender {
  if (_block) _block(sender);
}
@end

// ---- the game view: shows frames, turns mouse/keys/gestures into guest input ----

@interface KestrelView : NSView
@property(nonatomic) WindowInput* input;
@property(nonatomic) BOOL captured;
@property(nonatomic) uint32_t guestWidth;
@property(nonatomic) uint32_t guestHeight;
@end

@implementation KestrelView {
  NSEventModifierFlags _lastFlags;
  BOOL _scrolling;        // a scroll gesture is being played as a finger drag
  NSPoint _scrollPos;     // that finger, in guest pixels
  BOOL _wheel;            // the current scroll comes from a notched mouse wheel
  NSPoint _scrollStart;   // where the finger landed
  double _pinchScale;
}

- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)isFlipped { return YES; }  // origin top-left, like the guest display

// The frame is aspect-fit inside the view.
- (NSRect)gameRect {
  const CGFloat vw = self.bounds.size.width, vh = self.bounds.size.height;
  const CGFloat scale = std::min(vw / _guestWidth, vh / _guestHeight);
  return NSMakeRect((vw - _guestWidth * scale) / 2, (vh - _guestHeight * scale) / 2, _guestWidth * scale,
                    _guestHeight * scale);
}
- (CGFloat)guestPerPoint { return _guestWidth / [self gameRect].size.width; }
- (NSPoint)guestPointForEvent:(NSEvent*)event {
  NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
  NSRect g = [self gameRect];
  return NSMakePoint((p.x - g.origin.x) * [self guestPerPoint], (p.y - g.origin.y) * [self guestPerPoint]);
}

- (void)sendPointer:(NSEvent*)event down:(bool)down up:(bool)up {
  if (!_input || !_input->pointer) return;
  NSPoint g = [self guestPointForEvent:event];
  _input->pointer(down, up, static_cast<int32_t>(g.x), static_cast<int32_t>(g.y));
}
- (void)sendDelta:(NSEvent*)e {
  if (e.deltaX == 0 && e.deltaY == 0) return;
  if (raw_mouse::system_delta(e.deltaX, e.deltaY)) return;  // raw input is moving the camera
  if (_input && _input->mouse_delta) _input->mouse_delta(e.deltaX, e.deltaY);
}
- (void)sendButton:(uint16_t)button pressed:(bool)pressed {
  if (_input && _input->mouse_button) _input->mouse_button(button, pressed);
}

// Normal mode: the left button is a finger. Aim mode: buttons are game controls
// and movement turns the camera.
- (void)mouseDown:(NSEvent*)e {
  if (_captured) [self sendButton:kMouseLeft pressed:true];
  else [self sendPointer:e down:true up:false];
}
- (void)mouseDragged:(NSEvent*)e {
  if (_captured) [self sendDelta:e];
  else [self sendPointer:e down:false up:false];
}
- (void)mouseUp:(NSEvent*)e {
  if (_captured) [self sendButton:kMouseLeft pressed:false];
  else [self sendPointer:e down:false up:true];
}
- (void)mouseMoved:(NSEvent*)e {
  if (_captured) [self sendDelta:e];
}
- (void)rightMouseDown:(NSEvent*)e {
  if (_captured) [self sendButton:kMouseRight pressed:true];
  else send_action(WindowAction::kBack);  // like BlueStacks: right click = Back
}
- (void)rightMouseUp:(NSEvent*)e {
  if (_captured) [self sendButton:kMouseRight pressed:false];
}
- (void)rightMouseDragged:(NSEvent*)e {
  if (_captured) [self sendDelta:e];
}
// Middle and side buttons: game controls while aiming (bindable like keys).
- (uint16_t)otherButton:(NSEvent*)e {
  switch (e.buttonNumber) {
    case 2: return kMouseMiddle;
    case 3: return kMouseBack;
    case 4: return kMouseForward;
    default: return 0;
  }
}
- (void)otherMouseDown:(NSEvent*)e {
  if (uint16_t b = [self otherButton:e]; _captured && b) [self sendButton:b pressed:true];
}
- (void)otherMouseUp:(NSEvent*)e {
  if (uint16_t b = [self otherButton:e]; _captured && b) [self sendButton:b pressed:false];
}
- (void)otherMouseDragged:(NSEvent*)e {
  if (_captured) [self sendDelta:e];
}

// Scrolling: Android has no scroll wheel on phones, so a scroll becomes a finger
// dragging the content. Trackpads report gesture phases; wheels report notches,
// which end after a short pause.
- (void)scrollStage:(int)stage {
  if (_input && _input->scroll) {
    _input->scroll(stage, static_cast<int32_t>(_scrollPos.x), static_cast<int32_t>(_scrollPos.y));
  }
}
// A long scroll would drag the finger off the screen, where it stops moving the
// content: lift it and land again where the gesture started.
- (void)scrollMoved {
  if (_scrollPos.x < 0 || _scrollPos.y < 0 || _scrollPos.x >= _guestWidth || _scrollPos.y >= _guestHeight) {
    const NSPoint overshoot = NSMakePoint(_scrollPos.x - std::clamp<CGFloat>(_scrollPos.x, 0, _guestWidth - 1),
                                          _scrollPos.y - std::clamp<CGFloat>(_scrollPos.y, 0, _guestHeight - 1));
    _scrollPos.x -= overshoot.x;
    _scrollPos.y -= overshoot.y;
    [self scrollStage:1];
    [self scrollStage:2];
    _scrollPos = _scrollStart;
    [self scrollStage:0];
    return;
  }
  [self scrollStage:1];
}
- (void)endWheelScroll {
  if (!_scrolling || !_wheel) return;
  _scrolling = NO;
  [self scrollStage:2];
}
- (void)scrollWheel:(NSEvent*)e {
  if (_captured || !_input || !_input->scroll) return;
  if (e.momentumPhase != NSEventPhaseNone) return;  // Android does its own fling
  const CGFloat k = [self guestPerPoint];
  if (e.hasPreciseScrollingDeltas) {
    if (e.phase == NSEventPhaseBegan || (!_scrolling && e.phase == NSEventPhaseChanged)) {
      _scrolling = YES;
      _wheel = NO;
      _scrollPos = _scrollStart = [self guestPointForEvent:e];
      [self scrollStage:0];
    }
    if (!_scrolling) return;
    if (e.phase == NSEventPhaseChanged) {
      _scrollPos.x += e.scrollingDeltaX * k;
      _scrollPos.y += e.scrollingDeltaY * k;
      [self scrollMoved];
    } else if (e.phase == NSEventPhaseEnded || e.phase == NSEventPhaseCancelled) {
      _scrolling = NO;
      [self scrollStage:2];
    }
    return;
  }
  // Mouse wheel: each notch drags the content a fixed distance.
  if (!_scrolling) {
    _scrolling = YES;
    _wheel = YES;
    _scrollPos = _scrollStart = [self guestPointForEvent:e];
    [self scrollStage:0];
  }
  _scrollPos.x += e.scrollingDeltaX * 40;
  _scrollPos.y += e.scrollingDeltaY * 40;
  [self scrollMoved];
  [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(endWheelScroll) object:nil];
  [self performSelector:@selector(endWheelScroll) withObject:nil afterDelay:0.12];
}

- (void)magnifyWithEvent:(NSEvent*)e {
  if (_captured || !_input || !_input->pinch) return;
  NSPoint g = [self guestPointForEvent:e];
  if (e.phase == NSEventPhaseBegan) {
    _pinchScale = 1;
    _input->pinch(0, 1, static_cast<int32_t>(g.x), static_cast<int32_t>(g.y));
  } else if (e.phase == NSEventPhaseChanged) {
    _pinchScale = std::clamp(_pinchScale * (1 + e.magnification), 0.2, 5.0);
    _input->pinch(1, _pinchScale, static_cast<int32_t>(g.x), static_cast<int32_t>(g.y));
  } else if (e.phase == NSEventPhaseEnded || e.phase == NSEventPhaseCancelled) {
    _input->pinch(2, _pinchScale, static_cast<int32_t>(g.x), static_cast<int32_t>(g.y));
  }
}

- (void)sendKey:(NSEvent*)e pressed:(bool)pressed {
  if (!_input || !_input->key) return;
  if (auto code = mac_keycode_to_linux(e.keyCode)) _input->key(*code, pressed);
}
- (void)keyDown:(NSEvent*)e {
  if (e.modifierFlags & NSEventModifierFlagCommand) return;  // menu shortcuts, not Android keys
  if (!e.isARepeat) [self sendKey:e pressed:true];  // Android does its own key repeat
}
- (void)keyUp:(NSEvent*)e {
  if (e.modifierFlags & NSEventModifierFlagCommand) return;
  [self sendKey:e pressed:false];
}
- (void)flagsChanged:(NSEvent*)e {
  // Modifier keys only report the new flag state; compare with the last one.
  NSEventModifierFlags changed = e.modifierFlags ^ _lastFlags;
  _lastFlags = e.modifierFlags;
  const NSEventModifierFlags mask = NSEventModifierFlagShift | NSEventModifierFlagControl |
                                    NSEventModifierFlagOption | NSEventModifierFlagCapsLock;
  if (changed & mask) [self sendKey:e pressed:(e.modifierFlags & changed & mask) != 0];
}

// Drag and drop: APKs are installed, other files go to Android's Download folder.
- (NSArray<NSURL*>*)filesIn:(id<NSDraggingInfo>)info {
  return [info.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
  return [self filesIn:info].count ? NSDragOperationCopy : NSDragOperationNone;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
  NSArray<NSURL*>* files = [self filesIn:info];
  for (NSURL* u in files) {
    send_action(is_apk(u) ? WindowAction::kInstallApk : WindowAction::kShareFile, u.path.UTF8String);
  }
  return files.count > 0;
}
@end

// ---- control editor overlay ----

@interface KestrelEditorView : NSView
@property(nonatomic) uint32_t guestWidth;
@property(nonatomic) uint32_t guestHeight;
// action: ControlsEditorResult::Action; layout index or name as needed.
@property(nonatomic, copy) void (^onFinish)(NSInteger action, NSInteger layout, NSString* name);
- (void)setMarkers:(std::vector<ControlMarker>)markers title:(NSString*)title
           layouts:(NSArray<NSString*>*)layouts active:(NSInteger)active;
- (std::vector<ControlMarker>)markers;
@end

@implementation KestrelEditorView {
  std::vector<ControlMarker> _markers;
  NSInteger _dragging;
  NSInteger _selected;  // marker waiting for a key, or -1
  int _joyStep;         // joystick: which of up/left/down/right comes next
  NSTextField* _title;
  NSTextField* _help;
  NSPopUpButton* _layouts;
  NSInteger _activeLayout;
  BOOL _waitingSwitchKey;  // the next key switches to this layout
}

using EditorAction = ControlsEditorResult::Action;

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    _dragging = -1;
    _selected = -1;
    self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _title = [NSTextField labelWithString:@""];
    _title.textColor = NSColor.whiteColor;
    _title.font = [NSFont boldSystemFontOfSize:13];
    [self addSubview:_title];
    _help = [NSTextField labelWithString:@""];
    _help.textColor = [NSColor colorWithWhite:0.9 alpha:1];
    _help.font = [NSFont systemFontOfSize:12];
    _help.alignment = NSTextAlignmentCenter;
    [self addSubview:_help];
    _layouts = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _layouts.target = self;
    _layouts.action = @selector(layoutChosen:);
    [self addSubview:_layouts];
    const struct { NSString* title; SEL action; NSInteger tag; } buttons[] = {
        {@"+ Add Button", @selector(addButton:), 3}, {@"Cancel", @selector(cancel:), 2}, {@"Save", @selector(save:), 1}};
    for (const auto& b : buttons) {
      NSButton* btn = [NSButton buttonWithTitle:b.title target:self action:b.action];
      btn.tag = b.tag;
      if (b.tag == 1) btn.bezelColor = NSColor.systemBlueColor;
      [self addSubview:btn];
    }
  }
  return self;
}

- (void)layout {
  [super layout];
  const CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
  _title.frame = NSMakeRect(12, 12, std::max<CGFloat>(100, w - 560), 20);
  _layouts.frame = NSMakeRect(w - 530, 7, 170, 28);
  for (NSView* v in self.subviews) {
    if (![v isKindOfClass:NSButton.class] || v == _layouts) continue;
    NSButton* b = (NSButton*)v;
    const CGFloat x = b.tag == 1 ? w - 96 : b.tag == 2 ? w - 190 : w - 350;
    b.frame = NSMakeRect(x, 6, b.tag == 3 ? 150 : 86, 30);
  }
  _help.frame = NSMakeRect(12, h - 30, w - 24, 20);
}

- (void)updateHelp {
  if (_selected < 0 || _selected >= (NSInteger)_markers.size()) {
    _help.stringValue = @"Drag controls onto the game's buttons · Click one, then press a key to change it · "
                        @"Delete removes it · Right-click for more";
    return;
  }
  const auto& m = _markers[static_cast<size_t>(_selected)];
  static NSString* const dirs[] = {@"Up", @"Left", @"Down", @"Right"};
  switch (m.kind) {
    case ControlMarker::Kind::kJoystick:
      _help.stringValue = [NSString stringWithFormat:@"Press the key for %@ (%d of 4)", dirs[_joyStep], _joyStep + 1];
      break;
    case ControlMarker::Kind::kAim:
      _help.stringValue = @"Press the key that turns aiming on and off";
      break;
    case ControlMarker::Kind::kFire:
      _help.stringValue = @"Fire is the left mouse button while aiming — drag it onto the game's fire button";
      break;
    case ControlMarker::Kind::kTap:
      _help.stringValue = [NSString stringWithFormat:@"Press a key for “%@” · Delete removes it · Esc to finish",
                                                     ns(m.name.empty() ? std::string("Button") : m.name)];
      break;
  }
}

// What the marker says: its job and key.
- (NSString*)labelFor:(const ControlMarker&)m {
  switch (m.kind) {
    case ControlMarker::Kind::kJoystick:
      return [NSString stringWithFormat:@"Move (%@ %@ %@ %@)", ns(key_hint(m.joy[0])), ns(key_hint(m.joy[1])),
                                        ns(key_hint(m.joy[2])), ns(key_hint(m.joy[3]))];
    case ControlMarker::Kind::kAim:
      return [NSString stringWithFormat:@"Aim on/off (%@)", ns(key_hint(m.key))];
    case ControlMarker::Kind::kFire:
      return @"Fire (click)";
    case ControlMarker::Kind::kTap: {
      NSString* name = ns(m.name.empty() ? std::string("Button") : m.name);
      if (!m.key) return [NSString stringWithFormat:@"%@ (press a key)", name];
      return [NSString stringWithFormat:@"%@ (%@%@)", name, m.hold_to_use ? @"hold " : @"", ns(key_hint(m.key))];
    }
  }
  return @"";
}

- (void)setMarkers:(std::vector<ControlMarker>)markers title:(NSString*)title
           layouts:(NSArray<NSString*>*)layouts active:(NSInteger)active {
  _markers = std::move(markers);
  _selected = -1;
  _title.stringValue = [NSString stringWithFormat:@"%@ controls", title];
  [_layouts removeAllItems];
  for (NSString* l in layouts) [_layouts addItemWithTitle:[@"Layout: " stringByAppendingString:l]];
  [_layouts.menu addItem:NSMenuItem.separatorItem];
  [_layouts addItemWithTitle:@"New Layout…"];
  [_layouts addItemWithTitle:@"Rename Layout…"];
  [_layouts addItemWithTitle:@"Delete Layout"];
  [_layouts addItemWithTitle:@"Switch Key for This Layout…"];
  _activeLayout = active;
  [_layouts selectItemAtIndex:active];
  [self updateHelp];
  [self setNeedsDisplay:YES];
}
- (std::vector<ControlMarker>)markers { return _markers; }

- (NSRect)gameRect {
  const CGFloat vw = self.bounds.size.width, vh = self.bounds.size.height;
  const CGFloat scale = std::min(vw / _guestWidth, vh / _guestHeight);
  return NSMakeRect((vw - _guestWidth * scale) / 2, (vh - _guestHeight * scale) / 2, _guestWidth * scale,
                    _guestHeight * scale);
}
- (NSPoint)pointFor:(const ControlMarker&)m {
  NSRect g = [self gameRect];
  return NSMakePoint(g.origin.x + m.x * g.size.width, g.origin.y + m.y * g.size.height);
}

- (void)drawRect:(NSRect)dirty {
  [[NSColor colorWithWhite:0 alpha:0.35] setFill];
  NSRectFill(self.bounds);
  [[NSColor colorWithWhite:0 alpha:0.7] setFill];
  NSRectFill(NSMakeRect(0, 0, self.bounds.size.width, 42));
  NSRectFill(NSMakeRect(0, self.bounds.size.height - 38, self.bounds.size.width, 38));
  NSDictionary* attrs = @{NSFontAttributeName : [NSFont boldSystemFontOfSize:11],
                          NSForegroundColorAttributeName : NSColor.whiteColor};
  for (size_t i = 0; i < _markers.size(); ++i) {
    const auto& m = _markers[i];
    NSPoint p = [self pointFor:m];
    const CGFloat r = m.kind == ControlMarker::Kind::kJoystick ? 38 : 20;
    const bool unbound = m.kind == ControlMarker::Kind::kTap && !m.key;
    NSColor* color = unbound                                   ? NSColor.systemRedColor
                     : m.kind == ControlMarker::Kind::kAim    ? NSColor.systemBlueColor
                     : m.kind == ControlMarker::Kind::kFire   ? NSColor.systemRedColor
                                                              : NSColor.systemOrangeColor;
    [[color colorWithAlphaComponent:0.55] setFill];
    NSBezierPath* circle = [NSBezierPath bezierPathWithOvalInRect:NSMakeRect(p.x - r, p.y - r, 2 * r, 2 * r)];
    [circle fill];
    const bool selected = static_cast<NSInteger>(i) == _selected;
    [(selected ? NSColor.systemYellowColor : NSColor.whiteColor) setStroke];
    circle.lineWidth = selected ? 4 : 2;
    if (unbound) {
      const CGFloat dash[] = {5, 4};
      [circle setLineDash:dash count:2 phase:0];
    }
    [circle stroke];
    NSString* label = [self labelFor:m];
    NSSize size = [label sizeWithAttributes:attrs];
    const NSRect bg = NSMakeRect(p.x - size.width / 2 - 4, p.y + r + 2, size.width + 8, size.height + 2);
    [[NSColor colorWithWhite:0 alpha:0.55] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:bg xRadius:4 yRadius:4] fill];
    [label drawAtPoint:NSMakePoint(p.x - size.width / 2, p.y + r + 3) withAttributes:attrs];
  }
}

- (NSInteger)markerAt:(NSPoint)p {
  NSInteger found = -1;
  CGFloat best = 45;  // grab radius in points
  for (size_t i = 0; i < _markers.size(); ++i) {
    NSPoint c = [self pointFor:_markers[i]];
    const CGFloat d = std::hypot(c.x - p.x, c.y - p.y);
    if (d < best) {
      best = d;
      found = static_cast<NSInteger>(i);
    }
  }
  return found;
}

- (void)select:(NSInteger)index {
  _selected = index;
  _joyStep = 0;
  [self updateHelp];
  [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent*)e {
  [self.window makeFirstResponder:self];
  NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  if (p.y < 42 || p.y > self.bounds.size.height - 38) return;  // the bars
  _dragging = [self markerAt:p];
  [self select:_dragging];
}
- (void)mouseDragged:(NSEvent*)e {
  if (_dragging < 0) return;
  NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  NSRect g = [self gameRect];
  auto& m = _markers[static_cast<size_t>(_dragging)];
  m.x = std::clamp((p.x - g.origin.x) / g.size.width, 0.0, 1.0);
  m.y = std::clamp((p.y - g.origin.y) / g.size.height, 0.0, 1.0);
  [self setNeedsDisplay:YES];
}
- (void)mouseUp:(NSEvent*)e { _dragging = -1; }

// Middle and side mouse buttons bind like keys.
- (void)otherMouseDown:(NSEvent*)e {
  const uint16_t b = e.buttonNumber == 2 ? kMouseMiddle : e.buttonNumber == 3 ? kMouseBack
                     : e.buttonNumber == 4 ? kMouseForward : 0;
  if (!b) return;
  if (_waitingSwitchKey) [self finishSwitchKey:b];
  else [self assignKey:b];
}

- (void)rightMouseDown:(NSEvent*)e {
  NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
  const NSInteger i = [self markerAt:p];
  if (i < 0) return;
  [self select:i];
  if (_markers[static_cast<size_t>(i)].kind != ControlMarker::Kind::kTap) return;
  NSMenu* menu = [[NSMenu alloc] init];
  [menu addItemWithTitle:@"Rename…" action:@selector(renameSelected:) keyEquivalent:@""].target = self;
  [menu addItemWithTitle:@"Use Right Mouse Button" action:@selector(rightButtonSelected:) keyEquivalent:@""].target = self;
  NSMenuItem* hold = [menu addItemWithTitle:@"Hold to Use (on/off buttons like the scope)"
                                     action:@selector(toggleHoldToUse:)
                              keyEquivalent:@""];
  hold.target = self;
  hold.state = _markers[static_cast<size_t>(i)].hold_to_use ? NSControlStateValueOn : NSControlStateValueOff;
  [menu addItem:NSMenuItem.separatorItem];
  [menu addItemWithTitle:@"Delete" action:@selector(deleteSelected:) keyEquivalent:@""].target = self;
  [NSMenu popUpContextMenu:menu withEvent:e forView:self];
}

// A key already used elsewhere moves to the selected control (the old one loses it).
- (BOOL)assignKey:(uint16_t)code {
  if (_selected < 0) return NO;
  auto& m = _markers[static_cast<size_t>(_selected)];
  if (m.kind == ControlMarker::Kind::kFire) return NO;
  for (size_t i = 0; i < _markers.size(); ++i) {
    if (static_cast<NSInteger>(i) == _selected) continue;
    auto& o = _markers[i];
    const bool clash = (o.kind == ControlMarker::Kind::kTap && o.key == code) ||
                       (o.kind == ControlMarker::Kind::kAim && o.key == code) ||
                       (o.kind == ControlMarker::Kind::kJoystick && std::ranges::find(o.joy, code) != o.joy.end());
    if (!clash) continue;
    if (o.kind == ControlMarker::Kind::kTap) {
      o.key = 0;  // show it as needing a key
    } else {
      NSBeep();   // movement and aim keys stay; pick another key
      _help.stringValue = [NSString stringWithFormat:@"%@ is used for %@ — pick another key", ns(key_hint(code)),
                                                     o.kind == ControlMarker::Kind::kAim ? @"aiming" : @"moving"];
      return YES;
    }
  }
  if (m.kind == ControlMarker::Kind::kJoystick) {
    m.joy[static_cast<size_t>(_joyStep)] = code;
    if (++_joyStep == 4) [self select:-1];
    else [self updateHelp];
  } else {
    m.key = code;
    [self select:-1];
  }
  [self setNeedsDisplay:YES];
  return YES;
}

- (void)finishSwitchKey:(uint16_t)code {
  _waitingSwitchKey = NO;
  if (_onFinish) _onFinish(static_cast<NSInteger>(EditorAction::kSetSwitchKey), code, nil);
}

- (void)keyDown:(NSEvent*)e {
  if (_waitingSwitchKey) {
    if (e.keyCode == 53) {
      _waitingSwitchKey = NO;
      [self updateHelp];
    } else if (e.keyCode == 51 || e.keyCode == 117) {
      [self finishSwitchKey:0];
    } else if (auto code = mac_keycode_to_linux(e.keyCode)) {
      [self finishSwitchKey:*code];
    }
    return;
  }
  if (e.keyCode == 53) {  // Esc: finish choosing, or leave the editor
    if (_selected >= 0) [self select:-1];
    else if (_onFinish) _onFinish(static_cast<NSInteger>(EditorAction::kCancel), 0, nil);
    return;
  }
  if ((e.keyCode == 51 || e.keyCode == 117) && _selected >= 0 &&
      _markers[static_cast<size_t>(_selected)].kind == ControlMarker::Kind::kTap) {
    [self deleteSelected:nil];
    return;
  }
  if (e.modifierFlags & NSEventModifierFlagCommand) {
    [super keyDown:e];
    return;
  }
  if (auto code = mac_keycode_to_linux(e.keyCode)) {
    if ([self assignKey:*code]) return;
  }
  [super keyDown:e];
}
- (void)flagsChanged:(NSEvent*)e {
  // Modifier keys (Shift, Option, Control) can be bound too: on press.
  const NSEventModifierFlags mods = NSEventModifierFlagShift | NSEventModifierFlagOption | NSEventModifierFlagControl;
  if (!(e.modifierFlags & mods)) return;
  if (auto code = mac_keycode_to_linux(e.keyCode)) [self assignKey:*code];
}

- (NSString*)askName:(NSString*)title value:(NSString*)value {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = title;
  NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 24)];
  field.stringValue = value;
  alert.accessoryView = field;
  [alert addButtonWithTitle:@"OK"];
  [alert addButtonWithTitle:@"Cancel"];
  alert.window.initialFirstResponder = field;
  if ([alert runModal] != NSAlertFirstButtonReturn) return nil;
  NSString* v = [field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
  return v.length ? v : nil;
}

- (void)renameSelected:(id)sender {
  if (_selected < 0) return;
  auto& m = _markers[static_cast<size_t>(_selected)];
  if (NSString* name = [self askName:@"Name this button" value:ns(m.name)]) m.name = name.UTF8String;
  [self updateHelp];
  [self setNeedsDisplay:YES];
}
- (void)rightButtonSelected:(id)sender { [self assignKey:kMouseRight]; }
// The key taps the button when pressed and again when released: e.g. scoped in only
// while the right button is held (Free Fire's scope button is on/off).
- (void)toggleHoldToUse:(id)sender {
  if (_selected < 0) return;
  auto& m = _markers[static_cast<size_t>(_selected)];
  m.hold_to_use = !m.hold_to_use;
  [self select:-1];
}
- (void)deleteSelected:(id)sender {
  if (_selected < 0) return;
  _markers.erase(_markers.begin() + _selected);
  [self select:-1];
}
- (void)addButton:(id)sender {
  ControlMarker m{ControlMarker::Kind::kTap, _markers.size(), "", 0.5, 0.5, "Button", 0, {}};
  _markers.push_back(m);
  [self select:static_cast<NSInteger>(_markers.size() - 1)];
  [self.window makeFirstResponder:self];
}

- (void)layoutChosen:(NSPopUpButton*)sender {
  const NSInteger i = sender.indexOfSelectedItem;
  const NSInteger count = sender.numberOfItems - 5;  // layouts, separator, 4 commands
  [sender selectItemAtIndex:_activeLayout];
  if (!_onFinish) return;
  if (i >= 0 && i < count) {
    if (i != _activeLayout) _onFinish(static_cast<NSInteger>(EditorAction::kSelectLayout), i, nil);
  } else if (i == count + 1) {
    if (NSString* name = [self askName:@"Name the new layout" value:@"My layout"]) {
      _onFinish(static_cast<NSInteger>(EditorAction::kNewLayout), 0, name);
    }
  } else if (i == count + 2) {
    NSString* current = [[sender itemAtIndex:_activeLayout].title stringByReplacingOccurrencesOfString:@"Layout: " withString:@""];
    if (NSString* name = [self askName:@"Rename this layout" value:current]) {
      _onFinish(static_cast<NSInteger>(EditorAction::kRenameLayout), 0, name);
    }
  } else if (i == count + 3) {
    _onFinish(static_cast<NSInteger>(EditorAction::kDeleteLayout), 0, nil);
  } else if (i == count + 4) {
    [self select:-1];
    _waitingSwitchKey = YES;
    _help.stringValue = @"Press the key (or mouse side button) that switches the game to this layout — "
                        @"e.g. when you get in a vehicle · Delete: no key · Esc: cancel";
    [self.window makeFirstResponder:self];
  }
}

- (void)save:(id)sender {
  if (_onFinish) _onFinish(static_cast<NSInteger>(EditorAction::kSave), 0, nil);
}
- (void)cancel:(id)sender {
  if (_onFinish) _onFinish(static_cast<NSInteger>(EditorAction::kCancel), 0, nil);
}
@end

// ---- toolbar ----

@interface KestrelSidebar : NSView
@end

@implementation KestrelSidebar {
  NSMutableArray<NSButton*>* _buttons;
}

namespace {
NSString* const kAlwaysHintsKey = @"AlwaysShowKeyHints";
void set_always_hints(bool on);
void choose_files_and_send(WindowAction action);
std::function<void(bool)> g_set_sidebar;  // shows/hides the toolbar (set by the window)
bool g_sidebar_shown = true;
}  // namespace

struct ToolbarItem {
  NSString* symbol;
  NSString* tip;
  WindowAction action;  // kSettings etc.
  bool fullscreen;      // toggles full screen instead of sending an action
};

- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.wantsLayer = YES;
    self.layer.backgroundColor = [NSColor colorWithWhite:0.12 alpha:1].CGColor;
    _buttons = [NSMutableArray array];
    const ToolbarItem items[] = {
        {@"square.grid.2x2", @"Library: your games and apps (⌘L)", WindowAction::kShowLibrary, false},
        {nil, nil, WindowAction::kHome, false},
        {@"arrow.uturn.backward", @"Back (right click, ⌘[)", WindowAction::kBack, false},
        {@"house", @"Home (⇧⌘H)", WindowAction::kHome, false},
        {@"square.on.square", @"Recent apps — swipe an app away to close it", WindowAction::kRecents, false},
        {@"xmark.app", @"Close the current app", WindowAction::kCloseApp, false},
        {nil, nil, WindowAction::kHome, false},  // spacer
        {@"speaker.wave.3", @"Volume up", WindowAction::kVolumeUp, false},
        {@"speaker.wave.1", @"Volume down", WindowAction::kVolumeDown, false},
        {nil, nil, WindowAction::kHome, false},
        {@"bag", @"App Store", WindowAction::kAppStore, false},
        {@"arrow.down.app", @"Install APK (⌘O) — or drop it on the window", WindowAction::kInstallApk, false},
        {@"folder", @"Send files to Android (Download folder)", WindowAction::kShareFile, false},
        {nil, nil, WindowAction::kHome, false},
        {@"gamecontroller", @"Game controls on/off (⌘K)", WindowAction::kToggleGameControls, false},
        {@"keyboard", @"Edit game controls (⇧⌘K)", WindowAction::kEditGameControls, false},
        {@"eye", @"Key hints: always show / hide (⌘/)", WindowAction::kHome, false},
        {@"camera", @"Screenshot to Desktop (⌘S)", WindowAction::kScreenshot, false},
        {@"record.circle", @"Record the screen with sound (⇧⌘R) — saved in Movies › Kestrel", WindowAction::kToggleRecording, false},
        {@"arrow.up.left.and.arrow.down.right", @"Full screen (⌃⌘F)", WindowAction::kHome, true},
        {@"gearshape", @"Settings (⌘,)", WindowAction::kSettings, false},
        {nil, nil, WindowAction::kHome, false},
        {@"sidebar.right", @"Hide the toolbar (⌥⌘T) — the tab on the right edge brings it back",
         WindowAction::kHome, false},
    };
    for (const auto& it : items) {
      if (!it.symbol) {
        NSBox* sep = [[NSBox alloc] init];
        sep.boxType = NSBoxSeparator;
        [self addSubview:sep];
        continue;
      }
      NSImage* img = [NSImage imageWithSystemSymbolName:it.symbol accessibilityDescription:it.tip];
      NSButton* b = [NSButton buttonWithImage:img target:self action:@selector(pressed:)];
      b.bordered = NO;
      b.contentTintColor = [NSColor colorWithWhite:0.85 alpha:1];
      b.toolTip = it.tip;
      b.tag = it.fullscreen                                   ? -1
              : [it.symbol isEqualToString:@"eye"]            ? -2
              : [it.symbol isEqualToString:@"sidebar.right"]  ? -3
                                                              : static_cast<NSInteger>(it.action);
      b.imageScaling = NSImageScaleProportionallyUpOrDown;
      [self addSubview:b];
      [_buttons addObject:b];
    }
  }
  return self;
}

- (BOOL)isFlipped { return YES; }

- (void)layout {
  [super layout];
  const CGFloat w = self.bounds.size.width;
  // Shrink the spacing (then the buttons) so every button fits a short window.
  CGFloat natural = 24;
  for (NSView* v in self.subviews) natural += [v isKindOfClass:NSBox.class] ? 10 : 40;
  const CGFloat k = std::min<CGFloat>(1, self.bounds.size.height / natural);
  const CGFloat size = std::max<CGFloat>(18, 30 * std::min<CGFloat>(1, k * 1.15));
  CGFloat y = 12 * k;
  for (NSView* v in self.subviews) {
    if ([v isKindOfClass:NSBox.class]) {
      v.frame = NSMakeRect(10, y + 4 * k, w - 20, 1);
      y += 10 * k;
    } else {
      v.frame = NSMakeRect((w - size) / 2, y, size, size);
      y += 40 * k;
    }
  }
}

- (void)pressed:(NSButton*)b {
  if (b.tag == -1) {
    [self.window toggleFullScreen:nil];
    return;
  }
  if (b.tag == -2) {
    set_always_hints(![NSUserDefaults.standardUserDefaults boolForKey:kAlwaysHintsKey]);
    return;
  }
  if (b.tag == -3) {
    if (g_set_sidebar) g_set_sidebar(false);
    return;
  }
  const auto action = static_cast<WindowAction>(b.tag);
  if (action == WindowAction::kInstallApk || action == WindowAction::kShareFile) {
    choose_files_and_send(action);
    return;
  }
  send_action(action);
}
@end

// ---- content: game view + toolbar ----

// FPS readout over the game (Window > Show FPS). Clicks pass through to the game.
@interface KestrelFpsLabel : NSTextField
@end
@implementation KestrelFpsLabel
- (NSView*)hitTest:(NSPoint)point { return nil; }
@end

// Startup screen shown over the game while Android boots.
@interface KestrelSplashView : NSView
@property(nonatomic, strong) NSTextField* status;
@property(nonatomic, strong) NSProgressIndicator* bar;
@end
@implementation KestrelSplashView
- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.wantsLayer = YES;
    self.layer.backgroundColor = [NSColor colorWithRed:0.07 green:0.08 blue:0.16 alpha:1].CGColor;
    NSImage* app_icon = [NSImage imageNamed:@"Kestrel"];
    if (!app_icon) app_icon = [NSImage imageNamed:NSImageNameApplicationIcon];
    NSImageView* icon = [NSImageView imageViewWithImage:app_icon];
    icon.identifier = @"icon";
    [self addSubview:icon];
    NSTextField* title = [NSTextField labelWithString:@"Kestrel"];
    title.font = [NSFont systemFontOfSize:30 weight:NSFontWeightBold];
    title.textColor = NSColor.whiteColor;
    title.alignment = NSTextAlignmentCenter;
    title.identifier = @"title";
    [self addSubview:title];
    _status = [NSTextField labelWithString:@"Starting…"];
    _status.font = [NSFont systemFontOfSize:13];
    _status.textColor = [NSColor colorWithWhite:0.8 alpha:1];
    _status.alignment = NSTextAlignmentCenter;
    [self addSubview:_status];
    _bar = [[NSProgressIndicator alloc] init];
    _bar.indeterminate = NO;
    _bar.minValue = 0;
    _bar.maxValue = 1;
    _bar.style = NSProgressIndicatorStyleBar;
    [self addSubview:_bar];
  }
  return self;
}
- (NSView*)hitTest:(NSPoint)point { return self.hidden ? nil : self; }  // swallow clicks while booting
- (void)mouseDown:(NSEvent*)e {}
- (void)layout {
  [super layout];
  const NSRect b = self.bounds;
  const CGFloat cx = NSMidX(b), cy = NSMidY(b);
  for (NSView* v in self.subviews) {
    if ([v.identifier isEqualToString:@"icon"]) v.frame = NSMakeRect(cx - 64, cy + 10, 128, 128);
    if ([v.identifier isEqualToString:@"title"]) v.frame = NSMakeRect(cx - 150, cy - 34, 300, 40);
  }
  _status.frame = NSMakeRect(cx - 200, cy - 62, 400, 20);
  _bar.frame = NSMakeRect(cx - 140, cy - 90, 280, 12);
}
@end

@interface KestrelFlippedView : NSView
@end
@implementation KestrelFlippedView
- (BOOL)isFlipped { return YES; }
@end

// ---- Library: installed games and apps as big tiles ----

// The app's icon as Android's launcher would draw it: a whole image, or an adaptive
// icon's background (image or color) and foreground layers, in a rounded square.
NSImage* library_icon(const LibraryItem& item, CGFloat size) {
  NSImage* whole = item.icon_file.empty() ? nil : [[NSImage alloc] initWithContentsOfFile:ns(item.icon_file)];
  NSImage* fg = item.foreground_file.empty() ? nil : [[NSImage alloc] initWithContentsOfFile:ns(item.foreground_file)];
  NSImage* bg = item.background_file.empty() ? nil : [[NSImage alloc] initWithContentsOfFile:ns(item.background_file)];
  return [NSImage imageWithSize:NSMakeSize(size, size) flipped:NO drawingHandler:^BOOL(NSRect r) {
    NSBezierPath* clip = [NSBezierPath bezierPathWithRoundedRect:r xRadius:size * 0.22 yRadius:size * 0.22];
    [clip addClip];
    // Adaptive layers are 108dp with the visible 72dp in the middle: draw them 1.5x.
    const NSRect layer = NSInsetRect(r, -size * 0.25, -size * 0.25);
    if (whole) {
      [whole drawInRect:r];
    } else if (fg || bg || item.background_color) {
      if (item.background_color) {
        const uint32_t c = *item.background_color;
        [[NSColor colorWithSRGBRed:((c >> 16) & 0xff) / 255.0 green:((c >> 8) & 0xff) / 255.0
                              blue:(c & 0xff) / 255.0 alpha:1] setFill];
        NSRectFill(r);
      } else if (bg) {
        [bg drawInRect:layer];
      } else {
        [NSColor.whiteColor setFill];
        NSRectFill(r);
      }
      if (fg) [fg drawInRect:layer];
    } else {
      // No usable icon: the first letter on a color picked from the name.
      const size_t h = std::hash<std::string>{}(item.label);
      NSColor* a = [NSColor colorWithHue:(h % 360) / 360.0 saturation:0.55 brightness:0.85 alpha:1];
      NSColor* b = [NSColor colorWithHue:((h + 40) % 360) / 360.0 saturation:0.7 brightness:0.6 alpha:1];
      [[[NSGradient alloc] initWithStartingColor:a endingColor:b] drawInRect:r angle:-90];
      NSString* letter = item.label.empty() ? @"?" : [ns(item.label) substringToIndex:1].uppercaseString;
      NSDictionary* attrs = @{NSFontAttributeName : [NSFont systemFontOfSize:size * 0.5 weight:NSFontWeightBold],
                              NSForegroundColorAttributeName : NSColor.whiteColor};
      const NSSize ls = [letter sizeWithAttributes:attrs];
      [letter drawAtPoint:NSMakePoint(NSMidX(r) - ls.width / 2, NSMidY(r) - ls.height / 2) withAttributes:attrs];
    }
    return YES;
  }];
}

@interface KestrelTile : NSView
@property(nonatomic, copy) void (^onClick)(void);
@property(nonatomic) BOOL hovered;
@end
@implementation KestrelTile
- (BOOL)isFlipped { return YES; }
- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  for (NSTrackingArea* t in self.trackingAreas) [self removeTrackingArea:t];
  [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
                                                     options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow
                                                       owner:self
                                                    userInfo:nil]];
}
- (void)mouseEntered:(NSEvent*)e {
  _hovered = YES;
  [self setNeedsDisplay:YES];
}
- (void)mouseExited:(NSEvent*)e {
  _hovered = NO;
  [self setNeedsDisplay:YES];
}
- (void)mouseDown:(NSEvent*)e {}
- (void)mouseUp:(NSEvent*)e {
  if (_onClick && NSPointInRect([self convertPoint:e.locationInWindow fromView:nil], self.bounds)) _onClick();
}
- (void)drawRect:(NSRect)dirty {
  if (!_hovered) return;
  [[NSColor colorWithWhite:1 alpha:0.1] setFill];
  [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 2, 2) xRadius:14 yRadius:14] fill];
}
@end

@interface KestrelLibraryView : NSView
@property(nonatomic, copy) void (^onLaunch)(NSString* component);
@property(nonatomic, copy) void (^onAction)(NSInteger action);  // WindowAction
- (void)setItems:(std::vector<LibraryItem>)items loading:(BOOL)loading;
@end
@implementation KestrelLibraryView {
  NSScrollView* _scroll;
  NSView* _grid;
  NSTextField* _title;
  NSTextField* _subtitle;
  NSProgressIndicator* _spinner;
  NSArray<NSButton*>* _buttons;
  std::vector<LibraryItem> _items;
}
- (instancetype)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    self.wantsLayer = YES;
    self.layer.backgroundColor = [NSColor colorWithRed:0.06 green:0.07 blue:0.13 alpha:0.97].CGColor;
    _title = [NSTextField labelWithString:@"Library"];
    _title.font = [NSFont systemFontOfSize:26 weight:NSFontWeightBold];
    _title.textColor = NSColor.whiteColor;
    [self addSubview:_title];
    _subtitle = [NSTextField labelWithString:@"Click a game to play. Esc closes the Library."];
    _subtitle.font = [NSFont systemFontOfSize:13];
    _subtitle.textColor = [NSColor colorWithWhite:0.7 alpha:1];
    [self addSubview:_subtitle];
    NSMutableArray* buttons = [NSMutableArray array];
    const struct { NSString* title; NSString* symbol; WindowAction action; } defs[] = {
        {@"App Store", @"bag", WindowAction::kAppStore},
        {@"Install APK", @"arrow.down.app", WindowAction::kInstallApk},
        {@"Close", @"xmark", WindowAction::kHome},  // kHome here means "close the Library"
    };
    for (const auto& d : defs) {
      NSButton* b = [NSButton buttonWithTitle:d.title target:self action:@selector(button:)];
      b.image = [NSImage imageWithSystemSymbolName:d.symbol accessibilityDescription:d.title];
      b.imagePosition = NSImageLeading;
      b.bezelStyle = NSBezelStyleRounded;
      b.tag = static_cast<NSInteger>(d.action);
      [self addSubview:b];
      [buttons addObject:b];
    }
    _buttons = buttons;
    _scroll = [[NSScrollView alloc] init];
    _scroll.drawsBackground = NO;
    _scroll.hasVerticalScroller = YES;
    _grid = [[KestrelFlippedView alloc] init];
    _scroll.documentView = _grid;
    [self addSubview:_scroll];
    _spinner = [[NSProgressIndicator alloc] init];
    _spinner.style = NSProgressIndicatorStyleSpinning;
    [self addSubview:_spinner];
  }
  return self;
}
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)e {
  if (e.keyCode == 53) {  // Esc
    self.hidden = YES;
    [self.window makeFirstResponder:self.superview];
    return;
  }
  [super keyDown:e];
}
- (void)mouseDown:(NSEvent*)e {}  // clicks never reach the game underneath
- (void)button:(NSButton*)b {
  if (b.tag == static_cast<NSInteger>(WindowAction::kHome)) {
    self.hidden = YES;
    [self.window makeFirstResponder:self.superview];
    return;
  }
  if (_onAction) _onAction(b.tag);
}
- (void)setItems:(std::vector<LibraryItem>)items loading:(BOOL)loading {
  _items = std::move(items);
  if (loading) [_spinner startAnimation:nil];
  else [_spinner stopAnimation:nil];
  _spinner.hidden = !loading;
  for (NSView* v in [_grid.subviews copy]) [v removeFromSuperview];
  for (const auto& item : _items) {
    KestrelTile* tile = [[KestrelTile alloc] init];
    NSImageView* icon = [NSImageView imageViewWithImage:library_icon(item, 88)];
    icon.frame = NSMakeRect(22, 12, 88, 88);
    [tile addSubview:icon];
    NSTextField* name = [NSTextField wrappingLabelWithString:ns(item.label)];
    name.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    name.textColor = NSColor.whiteColor;
    name.alignment = NSTextAlignmentCenter;
    name.maximumNumberOfLines = 2;
    name.frame = NSMakeRect(4, 104, 124, 34);
    [tile addSubview:name];
    if (item.has_controls) {
      NSTextField* badge = [NSTextField labelWithString:@"🎮 Controls ready"];
      badge.font = [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold];
      badge.textColor = NSColor.systemOrangeColor;
      badge.alignment = NSTextAlignmentCenter;
      badge.frame = NSMakeRect(4, 138, 124, 14);
      [tile addSubview:badge];
    }
    NSString* component = ns(item.component);
    __weak KestrelLibraryView* weak = self;
    tile.onClick = ^{
      KestrelLibraryView* s = weak;
      if (s.onLaunch) s.onLaunch(component);
    };
    [_grid addSubview:tile];
  }
  if (!loading && _items.empty()) {
    _subtitle.stringValue = @"No apps yet. Get games from the App Store, or drop an APK on the window.";
  } else {
    _subtitle.stringValue = @"Click a game to play. Esc closes the Library.";
  }
  self.needsLayout = YES;
}
- (void)layout {
  [super layout];
  const NSRect b = self.bounds;
  const CGFloat pad = 32;
  _title.frame = NSMakeRect(pad, b.size.height - 60, 300, 34);
  _subtitle.frame = NSMakeRect(pad, b.size.height - 84, b.size.width - 2 * pad, 18);
  CGFloat x = b.size.width - pad;
  for (NSButton* btn in _buttons.reverseObjectEnumerator) {
    [btn sizeToFit];
    const CGFloat w = btn.frame.size.width + 10;
    x -= w;
    btn.frame = NSMakeRect(x, b.size.height - 58, w, 30);
    x -= 8;
  }
  _scroll.frame = NSMakeRect(pad, 16, b.size.width - 2 * pad, b.size.height - 110);
  _spinner.frame = NSMakeRect(NSMidX(b) - 16, NSMidY(b) - 16, 32, 32);
  const CGFloat tw = 132, th = 160, gap = 16;
  const CGFloat width = _scroll.contentSize.width;
  const NSUInteger cols = std::max<NSUInteger>(1, static_cast<NSUInteger>((width + gap) / (tw + gap)));
  NSUInteger i = 0;
  for (NSView* tile in _grid.subviews) {
    tile.frame = NSMakeRect((i % cols) * (tw + gap), (i / cols) * (th + gap), tw, th);
    ++i;
  }
  const NSUInteger rows = (i + cols - 1) / cols;
  _grid.frame = NSMakeRect(0, 0, width, std::max<CGFloat>(rows * (th + gap), _scroll.contentSize.height));
}
@end

// Key hints over the game: the key for each control, drawn where it taps. Clicks
// pass through to the game.
@interface KestrelHintsView : NSView
@property(nonatomic) uint32_t guestWidth;
@property(nonatomic) uint32_t guestHeight;
- (void)setMarkers:(std::vector<ControlMarker>)markers;
- (BOOL)hasMarkers;
@end
@implementation KestrelHintsView {
  std::vector<ControlMarker> _markers;
}
- (NSView*)hitTest:(NSPoint)point { return nil; }
- (BOOL)isFlipped { return YES; }
- (void)setMarkers:(std::vector<ControlMarker>)markers {
  _markers = std::move(markers);
  [self setNeedsDisplay:YES];
}
- (BOOL)hasMarkers { return !_markers.empty(); }
- (void)pill:(NSString*)text at:(NSPoint)c tint:(NSColor*)tint {
  NSDictionary* attrs = @{NSFontAttributeName : [NSFont systemFontOfSize:13 weight:NSFontWeightBold],
                          NSForegroundColorAttributeName : NSColor.whiteColor};
  const NSSize ts = [text sizeWithAttributes:attrs];
  const CGFloat h = 24, w = std::max<CGFloat>(h, ts.width + 14);
  const NSRect r = NSMakeRect(c.x - w / 2, c.y - h / 2, w, h);
  NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:r xRadius:h / 2 yRadius:h / 2];
  [[tint colorWithAlphaComponent:0.78] setFill];
  [path fill];
  [[NSColor colorWithWhite:1 alpha:0.9] setStroke];
  path.lineWidth = 1.5;
  [path stroke];
  [text drawAtPoint:NSMakePoint(c.x - ts.width / 2, c.y - ts.height / 2) withAttributes:attrs];
}
- (void)drawRect:(NSRect)dirty {
  if (!_guestWidth || !_guestHeight) return;
  const CGFloat vw = self.bounds.size.width, vh = self.bounds.size.height;
  const CGFloat scale = std::min(vw / _guestWidth, vh / _guestHeight);
  const CGFloat gx = (vw - _guestWidth * scale) / 2, gy = (vh - _guestHeight * scale) / 2;
  NSColor* dark = [NSColor colorWithWhite:0.08 alpha:1];
  for (const auto& m : _markers) {
    const NSPoint c = NSMakePoint(gx + m.x * _guestWidth * scale, gy + m.y * _guestHeight * scale);
    NSString* text = [NSString stringWithUTF8String:m.label.c_str()];
    if (m.kind == ControlMarker::Kind::kJoystick) {
      // "up left down right" around the stick.
      NSArray<NSString*>* k = [text componentsSeparatedByString:@" "];
      if (k.count == 4) {
        const CGFloat d = 46;
        [self pill:k[0] at:NSMakePoint(c.x, c.y - d) tint:dark];
        [self pill:k[1] at:NSMakePoint(c.x - d, c.y) tint:dark];
        [self pill:k[2] at:NSMakePoint(c.x, c.y + d) tint:dark];
        [self pill:k[3] at:NSMakePoint(c.x + d, c.y) tint:dark];
        continue;
      }
    }
    NSColor* tint = m.kind == ControlMarker::Kind::kFire  ? NSColor.systemRedColor
                    : m.kind == ControlMarker::Kind::kAim ? NSColor.systemBlueColor
                                                          : dark;
    [self pill:text at:c tint:tint];
  }
}
@end

namespace {
KestrelSplashView* g_splash = nil;
KestrelLibraryView* g_library = nil;

// Asks for files, then sends them with `action` (Install APK / send files).
void choose_files_and_send(WindowAction action) {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.allowsMultipleSelection = YES;
  if (action == WindowAction::kInstallApk) {
    panel.message = @"Choose Android apps (.apk) to install";
    if (UTType* apk = [UTType typeWithFilenameExtension:@"apk"]) panel.allowedContentTypes = @[ apk ];
  } else {
    panel.message = @"Choose files to copy to Android's Download folder";
  }
  if ([panel runModal] == NSModalResponseOK) {
    for (NSURL* u in panel.URLs) send_action(action, u.path.UTF8String);
  }
}
KestrelHintsView* g_hints_view = nil;
unsigned g_hints_generation = 0;  // cancels stale fade-outs
void fade_hints(bool visible, double seconds) {
  [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
    ctx.duration = seconds;
    g_hints_view.animator.alphaValue = visible ? 1 : 0;
  }];
}

// Shows the hints, then fades them out after a while unless they're always on.
void present_hints() {
  if (!g_hints_view.hasMarkers) {
    fade_hints(false, 0.2);
    return;
  }
  fade_hints(true, 0.2);
  if ([NSUserDefaults.standardUserDefaults boolForKey:kAlwaysHintsKey]) return;
  const unsigned gen = ++g_hints_generation;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
    if (gen == g_hints_generation && ![NSUserDefaults.standardUserDefaults boolForKey:kAlwaysHintsKey]) {
      fade_hints(false, 1.0);
    }
  });
}

void set_always_hints(bool on) {
  [NSUserDefaults.standardUserDefaults setBool:on forKey:kAlwaysHintsKey];
  NSMenuItem* it = [[NSApp.mainMenu itemWithTitle:@"Game"].submenu itemWithTitle:@"Always Show Key Hints"];
  it.state = on ? NSControlStateValueOn : NSControlStateValueOff;
  ++g_hints_generation;
  if (on) {
    present_hints();
  } else {
    fade_hints(false, 0.3);
  }
}
}  // namespace

namespace {
// Direct display: the GPU draws into a Metal view behind our (then transparent) game view.
std::function<bool(void*, int, int, int, int, float)> g_direct_attach;
bool g_direct = false;

NSRect g_direct_rect = NSZeroRect;  // last rectangle given to the GPU (points, top-left origin)

void update_direct_display(NSView* container, NSView* game, uint32_t gw, uint32_t gh) {
  if (!g_direct_attach || !game.window || gw == 0 || gh == 0) return;
  const NSRect f = game.frame;  // in the container (the window's content view)
  const CGFloat scale = std::min(f.size.width / gw, f.size.height / gh);
  const CGFloat w = std::floor(gw * scale), h = std::floor(gh * scale);
  const CGFloat x = f.origin.x + (f.size.width - w) / 2, y = f.origin.y + (f.size.height - h) / 2;
  const CGFloat top = container.bounds.size.height - (y + h);  // gfxstream wants top-left origin
  const NSRect rect = NSMakeRect(std::floor(x), std::floor(top), w, h);
  if (g_direct && NSEqualRects(rect, g_direct_rect)) return;  // layout passes without a change
  g_direct_rect = rect;
  const bool ok = g_direct_attach((__bridge void*)game.window, static_cast<int>(x), static_cast<int>(top),
                                  static_cast<int>(w), static_cast<int>(h),
                                  static_cast<float>(game.window.backingScaleFactor));
  if (ok && !g_direct) {
    g_direct = true;
    game.layer.contents = nil;
    game.layer.backgroundColor = NSColor.clearColor.CGColor;
    container.layer.backgroundColor = NSColor.blackColor.CGColor;
  }
}
}  // namespace

namespace {
std::atomic<unsigned> g_frames{0};  // frames shown since the last FPS update
FrameStats g_badge_stats;           // the FPS badge (1 s)
FrameStats g_log_stats;             // perf.csv (10 s)
FILE* g_perf_log = nullptr;         // ~/Library/Logs/Kestrel/perf.csv while Android runs
int g_perf_ticks = 0;
double g_perf_cpu = -1;             // process CPU seconds at the last log line

double process_cpu_seconds() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}
void note_frame() {
  g_frames.fetch_add(1, std::memory_order_relaxed);
  const auto now = FrameStats::Clock::now();
  g_badge_stats.record(now);
  g_log_stats.record(now);
}
KestrelFpsLabel* g_fps_label = nil;
NSTimer* g_fps_timer = nil;
NSString* const kShowFpsKey = @"ShowFPS";

// Shows or hides the FPS readout, remembers the choice and ticks the menu item.
void set_show_fps(bool show) {
  [NSUserDefaults.standardUserDefaults setBool:show forKey:kShowFpsKey];
  NSMenuItem* it = [[NSApp.mainMenu itemWithTitle:@"Window"].submenu itemWithTitle:@"Show FPS"];
  it.state = show ? NSControlStateValueOn : NSControlStateValueOff;
  g_fps_label.hidden = !show;
  g_frames = 0;
}
}  // namespace

// A slim tab on the right edge that brings the hidden toolbar back.
@interface KestrelRevealTab : NSView
@property(nonatomic) BOOL hovered;
@end
@implementation KestrelRevealTab
- (void)updateTrackingAreas {
  [super updateTrackingAreas];
  for (NSTrackingArea* t in self.trackingAreas) [self removeTrackingArea:t];
  [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
                                                     options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInKeyWindow
                                                       owner:self
                                                    userInfo:nil]];
}
- (void)mouseEntered:(NSEvent*)e {
  _hovered = YES;
  [self setNeedsDisplay:YES];
}
- (void)mouseExited:(NSEvent*)e {
  _hovered = NO;
  [self setNeedsDisplay:YES];
}
- (void)mouseDown:(NSEvent*)e {}
- (void)mouseUp:(NSEvent*)e {
  if (g_set_sidebar) g_set_sidebar(true);
}
- (void)drawRect:(NSRect)dirty {
  const NSRect r = NSInsetRect(self.bounds, 1, 0);
  [[NSColor colorWithWhite:0.12 alpha:_hovered ? 0.95 : 0.55] setFill];
  [[NSBezierPath bezierPathWithRoundedRect:r xRadius:6 yRadius:6] fill];
  NSImage* chevron = [NSImage imageWithSystemSymbolName:@"chevron.left" accessibilityDescription:@"Show toolbar"];
  NSImage* tinted = [chevron copy];
  [tinted lockFocus];
  [[NSColor colorWithWhite:1 alpha:_hovered ? 1 : 0.7] set];
  NSRectFillUsingOperation(NSMakeRect(0, 0, tinted.size.width, tinted.size.height), NSCompositingOperationSourceAtop);
  [tinted unlockFocus];
  const NSSize sz = tinted.size;
  [tinted drawInRect:NSMakeRect(NSMidX(r) - sz.width / 2, NSMidY(r) - sz.height / 2, sz.width, sz.height)];
}
- (NSString*)toolTip { return @"Show the toolbar (⌥⌘T)"; }
@end

@interface KestrelContainer : NSView
@property(nonatomic) NSView* game;
@property(nonatomic) NSView* sidebar;
@property(nonatomic) BOOL sidebarVisible;
@property(nonatomic) KestrelRevealTab* revealTab;
@end

@implementation KestrelContainer
- (void)layout {
  [super layout];
  const NSRect b = self.bounds;
  const CGFloat side = _sidebarVisible ? kSidebarWidth : 0;
  _game.frame = NSMakeRect(0, 0, b.size.width - side, b.size.height);
  if (g_fps_label) {  // top-left corner of the game
    const CGFloat y = _game.isFlipped ? 8 : _game.bounds.size.height - 30;
    g_fps_label.frame = NSMakeRect(8, y, 130, 22);
  }
  _sidebar.frame = NSMakeRect(b.size.width - side, 0, side, b.size.height);
  _sidebar.hidden = !_sidebarVisible;
  const bool fullscreen = self.window && (self.window.styleMask & NSWindowStyleMaskFullScreen);
  _revealTab.hidden = _sidebarVisible || fullscreen;
  _revealTab.frame = NSMakeRect(b.size.width - 16, (b.size.height - 90) / 2, 14, 90);
  KestrelView* game = (KestrelView*)_game;
  update_direct_display(self, game, game.guestWidth, game.guestHeight);
}
@end

// ---- window delegate ----

@interface KestrelWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) WindowInput* input;
@property(nonatomic) KestrelContainer* container;
@property(nonatomic) uint32_t guestWidth;
@property(nonatomic) uint32_t guestHeight;
@property(nonatomic) BOOL wantSidebar;
@end

@implementation KestrelWindowDelegate
- (void)windowDidMiniaturize:(NSNotification*)n {
  if (_input && _input->visibility) _input->visibility(false);
}
- (void)windowDidDeminiaturize:(NSNotification*)n {
  if (_input && _input->visibility) _input->visibility(true);
}
- (BOOL)windowShouldClose:(NSWindow*)sender {
  if (_input && _input->close_requested) _input->close_requested();
  return NO;  // the app quits once the VM has stopped
}
- (void)windowDidResignKey:(NSNotification*)n {
  // Lift every finger and free the mouse when the user switches away.
  if (_input && _input->focus_lost) _input->focus_lost();
}
// Keep the Android screen's shape when resizing (the toolbar has a fixed width).
- (NSSize)windowWillResize:(NSWindow*)w toSize:(NSSize)size {
  if (w.styleMask & NSWindowStyleMaskFullScreen) return size;
  const CGFloat side = _container.sidebarVisible ? kSidebarWidth : 0;
  NSRect content = [w contentRectForFrameRect:NSMakeRect(0, 0, size.width, size.height)];
  const CGFloat game_w = std::max<CGFloat>(content.size.width - side, 320);
  content.size.height = game_w * _guestHeight / _guestWidth;
  content.size.width = game_w + side;
  return [w frameRectForContentRect:content].size;
}
- (void)windowWillEnterFullScreen:(NSNotification*)n {
  _container.sidebarVisible = NO;  // full screen is just the game
  _container.needsLayout = YES;
}
- (void)windowDidExitFullScreen:(NSNotification*)n {
  _container.sidebarVisible = _wantSidebar;
  _container.needsLayout = YES;
}
@end

// ---- menu commands and app delegate ----

@interface KestrelMenuTarget : NSObject
@end

@implementation KestrelMenuTarget
- (void)home:(id)sender { send_action(WindowAction::kHome); }
- (void)back:(id)sender { send_action(WindowAction::kBack); }
- (void)recents:(id)sender { send_action(WindowAction::kRecents); }
- (void)closeApp:(id)sender { send_action(WindowAction::kCloseApp); }
- (void)volumeUp:(id)sender { send_action(WindowAction::kVolumeUp); }
- (void)volumeDown:(id)sender { send_action(WindowAction::kVolumeDown); }
- (void)appStore:(id)sender { send_action(WindowAction::kAppStore); }
- (void)settings:(id)sender { send_action(WindowAction::kSettings); }
- (void)paste:(id)sender {
  NSString* text = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
  if (text.length) send_action(WindowAction::kPaste, text.UTF8String);
}
- (void)installApk:(id)sender {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.allowsMultipleSelection = YES;
  panel.message = @"Choose Android apps (.apk) to install";
  if (UTType* apk = [UTType typeWithFilenameExtension:@"apk"]) panel.allowedContentTypes = @[ apk ];
  if ([panel runModal] == NSModalResponseOK) {
    for (NSURL* u in panel.URLs) send_action(WindowAction::kInstallApk, u.path.UTF8String);
  }
}
- (void)shareFiles:(id)sender {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.allowsMultipleSelection = YES;
  panel.message = @"Choose files to copy to Android's Download folder";
  if ([panel runModal] == NSModalResponseOK) {
    for (NSURL* u in panel.URLs) send_action(WindowAction::kShareFile, u.path.UTF8String);
  }
}
- (void)screenshot:(id)sender { send_action(WindowAction::kScreenshot); }
- (void)toggleRecording:(id)sender { send_action(WindowAction::kToggleRecording); }
- (void)newInstance:(id)sender {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"New Android instance";
  alert.informativeText = [NSString stringWithFormat:
      @"A second Android with its own apps and accounts, in its own window. It shares the system "
      @"files with the main one, so it takes almost no disk space.\n\nEach running Android uses up to "
      @"%.0f GB of memory: on this Mac, run at most %d at a time.",
      AppSettings::load(app_support_dir() + "/settings.json").ram_mb / 1024.0,
      std::max(1, static_cast<int>(host_memory_mb() / 1024 / 4))];
  NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 24)];
  field.placeholderString = @"Name, e.g. alt-account";
  alert.accessoryView = field;
  [alert addButtonWithTitle:@"Create and Open"];
  [alert addButtonWithTitle:@"Cancel"];
  alert.window.initialFirstResponder = field;
  if ([alert runModal] != NSAlertFirstButtonReturn) return;
  send_action(WindowAction::kNewInstance, field.stringValue.UTF8String);
}
- (void)openInstance:(id)sender {
  const auto names = list_instances(app_support_dir());
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"Open an Android instance";
  alert.informativeText = names.empty() ? @"There's only the main one so far: use File › New Instance… to make another."
                                        : @"It opens in its own window.";
  NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 240, 26) pullsDown:NO];
  [popup addItemWithTitle:@"Main"];
  for (const auto& n : names) [popup addItemWithTitle:ns(n)];
  alert.accessoryView = popup;
  [alert addButtonWithTitle:@"Open"];
  [alert addButtonWithTitle:@"Cancel"];
  if ([alert runModal] != NSAlertFirstButtonReturn) return;
  const NSInteger i = popup.indexOfSelectedItem;
  send_action(WindowAction::kOpenInstance, i <= 0 ? "" : names[static_cast<size_t>(i - 1)]);
}
- (void)factoryReset:(id)sender {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = @"Erase all Android data?";
  alert.informativeText = @"Installed apps, game progress stored on the device and settings will be "
                          @"deleted, and Android restarts like new. This can't be undone.";
  alert.alertStyle = NSAlertStyleCritical;
  [alert addButtonWithTitle:@"Erase and Restart"];
  [alert addButtonWithTitle:@"Cancel"];
  if ([alert runModal] == NSAlertFirstButtonReturn) send_action(WindowAction::kFactoryReset);
}
- (void)toggleGameControls:(id)sender { send_action(WindowAction::kToggleGameControls); }
- (void)library:(id)sender { send_action(WindowAction::kShowLibrary); }
- (void)reportProblem:(id)sender { send_action(WindowAction::kReportProblem); }
- (void)checkUpdates:(id)sender { check_for_updates(true); }
- (void)toggleSidebar:(id)sender {
  if (g_set_sidebar) g_set_sidebar(!g_sidebar_shown);
}
- (void)toggleHints:(id)sender {
  set_always_hints(![NSUserDefaults.standardUserDefaults boolForKey:kAlwaysHintsKey]);
}
- (void)toggleFps:(NSMenuItem*)sender {
  set_show_fps(![NSUserDefaults.standardUserDefaults boolForKey:kShowFpsKey]);
}
- (void)editGameControls:(id)sender { send_action(WindowAction::kEditGameControls); }
// Controls files are plain JSON (.json) so they can be shared anywhere.
- (void)exportControls:(id)sender {
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.allowedContentTypes = @[ UTTypeJSON ];
  panel.nameFieldStringValue = @"Free Fire controls.json";
  panel.message = @"Save the game's current layout to share it or keep a copy.";
  if ([panel runModal] == NSModalResponseOK) send_action(WindowAction::kExportControls, panel.URL.path.UTF8String);
}
- (void)importControls:(id)sender {
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.allowedContentTypes = @[ UTTypeJSON ];
  panel.message = @"Choose a Kestrel controls file. It's added as a new layout.";
  if ([panel runModal] == NSModalResponseOK) send_action(WindowAction::kImportControls, panel.URL.path.UTF8String);
}
- (void)quit:(id)sender {
  if (g_vm_running && g_input && g_input->close_requested) {
    g_input->close_requested();  // shut Android down cleanly; the app exits after
  } else {
    [NSApp terminate:nil];
  }
}
@end

@interface KestrelAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation KestrelAppDelegate
- (void)applicationDidHide:(NSNotification*)n {
  if (g_input && g_input->visibility) g_input->visibility(false);
}
- (void)applicationDidUnhide:(NSNotification*)n {
  if (g_input && g_input->visibility) g_input->visibility(true);
}
- (void)application:(NSApplication*)app openURLs:(NSArray<NSURL*>*)urls {
  for (NSURL* u in urls) {
    if (u.isFileURL && g_open_files) g_open_files(u.path.UTF8String);
  }
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  if (g_vm_running && g_input && g_input->close_requested) {
    g_input->close_requested();
    return NSTerminateCancel;  // quit_app_loop() ends the app once the VM stopped
  }
  return NSTerminateNow;
}
@end

namespace {

KestrelMenuTarget* g_menu_target = nil;
KestrelAppDelegate* g_app_delegate = nil;

NSMenuItem* item(NSMenu* menu, NSString* title, SEL action, NSString* key,
                 NSEventModifierFlags mods = NSEventModifierFlagCommand) {
  NSMenuItem* it = [menu addItemWithTitle:title action:action keyEquivalent:key];
  it.keyEquivalentModifierMask = mods;
  it.target = g_menu_target;
  return it;
}

void ensure_app() {
  [NSApplication sharedApplication];
  if (g_app_delegate) return;
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  // The Dock caches app icons; setting it here shows the current one right away.
  if (NSImage* icon = [NSImage imageNamed:@"Kestrel"]) NSApp.applicationIconImage = icon;
  g_app_delegate = [[KestrelAppDelegate alloc] init];
  NSApp.delegate = g_app_delegate;
  g_menu_target = [[KestrelMenuTarget alloc] init];

  NSMenu* bar = [[NSMenu alloc] init];
  auto submenu = [&](NSString* title) {
    NSMenuItem* top = [[NSMenuItem alloc] init];
    [bar addItem:top];
    NSMenu* m = [[NSMenu alloc] initWithTitle:title];
    top.submenu = m;
    return m;
  };
  NSMenu* app = submenu(@"Kestrel");
  [app addItemWithTitle:@"About Kestrel" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
  [app addItem:NSMenuItem.separatorItem];
  item(app, @"Settings…", @selector(settings:), @",");
  [app addItem:NSMenuItem.separatorItem];
  [app addItemWithTitle:@"Hide Kestrel" action:@selector(hide:) keyEquivalent:@"h"];
  [app addItem:NSMenuItem.separatorItem];
  item(app, @"Quit Kestrel", @selector(quit:), @"q");

  NSMenu* file = submenu(@"File");
  item(file, @"Install APK…", @selector(installApk:), @"o");
  item(file, @"Send Files to Android…", @selector(shareFiles:), @"o",
       NSEventModifierFlagCommand | NSEventModifierFlagShift);
  item(file, @"Save Screenshot to Desktop", @selector(screenshot:), @"s");
  item(file, @"Start/Stop Recording", @selector(toggleRecording:), @"r", NSEventModifierFlagCommand | NSEventModifierFlagShift);
  [file addItem:NSMenuItem.separatorItem];
  item(file, @"New Instance…", @selector(newInstance:), @"n", NSEventModifierFlagCommand | NSEventModifierFlagShift);
  item(file, @"Open Instance…", @selector(openInstance:), @"");

  NSMenu* edit = submenu(@"Edit");
  item(edit, @"Paste to Android", @selector(paste:), @"v");

  NSMenu* android = submenu(@"Android");
  item(android, @"Library", @selector(library:), @"l");
  [android addItem:NSMenuItem.separatorItem];
  item(android, @"Back", @selector(back:), @"[");
  item(android, @"Home", @selector(home:), @"h", NSEventModifierFlagCommand | NSEventModifierFlagShift);
  item(android, @"Recent Apps", @selector(recents:), @"r", NSEventModifierFlagCommand | NSEventModifierFlagShift);
  item(android, @"Close Current App", @selector(closeApp:), @"w");
  [android addItem:NSMenuItem.separatorItem];
  item(android, @"Volume Up", @selector(volumeUp:), @"=");
  item(android, @"Volume Down", @selector(volumeDown:), @"-");
  [android addItem:NSMenuItem.separatorItem];
  item(android, @"App Store", @selector(appStore:), @"a", NSEventModifierFlagCommand | NSEventModifierFlagShift);
  [android addItem:NSMenuItem.separatorItem];
  item(android, @"Erase All Data and Restart…", @selector(factoryReset:), @"");

  NSMenu* game = submenu(@"Game");
  item(game, @"Game Controls On/Off", @selector(toggleGameControls:), @"k");
  item(game, @"Edit Game Controls…", @selector(editGameControls:), @"k",
       NSEventModifierFlagCommand | NSEventModifierFlagShift);
  [game addItem:NSMenuItem.separatorItem];
  item(game, @"Import Controls…", @selector(importControls:), @"");
  item(game, @"Export Controls…", @selector(exportControls:), @"");
  [game addItem:NSMenuItem.separatorItem];
  item(game, @"Always Show Key Hints", @selector(toggleHints:), @"/").state =
      [NSUserDefaults.standardUserDefaults boolForKey:kAlwaysHintsKey] ? NSControlStateValueOn
                                                                         : NSControlStateValueOff;

  NSMenu* window = submenu(@"Window");
  [window addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
  [window addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"]
      .keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
  item(window, @"Show/Hide Toolbar", @selector(toggleSidebar:), @"t",
       NSEventModifierFlagCommand | NSEventModifierFlagOption);
  item(window, @"Show FPS", @selector(toggleFps:), @"f", NSEventModifierFlagCommand | NSEventModifierFlagShift)
      .state = [NSUserDefaults.standardUserDefaults boolForKey:kShowFpsKey] ? NSControlStateValueOn
                                                                            : NSControlStateValueOff;
  NSApp.windowsMenu = window;
  NSMenu* help = submenu(@"Help");
  item(help, @"Check for Updates…", @selector(checkUpdates:), @"");
  item(help, @"Report a Problem…", @selector(reportProblem:), @"");
  NSApp.helpMenu = help;
  NSApp.mainMenu = bar;
}

// ---- settings sheet ----

NSPopUpButton* popup(NSView* parent, CGFloat y, NSString* label, NSArray<NSString*>* items, NSInteger selected) {
  NSTextField* l = [NSTextField labelWithString:label];
  l.frame = NSMakeRect(20, y, 150, 22);
  l.alignment = NSTextAlignmentRight;
  [parent addSubview:l];
  NSPopUpButton* p = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(180, y - 2, 260, 26) pullsDown:NO];
  [p addItemsWithTitles:items];
  [p selectItemAtIndex:std::clamp<NSInteger>(selected, 0, (NSInteger)items.count - 1)];
  [parent addSubview:p];
  return p;
}

class MacWindowImpl final : public MacWindow {
 public:
  MacWindowImpl(const std::string& title, uint32_t width, uint32_t height, WindowInput input, bool show_sidebar)
      : input_(std::move(input)) {
    const CGFloat side = show_sidebar ? kSidebarWidth : 0;
    NSRect visible = NSScreen.mainScreen.visibleFrame;
    const CGFloat fit = std::min({1.0, (visible.size.width * 0.92 - side) / width, visible.size.height * 0.9 / height});
    NSRect frame = NSMakeRect(0, 0, width * fit + side, height * fit);

    window_ = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window_.title = ns(title);
    window_.backgroundColor = NSColor.blackColor;
    window_.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    window_.releasedWhenClosed = NO;
    window_.acceptsMouseMovedEvents = YES;
    window_.tabbingMode = NSWindowTabbingModeDisallowed;

    view_ = [[KestrelView alloc] initWithFrame:NSMakeRect(0, 0, width * fit, height * fit)];
    view_.wantsLayer = YES;
    view_.layer.backgroundColor = NSColor.blackColor.CGColor;
    view_.layer.contentsGravity = kCAGravityResizeAspect;
    view_.layer.magnificationFilter = kCAFilterLinear;
    view_.input = &input_;
    view_.guestWidth = width;
    view_.guestHeight = height;
    view_.allowedTouchTypes = NSTouchTypeMaskIndirect;
    [view_ registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];

    container_ = [[KestrelContainer alloc] initWithFrame:frame];
    container_.game = view_;
    container_.sidebar = [[KestrelSidebar alloc] initWithFrame:NSMakeRect(0, 0, kSidebarWidth, frame.size.height)];
    container_.sidebarVisible = show_sidebar;
    [container_ addSubview:view_];
    [container_ addSubview:container_.sidebar];
    container_.revealTab = [[KestrelRevealTab alloc] initWithFrame:NSZeroRect];
    [container_ addSubview:container_.revealTab];
    g_set_sidebar = [this](bool show) { set_sidebar_visible(show); };

    delegate_ = [[KestrelWindowDelegate alloc] init];
    delegate_.input = &input_;
    delegate_.container = container_;
    delegate_.guestWidth = width;
    delegate_.guestHeight = height;
    delegate_.wantSidebar = show_sidebar;
    g_sidebar_shown = show_sidebar;
    window_.delegate = delegate_;
    window_.contentView = container_;
    window_.contentMinSize = NSMakeSize(480 + side, 270);
    [window_ center];
    [window_ makeKeyAndOrderFront:nil];
    [window_ makeFirstResponder:view_];

    g_splash = [[KestrelSplashView alloc] initWithFrame:view_.bounds];
    g_splash.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [view_ addSubview:g_splash];

    g_library = [[KestrelLibraryView alloc] initWithFrame:view_.bounds];
    g_library.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    g_library.hidden = YES;
    g_library.onLaunch = ^(NSString* component) { send_action(WindowAction::kLaunchApp, component.UTF8String); };
    g_library.onAction = ^(NSInteger action) {
      const auto a = static_cast<WindowAction>(action);
      if (a == WindowAction::kInstallApk) choose_files_and_send(a);
      else send_action(a);
    };

    g_hints_view = [[KestrelHintsView alloc] initWithFrame:view_.bounds];
    g_hints_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    g_hints_view.guestWidth = width;
    g_hints_view.guestHeight = height;
    g_hints_view.wantsLayer = YES;
    g_hints_view.alphaValue = 0;
    [view_ addSubview:g_hints_view];
    [view_ addSubview:g_library];

    g_fps_label = [[KestrelFpsLabel alloc] initWithFrame:NSMakeRect(8, view_.bounds.size.height - 30, 74, 22)];
    g_fps_label.editable = NO;
    g_fps_label.selectable = NO;
    g_fps_label.bezeled = NO;
    g_fps_label.drawsBackground = YES;
    g_fps_label.backgroundColor = [NSColor colorWithWhite:0 alpha:0.55];
    g_fps_label.textColor = NSColor.whiteColor;
    g_fps_label.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightSemibold];
    g_fps_label.alignment = NSTextAlignmentCenter;
    g_fps_label.stringValue = @"– FPS";
    g_fps_label.frame = NSMakeRect(8, view_.bounds.size.height - 30, 120, 22);
    g_fps_label.wantsLayer = YES;
    g_fps_label.layer.cornerRadius = 5;
    g_fps_label.hidden = ![NSUserDefaults.standardUserDefaults boolForKey:kShowFpsKey];
    [view_ addSubview:g_fps_label];
    container_.needsLayout = YES;
    g_fps_timer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                  repeats:YES
                                                    block:^(NSTimer*) {
                                                      g_frames.exchange(0);
                                                      const auto w = g_badge_stats.take();
                                                      if (!g_fps_label.hidden) {
                                                        // "1% low": how fast the slowest frames were.
                                                        g_fps_label.stringValue =
                                                            w.frames > 10 ? [NSString stringWithFormat:@"%.0f FPS · low %.0f", w.fps, std::min(w.fps, w.low1_fps)]
                                                                          : [NSString stringWithFormat:@"%.0f FPS", w.fps];
                                                      }
                                                      // Every 10 s: a line in perf.csv for tuning.
                                                      if (++g_perf_ticks % 10 == 0 && g_perf_log) {
                                                        const auto l = g_log_stats.take();
                                                        const double cpu = process_cpu_seconds();
                                                        if (l.frames > 0 && g_perf_cpu >= 0 && l.seconds > 0) {
                                                          const std::time_t t = std::time(nullptr);
                                                          char stamp[32];
                                                          std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&t));
                                                          std::fprintf(g_perf_log, "%s,%.1f,%.1f,%.1f,%u,%.0f\n", stamp, l.fps,
                                                                       std::min(l.fps, l.low1_fps), l.worst_ms, l.hitches,
                                                                       100 * (cpu - g_perf_cpu) / l.seconds);
                                                          std::fflush(g_perf_log);
                                                        }
                                                        g_perf_cpu = cpu;
                                                      }
                                                    }];
    {
      const std::string path = app_logs_dir() + "/perf.csv";
      std::error_code ec;
      std::filesystem::rename(path, app_logs_dir() + "/perf.prev.csv", ec);
      g_perf_log = std::fopen(path.c_str(), "w");
      if (g_perf_log) std::fprintf(g_perf_log, "time,fps,low1_fps,worst_frame_ms,hitches,kestrel_cpu_pct\n");
    }
    g_input = &input_;
    g_raw_input = &input_;
    static bool raw_started = false;
    if (!raw_started) {
      raw_started = true;
      raw_mouse::start([](double dx, double dy) {
        if (WindowInput* in = g_raw_input.load(); in && in->mouse_delta) in->mouse_delta(dx, dy);
      });
    }
    g_vm_running = true;
  }

  ~MacWindowImpl() override {
    if (view_.captured) {
      CGAssociateMouseAndMouseCursorPosition(true);
      [NSCursor unhide];
    }
    raw_mouse::set_captured(false);
    g_raw_input = nullptr;
    g_input = nullptr;
    g_vm_running = false;
    [g_fps_timer invalidate];
    g_fps_timer = nil;
    g_fps_label = nil;
    if (g_perf_log) std::fclose(g_perf_log);
    g_perf_log = nullptr;
    g_hints_view = nil;
    g_splash = nil;
    g_set_sidebar = nullptr;
    g_library = nil;
    if (last_image_) CGImageRelease(last_image_);
  }

  void present(const Frame& f) override {
    note_frame();
    {
      std::lock_guard lock(mutex_);
      if (!surface_ || width_ != f.width || height_ != f.height) {
        // IOSurface: Core Animation composites it on the GPU directly; no CGImage
        // conversion per frame.
        NSDictionary* props = @{
          (id)kIOSurfaceWidth : @(f.width),
          (id)kIOSurfaceHeight : @(f.height),
          (id)kIOSurfaceBytesPerElement : @4,
          (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
        };
        if (surface_) CFRelease(surface_);
        surface_ = IOSurfaceCreate((__bridge CFDictionaryRef)props);
        width_ = f.width;
        height_ = f.height;
      }
      if (!surface_) return;
      IOSurfaceLock(surface_, 0, nullptr);
      auto* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface_));
      const size_t stride = IOSurfaceGetBytesPerRow(surface_);
      for (uint32_t y = 0; y < f.height; ++y) {
        uint8_t* dst = base + y * stride;
        const uint8_t* src = f.pixels.data() + size_t(y) * f.stride;
        if (f.bgra) {
          std::memcpy(dst, src, size_t(f.width) * 4);
        } else {
          for (uint32_t x = 0; x < f.width; ++x) {  // RGBA -> BGRA
            dst[4 * x] = src[4 * x + 2];
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = src[4 * x];
            dst[4 * x + 3] = 255;
          }
        }
      }
      IOSurfaceUnlock(surface_, 0, nullptr);
      if (g_direct) return;         // kept only for screenshots; the GPU draws the window
      if (update_pending_) return;  // the main thread will pick up the newest pixels
      update_pending_ = true;
    }
    dispatch_async(dispatch_get_main_queue(), ^{ this->show_latest(); });
  }

  void set_status(const std::string& status) override {
    NSString* s = ns(status);
    dispatch_async(dispatch_get_main_queue(), ^{ window_.subtitle = s; });
  }

  bool save_screenshot(const std::string& path) override {
    __block bool ok = false;
    auto work = ^{
      CGImageRef image = nullptr;
      {
        std::lock_guard lock(mutex_);
        if (!surface_) return;
        IOSurfaceLock(surface_, kIOSurfaceLockReadOnly, nullptr);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(
            IOSurfaceGetBaseAddress(surface_), width_, height_, 8, IOSurfaceGetBytesPerRow(surface_), space,
            CGBitmapInfo(kCGBitmapByteOrder32Little) | CGBitmapInfo(kCGImageAlphaNoneSkipFirst));
        image = CGBitmapContextCreateImage(ctx);
        CGContextRelease(ctx);
        CGColorSpaceRelease(space);
        IOSurfaceUnlock(surface_, kIOSurfaceLockReadOnly, nullptr);
      }
      if (!image) return;
      NSURL* url = [NSURL fileURLWithPath:ns(path)];
      CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
          (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
      if (dest) {
        CGImageDestinationAddImage(dest, image, nullptr);
        ok = CGImageDestinationFinalize(dest);
        CFRelease(dest);
      }
      CGImageRelease(image);
    };
    if (NSThread.isMainThread) work();
    else dispatch_sync(dispatch_get_main_queue(), work);
    return ok;
  }

  void set_mouse_captured(bool captured) override {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (captured == view_.captured) return;
      view_.captured = captured;
      raw_mouse::set_captured(captured);
      // Every mouse report, not one per screen frame, while aiming.
      NSEvent.mouseCoalescingEnabled = !captured;
      if (captured) {
        // Park the cursor in the window and stop it moving; raw deltas still arrive.
        NSRect r = [window_ convertRectToScreen:[view_ convertRect:view_.bounds toView:nil]];
        const CGFloat screen_h = NSScreen.screens.firstObject.frame.size.height;
        CGWarpMouseCursorPosition(CGPointMake(NSMidX(r), screen_h - NSMidY(r)));
        CGAssociateMouseAndMouseCursorPosition(false);
        [NSCursor hide];
      } else {
        CGAssociateMouseAndMouseCursorPosition(true);
        [NSCursor unhide];
      }
    });
  }

  void set_boot_progress(double fraction, const std::string& text) override {
    NSString* t = ns(text);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!g_splash || g_splash.hidden) return;
      g_splash.status.stringValue = t;
      [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
        ctx.duration = 0.25;
        g_splash.bar.animator.doubleValue = std::clamp(fraction, 0.0, 1.0);
      }];
      if (fraction >= 1) {
        [NSAnimationContext
            runAnimationGroup:^(NSAnimationContext* ctx) {
              ctx.duration = 0.4;
              g_splash.animator.alphaValue = 0;
            }
            completionHandler:^{
              g_splash.hidden = YES;
            }];
      }
    });
  }

  void show_library(std::vector<LibraryItem> items, bool loading) override {
    auto shared = std::make_shared<std::vector<LibraryItem>>(std::move(items));
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!g_library) return;
      [g_library setItems:*shared loading:loading];
      if (g_library.hidden) {
        g_library.hidden = NO;
        g_library.alphaValue = 0;
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
          ctx.duration = 0.2;
          g_library.animator.alphaValue = 1;
        }];
      }
      [window_ makeFirstResponder:g_library];
    });
  }

  void hide_library() override {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!g_library || g_library.hidden) return;
      g_library.hidden = YES;
      [window_ makeFirstResponder:view_];
    });
  }

  void android_ready() override {
    dispatch_async(dispatch_get_main_queue(), ^{
      NSUserDefaults* d = NSUserDefaults.standardUserDefaults;
      // Quietly, at most once a day (says nothing unless there's a new version).
      NSDate* last = [d objectForKey:@"LastUpdateCheck"];
      if (!last || -last.timeIntervalSinceNow > 24 * 3600) check_for_updates(false);
      if ([d boolForKey:@"WelcomeShown"]) return;
      [d setBool:YES forKey:@"WelcomeShown"];
      NSAlert* alert = [[NSAlert alloc] init];
      alert.messageText = @"Welcome to Kestrel";
      alert.informativeText =
          @"• Get games: click App Store (bag icon), or drop an .apk file on the window.\n"
          @"• Your games are in the Library (⌘L).\n"
          @"• In Free Fire: WASD moves, Option (⌥) turns aiming on and off, left click fires. "
          @"Keys appear on screen when the game starts (⌘/ keeps them visible).\n"
          @"• Move keys onto the right buttons with ⇧⌘K.\n"
          @"• Settings (⌘,): Performance mode, frame rate, resolution.";
      [alert addButtonWithTitle:@"Let's play"];
      [alert beginSheetModalForWindow:window_ completionHandler:nil];
    });
  }

  // Shows or hides the toolbar, growing or shrinking the window so the game keeps
  // its size, and tells the app to remember the choice.
  void set_sidebar_visible(bool show) {
    if (delegate_.wantSidebar == show && container_.sidebarVisible == show) return;
    delegate_.wantSidebar = show;
    g_sidebar_shown = show;
    const bool fullscreen = window_.styleMask & NSWindowStyleMaskFullScreen;
    if (!fullscreen) {
      NSRect f = window_.frame;
      const CGFloat delta = show ? kSidebarWidth : -kSidebarWidth;
      f.size.width += delta;
      container_.sidebarVisible = show;
      [window_ setFrame:f display:YES animate:NO];  // one resize: the GPU surface follows once
    }
    container_.needsLayout = YES;
    send_action(WindowAction::kSidebarChanged, show ? "1" : "0");
  }

  void frame_shown() override { note_frame(); }

  void set_direct_display(std::function<bool(void*, int, int, int, int, float)> attach) override {
    auto shared = std::make_shared<std::function<bool(void*, int, int, int, int, float)>>(std::move(attach));
    dispatch_async(dispatch_get_main_queue(), ^{
      g_direct_attach = *shared;
      container_.wantsLayer = YES;
      update_direct_display(container_, view_, view_.guestWidth, view_.guestHeight);
    });
  }

  void show_key_hints(std::vector<ControlMarker> markers) override {
    auto shared = std::make_shared<std::vector<ControlMarker>>(std::move(markers));
    dispatch_async(dispatch_get_main_queue(), ^{
      [g_hints_view setMarkers:*shared];
      present_hints();
    });
  }

  void edit_controls(const std::string& title, std::vector<ControlMarker> markers, std::vector<std::string> layouts,
                     size_t active_layout, std::function<void(ControlsEditorResult)> done) override {
    NSString* t = ns(title);
    auto shared_done = std::make_shared<std::function<void(ControlsEditorResult)>>(std::move(done));
    auto shared_markers = std::make_shared<std::vector<ControlMarker>>(std::move(markers));
    NSMutableArray* names = [NSMutableArray array];
    for (const auto& l : layouts) [names addObject:ns(l)];
    if (names.count == 0) [names addObject:@"Default"];
    dispatch_async(dispatch_get_main_queue(), ^{
      if (editor_) [editor_ removeFromSuperview];  // reopened with another layout
      set_mouse_captured(false);
      ++g_hints_generation;  // the editor shows the same controls: no hints under it
      fade_hints(false, 0.1);
      editor_ = [[KestrelEditorView alloc] initWithFrame:view_.bounds];
      editor_.guestWidth = view_.guestWidth;
      editor_.guestHeight = view_.guestHeight;
      [editor_ setMarkers:*shared_markers title:t layouts:names active:static_cast<NSInteger>(active_layout)];
      KestrelEditorView* editor = editor_;
      editor_.onFinish = ^(NSInteger action, NSInteger layout, NSString* name) {
        ControlsEditorResult r;
        r.action = static_cast<ControlsEditorResult::Action>(action);
        r.markers = [editor markers];
        r.layout = static_cast<size_t>(std::max<NSInteger>(0, layout));
        if (r.action == ControlsEditorResult::Action::kSetSwitchKey) r.key = static_cast<uint16_t>(layout);
        if (name) r.name = name.UTF8String;
        [editor removeFromSuperview];
        if (this->editor_ == editor) this->editor_ = nil;
        if ([NSUserDefaults.standardUserDefaults boolForKey:kAlwaysHintsKey]) present_hints();
        [this->window_ makeFirstResponder:this->view_];
        (*shared_done)(std::move(r));
      };
      [view_ addSubview:editor_];
      [window_ makeFirstResponder:editor_];
    });
  }

  void show_settings(AppSettings current, std::function<void(const AppSettings&, bool)> done) override {
    auto shared_done = std::make_shared<std::function<void(const AppSettings&, bool)>>(std::move(done));
    dispatch_async(dispatch_get_main_queue(), ^{
      const CGFloat W = 460, H = 664;
      NSWindow* sheet = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                                    styleMask:NSWindowStyleMaskTitled
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
      NSView* v = sheet.contentView;
      NSMutableArray* targets = [NSMutableArray array];  // keeps the control actions alive
      CGFloat y = H - 40;
      auto heading = [&](NSString* text, CGFloat size) {
        NSTextField* t = [NSTextField labelWithString:text];
        t.font = [NSFont boldSystemFontOfSize:size];
        t.frame = NSMakeRect(20, y, W - 40, size + 6);
        [v addSubview:t];
        y -= size + 14;
      };
      auto row = [&](NSString* label, NSArray<NSString*>* items, NSInteger sel) {
        NSPopUpButton* p = popup(v, y, label, items, sel);
        y -= 32;
        return p;
      };
      auto check = [&](NSString* title, bool on) {
        NSButton* b = [NSButton checkboxWithTitle:title target:nil action:nil];
        b.state = on ? NSControlStateValueOn : NSControlStateValueOff;
        b.frame = NSMakeRect(178, y, W - 190, 22);
        [v addSubview:b];
        y -= 24;
        return b;
      };

      heading(@"Kestrel Settings", 15);

      // --- Performance ---
      heading(@"Performance", 12);
      const uint32_t screen_hz = static_cast<uint32_t>(std::max<NSInteger>(60, NSScreen.mainScreen.maximumFramesPerSecond));
      const std::vector<std::string> modes = {"battery", "balanced", "max", "custom"};
      NSInteger mode_sel = std::find(modes.begin(), modes.end(), current.performance) - modes.begin();
      NSPopUpButton* mode_p = row(@"Mode:", @[ @"Battery saver — cool and quiet", @"Balanced", @"Max performance", @"Custom" ],
                                  mode_sel >= 4 ? 1 : mode_sel);
      std::vector<uint32_t> rates;
      NSMutableArray* rate_items = [NSMutableArray array];
      NSInteger rate_sel = 0;
      for (uint32_t hz : {30u, 60u, 90u, 120u}) {
        if (hz > screen_hz) break;
        if (hz <= current.refresh_hz) rate_sel = static_cast<NSInteger>(rates.size());
        rates.push_back(hz);
        [rate_items addObject:[NSString stringWithFormat:@"%u FPS", hz]];
      }
      NSPopUpButton* rate_p = row(@"Frame rate limit:", rate_items, rate_sel);

      // --- Display ---
      heading(@"Display", 12);
      auto [sw, sh] = main_screen_pixels();
      auto choices = AppSettings::resolution_choices(sw, sh);
      NSMutableArray* res = [NSMutableArray array];
      NSInteger res_sel = 0;
      for (size_t i = 0; i < choices.size(); ++i) {
        const bool match = i == 3;
        [res addObject:[NSString stringWithFormat:@"%u × %u%@", choices[i].first, choices[i].second,
                                                  match ? @" (matches your screen)" : @""]];
        if (choices[i] == std::pair{current.width, current.height}) res_sel = static_cast<NSInteger>(i);
      }
      NSPopUpButton* res_p = row(@"Resolution:", res, res_sel);
      const uint32_t dpis[] = {0, 160, 240, 320};
      NSPopUpButton* dpi_p = row(@"Density:",
                                 @[ @"Automatic (tablet layout)", @"Small (160 dpi)", @"Medium (240 dpi)", @"Large (320 dpi)" ],
                                 current.dpi == 0 ? 0 : current.dpi <= 160 ? 1 : current.dpi <= 240 ? 2 : 3);

      // --- Hardware ---
      heading(@"Hardware", 12);
      const uint64_t host_mb = host_memory_mb();
      std::vector<uint32_t> rams;
      NSMutableArray* ram_items = [NSMutableArray array];
      NSInteger ram_sel = 0;
      for (uint32_t mb : {2048u, 3072u, 4096u, 6144u, 8192u}) {
        if (mb > host_mb * 3 / 4) break;  // keep enough for macOS
        if (mb == current.ram_mb) ram_sel = static_cast<NSInteger>(rams.size());
        rams.push_back(mb);
        [ram_items addObject:[NSString stringWithFormat:@"%u GB", mb / 1024]];
      }
      NSPopUpButton* ram_p = row(@"Memory:", ram_items, ram_sel);
      std::vector<uint32_t> cpus;
      NSMutableArray* cpu_items = [NSMutableArray array];
      NSInteger cpu_sel = 0;
      // 3 is offered too: on Macs with 2 performance cores it can pace frames a little
      // more evenly than 4 (fewer cross-CPU wake-ups inside Android).
      for (uint32_t n : {2u, 3u, 4u, 6u, 8u}) {
        if (n > std::min<uint32_t>(host_cpu_count(), 8)) break;
        if (n == current.cpus) cpu_sel = static_cast<NSInteger>(cpus.size());
        cpus.push_back(n);
        [cpu_items addObject:[NSString stringWithFormat:@"%u cores", n]];
      }
      NSPopUpButton* cpu_p = row(@"Processor cores:", cpu_items, cpu_sel);
      NSPopUpButton* gpu_p = row(@"Graphics:", @[ @"Hardware (fast, recommended)", @"Software (compatibility)" ],
                                 current.gpu ? 0 : 1);
      NSButton* low_lat = check(@"Low-latency display", current.low_latency_display);

      // --- Controls ---
      heading(@"Controls", 12);
      NSTextField* sens_l = [NSTextField labelWithString:@"Aim sensitivity:"];
      sens_l.frame = NSMakeRect(20, y, 150, 22);
      sens_l.alignment = NSTextAlignmentRight;
      [v addSubview:sens_l];
      NSSlider* sens = [NSSlider sliderWithValue:current.mouse_sensitivity minValue:0.2 maxValue:4 target:nil action:nil];
      sens.frame = NSMakeRect(180, y - 2, 250, 24);
      [v addSubview:sens];
      y -= 30;
      NSButton* raw = check(@"Raw mouse input (no acceleration)", current.raw_mouse);
      y -= 6;

      // --- Window ---
      heading(@"Window", 12);
      NSButton* sidebar = check(@"Show the toolbar beside the game", current.show_sidebar);
      NSButton* fps = check(@"Show FPS in the corner (⇧⌘F)", [NSUserDefaults.standardUserDefaults boolForKey:kShowFpsKey]);
      NSButton* pause = check(@"Pause Android when Kestrel is minimized", current.pause_when_hidden);
      NSButton* library = check(@"Show the Library when Android starts", current.show_library);

      // Choosing a mode fills in its values; changing one of them makes it Custom.
      auto apply_mode = [=]() {
        const NSInteger m = mode_p.indexOfSelectedItem;
        if (m < 0 || m >= 3) return;
        AppSettings preset = current;
        preset.apply_performance(modes[static_cast<size_t>(m)], screen_hz);
        for (size_t i = 0; i < rates.size(); ++i) if (rates[i] <= preset.refresh_hz) [rate_p selectItemAtIndex:(NSInteger)i];
        for (size_t i = 0; i < choices.size(); ++i)
          if (choices[i] == std::pair{preset.width, preset.height}) [res_p selectItemAtIndex:(NSInteger)i];
        for (size_t i = 0; i < rams.size(); ++i) if (rams[i] == preset.ram_mb) [ram_p selectItemAtIndex:(NSInteger)i];
        for (size_t i = 0; i < cpus.size(); ++i) if (cpus[i] == preset.cpus) [cpu_p selectItemAtIndex:(NSInteger)i];
      };
      KestrelAction* on_mode = [KestrelAction actionWithBlock:^(id) { apply_mode(); }];
      mode_p.target = on_mode;
      mode_p.action = @selector(fire:);
      KestrelAction* on_custom = [KestrelAction actionWithBlock:^(id) { [mode_p selectItemAtIndex:3]; }];
      for (NSPopUpButton* p in @[ rate_p, res_p, ram_p, cpu_p ]) {
        p.target = on_custom;
        p.action = @selector(fire:);
      }
      [targets addObjectsFromArray:@[ on_mode, on_custom ]];

      NSTextField* note = [NSTextField labelWithString:@"Performance, screen, memory, cores and graphics apply when Android restarts."];
      note.textColor = NSColor.secondaryLabelColor;
      note.font = [NSFont systemFontOfSize:11];
      note.frame = NSMakeRect(20, 50, W - 40, 16);
      [v addSubview:note];

      NSButton* restart = [NSButton buttonWithTitle:@"Save and Restart Android" target:nil action:nil];
      NSButton* save = [NSButton buttonWithTitle:@"Save" target:nil action:nil];
      NSButton* cancel = [NSButton buttonWithTitle:@"Cancel" target:nil action:nil];
      restart.frame = NSMakeRect(W - 220, 12, 200, 30);
      save.frame = NSMakeRect(W - 314, 12, 86, 30);
      cancel.frame = NSMakeRect(20, 12, 86, 30);
      restart.keyEquivalent = @"\r";
      cancel.keyEquivalent = @"\033";
      restart.tag = 2;
      save.tag = 1;
      cancel.tag = 0;
      for (NSButton* b in @[ restart, save, cancel ]) [v addSubview:b];
      auto finish = [=](NSInteger tag) {
        (void)targets;
        if (tag == 0) return;
        AppSettings s = current;
        const auto r = choices[static_cast<size_t>(std::max<NSInteger>(0, res_p.indexOfSelectedItem))];
        s.width = r.first;
        s.height = r.second;
        s.dpi = dpis[std::max<NSInteger>(0, dpi_p.indexOfSelectedItem)];
        if (!rams.empty()) s.ram_mb = rams[static_cast<size_t>(std::max<NSInteger>(0, ram_p.indexOfSelectedItem))];
        if (!cpus.empty()) s.cpus = cpus[static_cast<size_t>(std::max<NSInteger>(0, cpu_p.indexOfSelectedItem))];
        if (!rates.empty()) s.refresh_hz = rates[static_cast<size_t>(std::max<NSInteger>(0, rate_p.indexOfSelectedItem))];
        s.performance = modes[static_cast<size_t>(std::clamp<NSInteger>(mode_p.indexOfSelectedItem, 0, 3))];
        s.gpu = gpu_p.indexOfSelectedItem == 0;
        s.mouse_sensitivity = sens.doubleValue;
        s.raw_mouse = raw.state == NSControlStateValueOn;
        s.low_latency_display = low_lat.state == NSControlStateValueOn;
        s.show_sidebar = sidebar.state == NSControlStateValueOn;
        s.pause_when_hidden = pause.state == NSControlStateValueOn;
        s.show_library = library.state == NSControlStateValueOn;
        (*shared_done)(s, tag == 2);
      };
      for (NSButton* b in @[ restart, save, cancel ]) {
        b.target = g_menu_target;
        b.action = @selector(settingsButton:);
      }
      settings_finish_ = [=, this](NSInteger tag) {
        [window_ endSheet:sheet];
        finish(tag);
        // Apply the toolbar choice right away.
        container_.sidebarVisible = !(window_.styleMask & NSWindowStyleMaskFullScreen) &&
                                    sidebar.state == NSControlStateValueOn;
        delegate_.wantSidebar = sidebar.state == NSControlStateValueOn;
        g_sidebar_shown = delegate_.wantSidebar;
        container_.needsLayout = YES;
        if (tag != 0) set_show_fps(fps.state == NSControlStateValueOn);
      };
      g_settings_window = this;
      [window_ beginSheet:sheet completionHandler:nil];
    });
  }

  void settings_button(NSInteger tag) {
    if (settings_finish_) {
      auto f = std::move(settings_finish_);
      settings_finish_ = nullptr;
      f(tag);
    }
  }

  void show_message(const std::string& title, const std::string& text) override {
    NSString* t = ns(title);
    NSString* x = ns(text);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (window_.attachedSheet) {  // one sheet at a time: fall back to a notification-style alert
        window_.subtitle = t;
        return;
      }
      NSAlert* alert = [[NSAlert alloc] init];
      alert.messageText = t;
      alert.informativeText = x;
      [alert beginSheetModalForWindow:window_ completionHandler:nil];
    });
  }

  void ask(const std::string& title, const std::string& text, const std::string& action,
           std::function<void()> on_action) override {
    NSString* t = ns(title);
    NSString* x = ns(text);
    NSString* a = ns(action);
    auto shared = std::make_shared<std::function<void()>>(std::move(on_action));
    dispatch_async(dispatch_get_main_queue(), ^{
      if (window_.attachedSheet) return;  // something else is being asked already
      set_mouse_captured(false);          // the player needs the cursor to answer
      NSAlert* alert = [[NSAlert alloc] init];
      alert.messageText = t;
      alert.informativeText = x;
      [alert addButtonWithTitle:a];
      [alert addButtonWithTitle:@"Wait"];
      ask_alert_ = alert;
      [alert beginSheetModalForWindow:window_
                    completionHandler:^(NSModalResponse r) {
                      if (this->ask_alert_ == alert) this->ask_alert_ = nil;
                      if (r == NSAlertFirstButtonReturn && *shared) (*shared)();
                    }];
    });
  }

  void set_recording(bool on) override {
    dispatch_async(dispatch_get_main_queue(), ^{
      [rec_timer_ invalidate];
      rec_timer_ = nil;
      if (!on) {
        [rec_label_ removeFromSuperview];
        rec_label_ = nil;
        return;
      }
      if (!rec_label_) {
        rec_label_ = [[KestrelFpsLabel alloc] initWithFrame:NSZeroRect];
        rec_label_.editable = NO;
        rec_label_.selectable = NO;
        rec_label_.bezeled = NO;
        rec_label_.drawsBackground = YES;
        rec_label_.backgroundColor = [NSColor colorWithRed:0.85 green:0.1 blue:0.1 alpha:0.85];
        rec_label_.textColor = NSColor.whiteColor;
        rec_label_.font = [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightBold];
        rec_label_.alignment = NSTextAlignmentCenter;
        rec_label_.wantsLayer = YES;
        rec_label_.layer.cornerRadius = 5;
        rec_label_.autoresizingMask = NSViewMinXMargin;
        [view_ addSubview:rec_label_];
      }
      rec_label_.frame = NSMakeRect(view_.bounds.size.width - 110, 8, 100, 22);  // flipped: top right
      const double start = CACurrentMediaTime();
      KestrelFpsLabel* label = rec_label_;
      auto tick = ^(NSTimer*) {
        const int s = static_cast<int>(CACurrentMediaTime() - start);
        label.stringValue = [NSString stringWithFormat:@"● REC %d:%02d", s / 60, s % 60];
      };
      tick(nil);
      rec_timer_ = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:tick];
    });
  }

  void dismiss_ask() override {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!ask_alert_) return;
      NSWindow* sheet = ask_alert_.window;
      ask_alert_ = nil;
      [window_ endSheet:sheet returnCode:NSAlertSecondButtonReturn];  // as if "Wait"
    });
  }

  void show_toast(const std::string& text, double seconds) override {
    NSString* t = ns(text);
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!toast_) {
        toast_ = [[KestrelFpsLabel alloc] initWithFrame:NSZeroRect];  // ignores clicks
        toast_.editable = NO;
        toast_.selectable = NO;
        toast_.bezeled = NO;
        toast_.drawsBackground = YES;
        toast_.backgroundColor = [NSColor colorWithWhite:0 alpha:0.7];
        toast_.textColor = NSColor.whiteColor;
        toast_.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
        toast_.alignment = NSTextAlignmentCenter;
        toast_.wantsLayer = YES;
        toast_.layer.cornerRadius = 8;
        [view_ addSubview:toast_];
      }
      toast_.stringValue = t;
      const CGFloat w = std::max<CGFloat>(160, [t sizeWithAttributes:@{NSFontAttributeName : toast_.font}].width + 32);
      toast_.frame = NSMakeRect((view_.bounds.size.width - w) / 2, 14, w, 30);  // flipped: near the top
      toast_.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin;
      toast_.alphaValue = 1;
      const unsigned generation = ++toast_generation_;
      KestrelFpsLabel* label = toast_;
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        if (generation != this->toast_generation_) return;  // a newer notice is showing
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext* ctx) {
          ctx.duration = 0.4;
          label.animator.alphaValue = 0;
        }];
      });
    });
  }

  static MacWindowImpl* g_settings_window;

 private:
  void show_latest() {
    std::lock_guard lock(mutex_);
    update_pending_ = false;
    if (!surface_) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    // Re-assigning the same IOSurface makes Core Animation pick up new contents.
    view_.layer.contents = nil;
    view_.layer.contents = (__bridge id)surface_;
    [CATransaction commit];
  }

  WindowInput input_;
  NSWindow* window_;
  KestrelView* view_;
  KestrelContainer* container_;
  KestrelWindowDelegate* delegate_;
  KestrelEditorView* editor_ = nil;  // main thread only
  KestrelFpsLabel* toast_ = nil;     // main thread only
  NSAlert* ask_alert_ = nil;         // main thread only
  KestrelFpsLabel* rec_label_ = nil;  // main thread only
  NSTimer* rec_timer_ = nil;
  unsigned toast_generation_ = 0;
  std::function<void(NSInteger)> settings_finish_;
  CGImageRef last_image_ = nullptr;
  std::mutex mutex_;
  IOSurfaceRef surface_ = nullptr;
  uint32_t width_ = 0, height_ = 0;
  bool update_pending_ = false;
};

MacWindowImpl* MacWindowImpl::g_settings_window = nullptr;

}  // namespace

@implementation KestrelMenuTarget (Settings)
- (void)settingsButton:(NSButton*)b {
  if (MacWindowImpl::g_settings_window) MacWindowImpl::g_settings_window->settings_button(b.tag);
}
@end

// ---- downloads ----

@interface KestrelDownload : NSObject <NSURLSessionDownloadDelegate>
@property(nonatomic) std::function<void(double)> progress;
@property(nonatomic, copy) NSString* destination;
@property(nonatomic, copy) NSString* error;
@property(nonatomic) dispatch_semaphore_t done;
@end

@implementation KestrelDownload
- (void)URLSession:(NSURLSession*)s downloadTask:(NSURLSessionDownloadTask*)t
                 didWriteData:(int64_t)written totalBytesWritten:(int64_t)total
    totalBytesExpectedToWrite:(int64_t)expected {
  if (_progress && expected > 0) _progress(double(total) / double(expected));
}
- (void)URLSession:(NSURLSession*)s downloadTask:(NSURLSessionDownloadTask*)t
    didFinishDownloadingToURL:(NSURL*)location {
  NSHTTPURLResponse* r = (NSHTTPURLResponse*)t.response;
  if ([r isKindOfClass:NSHTTPURLResponse.class] && r.statusCode != 200) {
    self.error = [NSString stringWithFormat:@"server answered %ld", (long)r.statusCode];
    return;
  }
  NSURL* dest = [NSURL fileURLWithPath:_destination];
  [NSFileManager.defaultManager removeItemAtURL:dest error:nil];
  NSError* err = nil;
  // The temporary file is deleted when this returns: move it now.
  if (![NSFileManager.defaultManager moveItemAtURL:location toURL:dest error:&err]) {
    self.error = err.localizedDescription;
  }
}
- (void)URLSession:(NSURLSession*)s task:(NSURLSessionTask*)t didCompleteWithError:(NSError*)err {
  if (err && !_error) self.error = err.localizedDescription;
  dispatch_semaphore_signal(_done);
}
@end

// ---- setup window ----

namespace {

struct SetupUi {
  NSWindow* window;
  NSTextField* label;
  NSProgressIndicator* bar;
};

SetupUi make_setup_window(const std::string& title) {
  SetupUi ui;
  ui.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 460, 130)
                                          styleMask:NSWindowStyleMaskTitled
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
  ui.window.title = ns(title);
  ui.window.releasedWhenClosed = NO;
  NSView* content = ui.window.contentView;
  NSTextField* heading = [NSTextField labelWithString:@"Setting up Android"];
  heading.font = [NSFont boldSystemFontOfSize:15];
  heading.frame = NSMakeRect(24, 84, 412, 22);
  [content addSubview:heading];
  ui.label = [NSTextField labelWithString:@"Preparing…"];
  ui.label.frame = NSMakeRect(24, 58, 412, 20);
  ui.label.textColor = NSColor.secondaryLabelColor;
  [content addSubview:ui.label];
  ui.bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(24, 26, 412, 20)];
  ui.bar.style = NSProgressIndicatorStyleBar;
  ui.bar.minValue = 0;
  ui.bar.maxValue = 1;
  ui.bar.indeterminate = YES;
  [ui.bar startAnimation:nil];
  [content addSubview:ui.bar];
  [ui.window center];
  [ui.window makeKeyAndOrderFront:nil];
  return ui;
}

}  // namespace

namespace kestrel {

std::unique_ptr<MacWindow> MacWindow::create(const std::string& title, uint32_t width,
                                             uint32_t height, WindowInput input, bool show_sidebar) {
  ensure_app();
  return std::make_unique<MacWindowImpl>(title, width, height, std::move(input), show_sidebar);
}

void run_app_loop() {
  ensure_app();
  // No App Nap: Android keeps running at full speed (downloads, music) while the
  // window is in the background. Idle system sleep is still allowed.
  static id<NSObject> activity = [NSProcessInfo.processInfo
      beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                        reason:@"Running Android"];
  (void)activity;
  [NSApp activateIgnoringOtherApps:YES];
  [NSApp run];
}

void quit_app_loop() {
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp stop:nil];
    // stop: only takes effect after the next event; post one.
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
    [NSApp postEvent:wake atStart:YES];
  });
}

void set_open_files_handler(std::function<void(const std::string&)> handler) {
  ensure_app();
  g_open_files = std::move(handler);
}

Result<void> run_setup_window(
    const std::string& title,
    std::function<Result<void>(const std::function<void(const std::string&, double)>&)> task) {
  ensure_app();
  [NSApp activateIgnoringOtherApps:YES];
  SetupUi ui = make_setup_window(title);
  Result<void> result = make_error("setup did not finish");
  auto report = [ui](const std::string& status, double progress) {
    NSString* s = ns(status);
    dispatch_async(dispatch_get_main_queue(), ^{
      ui.label.stringValue = s;
      if (progress < 0) {
        ui.bar.indeterminate = YES;
        [ui.bar startAnimation:nil];
      } else {
        if (ui.bar.indeterminate) {
          [ui.bar stopAnimation:nil];  // otherwise the indeterminate stripe keeps drawing
          ui.bar.indeterminate = NO;
        }
        ui.bar.doubleValue = progress;
      }
    });
  };
  std::thread worker([&] {
    result = task(report);
    quit_app_loop();
  });
  [NSApp run];
  worker.join();
  [ui.window close];
  return result;
}

Result<void> download_file(const std::string& url, const std::string& path,
                           const std::function<void(double)>& progress) {
  KestrelDownload* d = [[KestrelDownload alloc] init];
  d.progress = progress;
  d.destination = ns(path);
  d.done = dispatch_semaphore_create(0);
  NSURLSession* session = [NSURLSession sessionWithConfiguration:NSURLSessionConfiguration.defaultSessionConfiguration
                                                        delegate:d
                                                   delegateQueue:nil];
  [[session downloadTaskWithURL:[NSURL URLWithString:ns(url)]] resume];
  dispatch_semaphore_wait(d.done, DISPATCH_TIME_FOREVER);
  [session finishTasksAndInvalidate];
  if (d.error) return make_error(std::string("download failed: ") + d.error.UTF8String);
  return {};
}

void show_fatal_error(const std::string& title, const std::string& text) {
  ensure_app();
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = ns(title);
  alert.informativeText = ns(text);
  alert.alertStyle = NSAlertStyleCritical;
  [NSApp activateIgnoringOtherApps:YES];
  [alert runModal];
}

std::string app_support_dir() {
  NSString* base = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
  return std::string([base stringByAppendingPathComponent:@"Kestrel"].UTF8String);
}

std::string app_logs_dir() {
  NSString* lib = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES).firstObject;
  const std::string base = [lib stringByAppendingPathComponent:@"Logs/Kestrel"].UTF8String;
  const std::string instance = current_instance();  // each instance keeps its own logs
  return instance.empty() ? base : base + "/" + instance;
}

std::pair<uint32_t, uint32_t> main_screen_pixels() {
  __block NSSize size = NSZeroSize;
  auto work = ^{
    NSScreen* s = NSScreen.mainScreen;
    size = NSMakeSize(s.frame.size.width * s.backingScaleFactor, s.frame.size.height * s.backingScaleFactor);
  };
  if (NSThread.isMainThread) work();
  else dispatch_sync(dispatch_get_main_queue(), work);
  return {static_cast<uint32_t>(size.width), static_cast<uint32_t>(size.height)};
}

}  // namespace kestrel
