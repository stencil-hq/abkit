/*
 * Copyright (C) 2014 Igalia S.L.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebEditorClient.h"

#include "WebPage.h"
#include <WebCore/Document.h>
#include <WebCore/Editor.h>
#include <WebCore/EventNames.h>
#include <WebCore/FrameDestructionObserverInlines.h>
#include <WebCore/KeyboardEvent.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/Node.h>
#include <WebCore/NodeDocument.h>
#include <WebCore/NodeInlines.h>
#include <WebCore/PlatformKeyboardEvent.h>
#include <WebCore/WindowsKeyboardCodes.h>
#include <array>
#include <wtf/NeverDestroyed.h>

namespace WebKit {
using namespace WebCore;

// The idea for the array/map below comes from Blink's EditingBehavior.cpp.

static const unsigned CtrlKey  = 1 << 0;
static const unsigned AltKey   = 1 << 1;
static const unsigned ShiftKey = 1 << 2;
static const unsigned MetaKey  = 1 << 3;

// Keys with special meaning. These will be delegated to the editor using
// the execCommand() method. A name may hold several commands, separated by
// spaces, run in order.
struct KeyDownEntry {
    unsigned virtualKey;
    unsigned modifiers;
    const char* name;
};

struct KeyPressEntry {
    unsigned charCode;
    unsigned modifiers;
    const char* name;
};

#if OS(DARWIN)
// The Mac's: AppKit's standard key bindings (StandardKeyBinding.dict, the
// selectors named as WebKit's Cocoa port names their commands) and the
// Edit menu's key equivalents, which a Mac page gets when it leaves the key.
static constexpr auto keyDownEntries = WTF::toArray<KeyDownEntry>({
    { VK_LEFT,   0,                         "MoveLeft"                                    },
    { VK_LEFT,   ShiftKey,                  "MoveLeftAndModifySelection"                  },
    { VK_LEFT,   AltKey,                    "MoveWordLeft"                                },
    { VK_LEFT,   AltKey | ShiftKey,         "MoveWordLeftAndModifySelection"              },
    { VK_LEFT,   MetaKey,                   "MoveToLeftEndOfLine"                         },
    { VK_LEFT,   MetaKey | ShiftKey,        "MoveToLeftEndOfLineAndModifySelection"       },
    { VK_LEFT,   CtrlKey,                   "MoveToLeftEndOfLine"                         },
    { VK_LEFT,   CtrlKey | ShiftKey,        "MoveToLeftEndOfLineAndModifySelection"       },
    { VK_RIGHT,  0,                         "MoveRight"                                   },
    { VK_RIGHT,  ShiftKey,                  "MoveRightAndModifySelection"                 },
    { VK_RIGHT,  AltKey,                    "MoveWordRight"                               },
    { VK_RIGHT,  AltKey | ShiftKey,         "MoveWordRightAndModifySelection"             },
    { VK_RIGHT,  MetaKey,                   "MoveToRightEndOfLine"                        },
    { VK_RIGHT,  MetaKey | ShiftKey,        "MoveToRightEndOfLineAndModifySelection"      },
    { VK_RIGHT,  CtrlKey,                   "MoveToRightEndOfLine"                        },
    { VK_RIGHT,  CtrlKey | ShiftKey,        "MoveToRightEndOfLineAndModifySelection"      },
    { VK_UP,     0,                         "MoveUp"                                      },
    { VK_UP,     ShiftKey,                  "MoveUpAndModifySelection"                    },
    { VK_UP,     AltKey,                    "MoveBackward MoveToBeginningOfParagraph"     },
    { VK_UP,     AltKey | ShiftKey,         "MoveParagraphBackwardAndModifySelection"     },
    { VK_UP,     MetaKey,                   "MoveToBeginningOfDocument"                   },
    { VK_UP,     MetaKey | ShiftKey,        "MoveToBeginningOfDocumentAndModifySelection" },
    { VK_UP,     CtrlKey,                   "ScrollPageBackward"                          },
    { VK_DOWN,   0,                         "MoveDown"                                    },
    { VK_DOWN,   ShiftKey,                  "MoveDownAndModifySelection"                  },
    { VK_DOWN,   AltKey,                    "MoveForward MoveToEndOfParagraph"            },
    { VK_DOWN,   AltKey | ShiftKey,         "MoveParagraphForwardAndModifySelection"      },
    { VK_DOWN,   MetaKey,                   "MoveToEndOfDocument"                         },
    { VK_DOWN,   MetaKey | ShiftKey,        "MoveToEndOfDocumentAndModifySelection"       },
    { VK_DOWN,   CtrlKey,                   "ScrollPageForward"                           },
    { VK_HOME,   0,                         "ScrollToBeginningOfDocument"                 },
    { VK_HOME,   ShiftKey,                  "MoveToBeginningOfDocumentAndModifySelection" },
    { VK_END,    0,                         "ScrollToEndOfDocument"                       },
    { VK_END,    ShiftKey,                  "MoveToEndOfDocumentAndModifySelection"       },
    { VK_PRIOR,  0,                         "ScrollPageBackward"                          },
    { VK_PRIOR,  ShiftKey,                  "MovePageUpAndModifySelection"                },
    { VK_PRIOR,  AltKey,                    "MovePageUp"                                  },
    { VK_NEXT,   0,                         "ScrollPageForward"                           },
    { VK_NEXT,   ShiftKey,                  "MovePageDownAndModifySelection"              },
    { VK_NEXT,   AltKey,                    "MovePageDown"                                },
    { VK_BACK,   0,                         "DeleteBackward"                              },
    { VK_BACK,   ShiftKey,                  "DeleteBackward"                              },
    { VK_BACK,   AltKey,                    "DeleteWordBackward"                          },
    { VK_BACK,   CtrlKey | AltKey,          "DeleteWordBackward"                          },
    { VK_BACK,   MetaKey,                   "DeleteToBeginningOfLine"                     },
    { VK_BACK,   CtrlKey,                   "DeleteBackwardByDecomposingPreviousCharacter"},
    { VK_DELETE, 0,                         "DeleteForward"                               },
    { VK_DELETE, AltKey,                    "DeleteWordForward"                           },
    { 'A',       CtrlKey,                   "MoveToBeginningOfParagraph"                  },
    { 'A',       CtrlKey | ShiftKey,        "MoveToBeginningOfParagraphAndModifySelection"},
    { 'E',       CtrlKey,                   "MoveToEndOfParagraph"                        },
    { 'E',       CtrlKey | ShiftKey,        "MoveToEndOfParagraphAndModifySelection"      },
    { 'B',       CtrlKey,                   "MoveBackward"                                },
    { 'B',       CtrlKey | ShiftKey,        "MoveBackwardAndModifySelection"              },
    { 'B',       CtrlKey | AltKey,          "MoveWordBackward"                            },
    { 'B',       CtrlKey | AltKey | ShiftKey, "MoveWordBackwardAndModifySelection"        },
    { 'F',       CtrlKey,                   "MoveForward"                                 },
    { 'F',       CtrlKey | ShiftKey,        "MoveForwardAndModifySelection"               },
    { 'F',       CtrlKey | AltKey,          "MoveWordForward"                             },
    { 'F',       CtrlKey | AltKey | ShiftKey, "MoveWordForwardAndModifySelection"         },
    { 'N',       CtrlKey,                   "MoveDown"                                    },
    { 'N',       CtrlKey | ShiftKey,        "MoveDownAndModifySelection"                  },
    { 'P',       CtrlKey,                   "MoveUp"                                      },
    { 'P',       CtrlKey | ShiftKey,        "MoveUpAndModifySelection"                    },
    { 'D',       CtrlKey,                   "DeleteForward"                               },
    { 'H',       CtrlKey,                   "DeleteBackward"                              },
    { 'H',       CtrlKey | AltKey,          "DeleteWordBackward"                          },
    { 'K',       CtrlKey,                   "DeleteToEndOfParagraph"                      },
    { 'Y',       CtrlKey,                   "Yank"                                        },
    { 'T',       CtrlKey,                   "Transpose"                                   },
    { 'V',       CtrlKey,                   "MovePageDown"                                },
    { 'V',       CtrlKey | ShiftKey,        "MovePageDownAndModifySelection"              },
    { 'C',       MetaKey,                   "Copy"                                        },
    { 'X',       MetaKey,                   "Cut"                                         },
    { 'V',       MetaKey,                   "Paste"                                       },
    { 'V',       MetaKey | AltKey | ShiftKey, "PasteAndMatchStyle"                        },
    { 'A',       MetaKey,                   "SelectAll"                                   },
    { 'Z',       MetaKey,                   "Undo"                                        },
    { 'Z',       MetaKey | ShiftKey,        "Redo"                                        },
    { 'B',       MetaKey,                   "ToggleBold"                                  },
    { 'I',       MetaKey,                   "ToggleItalic"                                },
    { 'U',       MetaKey,                   "ToggleUnderline"                             },
    { VK_TAB,    0,                         "InsertTab"                                   },
    { VK_TAB,    ShiftKey,                  "InsertBacktab"                               },
    { VK_TAB,    AltKey,                    "InsertTab"                                   },
    { VK_RETURN, 0,                         "InsertNewline"                               },
    { VK_RETURN, AltKey,                    "InsertNewline"                               },
    { VK_RETURN, ShiftKey,                  "InsertLineBreak"                             },
    { VK_RETURN, CtrlKey,                   "InsertLineBreak"                             },
});

static constexpr auto keyPressEntries = WTF::toArray<KeyPressEntry>({
    { '\t',   0,                  "InsertTab"                                  },
    { '\t',   ShiftKey,           "InsertBacktab"                              },
    { '\t',   AltKey,             "InsertTab"                                  },
    { '\r',   0,                  "InsertNewline"                              },
    { '\r',   AltKey,             "InsertNewline"                              },
    { '\r',   ShiftKey,           "InsertLineBreak"                            },
    { '\r',   CtrlKey,            "InsertLineBreak"                            },
});
#else
static constexpr auto keyDownEntries = WTF::toArray<KeyDownEntry>({
    { VK_LEFT,   0,                  "MoveLeft"                                },
    { VK_LEFT,   ShiftKey,           "MoveLeftAndModifySelection"              },
    { VK_LEFT,   CtrlKey,            "MoveWordLeft"                            },
    { VK_LEFT,   CtrlKey | ShiftKey,
        "MoveWordLeftAndModifySelection"                                       },
    { VK_RIGHT,  0,                  "MoveRight"                               },
    { VK_RIGHT,  ShiftKey,           "MoveRightAndModifySelection"             },
    { VK_RIGHT,  CtrlKey,            "MoveWordRight"                           },
    { VK_RIGHT,  CtrlKey | ShiftKey, "MoveWordRightAndModifySelection"         },
    { VK_UP,     0,                  "MoveUp"                                  },
    { VK_UP,     ShiftKey,           "MoveUpAndModifySelection"                },
    { VK_PRIOR,  ShiftKey,           "MovePageUpAndModifySelection"            },
    { VK_DOWN,   0,                  "MoveDown"                                },
    { VK_DOWN,   ShiftKey,           "MoveDownAndModifySelection"              },
    { VK_NEXT,   ShiftKey,           "MovePageDownAndModifySelection"          },
    { VK_UP,     CtrlKey | ShiftKey, "MoveParagraphBackwardAndModifySelection" },
    { VK_DOWN,   CtrlKey | ShiftKey, "MoveParagraphForwardAndModifySelection"  },
    { VK_PRIOR,  0,                  "MovePageUp"                              },
    { VK_NEXT,   0,                  "MovePageDown"                            },
    { VK_HOME,   0,                  "MoveToBeginningOfLine"                   },
    { VK_HOME,   ShiftKey,
        "MoveToBeginningOfLineAndModifySelection"                              },
    { VK_HOME,   CtrlKey,            "MoveToBeginningOfDocument"               },
    { VK_HOME,   CtrlKey | ShiftKey,
        "MoveToBeginningOfDocumentAndModifySelection"                          },
    { VK_END,    0,                  "MoveToEndOfLine"                         },
    { VK_END,    ShiftKey,           "MoveToEndOfLineAndModifySelection"       },
    { VK_END,    CtrlKey,            "MoveToEndOfDocument"                     },
    { VK_END,    CtrlKey | ShiftKey,
        "MoveToEndOfDocumentAndModifySelection"                                },
    { VK_BACK,   0,                  "DeleteBackward"                          },
    { VK_BACK,   ShiftKey,           "DeleteBackward"                          },
    { VK_DELETE, 0,                  "DeleteForward"                           },
    { VK_BACK,   CtrlKey,            "DeleteWordBackward"                      },
    { VK_DELETE, CtrlKey,            "DeleteWordForward"                       },
    { 'B',         CtrlKey,          "ToggleBold"                              },
    { 'I',         CtrlKey,          "ToggleItalic"                            },
    { 'U',         CtrlKey,          "ToggleUnderline"                         },
    { VK_OEM_PERIOD, CtrlKey,        "Cancel"                                  },
    { VK_TAB,    0,                  "InsertTab"                               },
    { VK_TAB,    ShiftKey,           "InsertBacktab"                           },
    { VK_RETURN, 0,                  "InsertNewline"                           },
    { VK_RETURN, CtrlKey,            "InsertNewline"                           },
    { VK_RETURN, AltKey,             "InsertNewline"                           },
    { VK_RETURN, AltKey | ShiftKey,  "InsertNewline"                           },
    { VK_RETURN, ShiftKey,           "InsertLineBreak"                         },
    // These probably need handling somewhere else so do not execute them. The
    // 'Cut' command is removing text so let's avoid losing the user losing data
    // until we implement clipboard support wherever it should be.
    { VK_INSERT, CtrlKey,            "Copy"                                    },
    { VK_INSERT, ShiftKey,           "Paste"                                   },
    { VK_DELETE, ShiftKey,           "Cut"                                     },
    { 'C',         CtrlKey,          "Copy"                                    },
    { 'V',         CtrlKey,          "Paste"                                   },
    { 'V',         CtrlKey | ShiftKey, "PasteAndMatchStyle"                    },
    { 'X',         CtrlKey,          "Cut"                                     },
    { 'A',         CtrlKey,          "SelectAll"                               },
    { 'Z',         CtrlKey,          "Undo"                                    },
    { 'Z',         CtrlKey | ShiftKey, "Redo"                                  },
    { 'Y',         CtrlKey,          "Redo"                                    },
    { VK_INSERT, 0,                  "OverWrite"                               },
});

static constexpr auto keyPressEntries = WTF::toArray<KeyPressEntry>({
    { '\t',   0,                  "InsertTab"                                  },
    { '\t',   ShiftKey,           "InsertBacktab"                              },
    { '\r',   0,                  "InsertNewline"                              },
    { '\r',   CtrlKey,            "InsertNewline"                              },
    { '\r',   ShiftKey,           "InsertLineBreak"                            },
    { '\r',   AltKey,             "InsertNewline"                              },
    { '\r',   AltKey | ShiftKey,  "InsertNewline"                              },
});
#endif

static const char* interpretKeyEvent(const KeyboardEvent& event)
{
    static NeverDestroyed<HashMap<int, const char*>> keyDownCommandsMap;
    static NeverDestroyed<HashMap<int, const char*>> keyPressCommandsMap;

    if (keyDownCommandsMap.get().isEmpty()) {
        for (const auto& entry : keyDownEntries)
            keyDownCommandsMap.get().set(entry.modifiers << 16 | entry.virtualKey, entry.name);
        for (const auto& entry : keyPressEntries)
            keyPressCommandsMap.get().set(entry.modifiers << 16 | entry.charCode, entry.name);
    }

    unsigned modifiers = 0;
    if (event.shiftKey())
        modifiers |= ShiftKey;
    if (event.altKey())
        modifiers |= AltKey;
    if (event.ctrlKey())
        modifiers |= CtrlKey;
    if (event.metaKey())
        modifiers |= MetaKey;

    if (event.type() == eventNames().keydownEvent) {
        int mapKey = modifiers << 16 | event.keyCode();
        return mapKey ? keyDownCommandsMap.get().get(mapKey) : nullptr;
    }

    int mapKey = modifiers << 16 | event.charCode();
    return mapKey ? keyPressCommandsMap.get().get(mapKey) : nullptr;
}

static void handleKeyPress(LocalFrame& frame, KeyboardEvent& event, const PlatformKeyboardEvent& platformEvent)
{
    // Every command reached from here inserts text, so nothing is left to do
    // for content that cannot be edited.
    if (!frame.editor().canEdit())
        return;

    auto commandName = String::fromLatin1(interpretKeyEvent(event));

    if (!commandName.isEmpty()) {
        frame.editor().command(commandName).execute();
        event.setDefaultHandled();
        return;
    }

    // Don't insert null or control characters as they can result in unexpected behaviour
    if (event.charCode() < ' ')
        return;

    // Don't insert anything if a modifier is pressed and it has not been handled yet.
    // On a Mac, Option types characters of its own (⌥2 is ™); Command and Control type none.
#if OS(DARWIN)
    if (platformEvent.controlKey() || platformEvent.metaKey())
        return;
#else
    if (platformEvent.controlKey() || platformEvent.altKey())
        return;
#endif

    if (frame.editor().insertText(platformEvent.text(), &event))
        event.setDefaultHandled();
}

static void handleKeyDown(LocalFrame& frame, KeyboardEvent& event, const PlatformKeyboardEvent&)
{
    auto commandNames = String::fromLatin1(interpretKeyEvent(event));
    if (commandNames.isEmpty())
        return;

    bool handled = false;
    for (auto commandName : StringView(commandNames).split(' ')) {
        // We shouldn't insert text through the editor. Let WebCore decide
        // how to handle that (say, Tab, which could also be used to
        // change focus).
        Editor::Command command = frame.editor().command(commandName.toString());
        if (command.isTextInsertion())
            return;

        // A command the selection does not allow, like moving the caret through
        // content that is not editable, has to fall through to its default handling.
        handled |= command.execute();
    }
    if (handled)
        event.setDefaultHandled();
}

void WebEditorClient::handleKeyboardEvent(WebCore::KeyboardEvent& event)
{
    ASSERT(event.target());
    auto* frame = downcast<Node>(event.target())->document().frame();
    ASSERT(frame);

    // FIXME: Reorder the checks in a more sensible way.

    auto* platformEvent = event.underlyingPlatformEvent();
    if (!platformEvent)
        return;

    // If this was an IME event don't do anything.
    if (platformEvent->windowsVirtualKeyCode() == VK_PROCESSKEY)
        return;

    // This is just a normal text insertion, so wait to execute the insertion
    // until a keypress event happens. This will ensure that the insertion will not
    // be reflected in the contents of the field until the keyup DOM event.
    if (event.type() == eventNames().keypressEvent)
        return handleKeyPress(*frame, event, *platformEvent);
    if (event.type() == eventNames().keydownEvent)
        return handleKeyDown(*frame, event, *platformEvent);
}

} // namespace WebKit
