// Scripts on objects: an object holds a list of scripts (a "ball.lua" and a
// "hover.lua" can both sit on the ball), and each script declares inputs that
// the Inspector shows and the scene saves per object.
//
// A script declares its inputs in comments at the top of the file, so the
// editor can read them without running Lua:
//
//   -- @prop speed   number = 5  range 0 20
//   -- @prop label   text   = "Hello"
//   -- @prop mode    list(idle, patrol, chase) = patrol
//   -- @prop active  toggle = true
//   -- @prop target  object
//
// An `object` input links to another object in the scene; at run time the
// script reaches that object's scripts through it (target:script("door"),
// see docs/scripting.md), which is how scripts on different objects work
// together. The editor side of that is here too: find a script on an object
// by name and read or change its inputs.
#pragma once

#include <stdint.h>

namespace fe {

typedef uint32_t ObjectId;

enum class PropType : uint8_t { Number, Text, Toggle, List, Object };

constexpr int kMaxScripts = 6;          // per object
constexpr int kMaxProps = 12;           // per script
constexpr int kMaxChoices = 8;          // per list input

struct ScriptProp {
    char name[32] = {};
    PropType type = PropType::Number;
    // Number
    float number = 0;
    float min = 0, max = 0;             // min == max: no range
    // Text
    char text[64] = {};
    // Toggle
    bool toggle = false;
    // List: the choices, and the chosen one
    char choices[kMaxChoices][24] = {};
    int choice_count = 0;
    int choice = 0;
    // Object: the linked object (0 = none)
    ObjectId object = 0;
};

struct ScriptSlot {
    char path[256] = {};
    ScriptProp props[kMaxProps];
    int prop_count = 0;
    bool missing = false;               // the file could not be read the last time

    // The input called `name`, or null.
    ScriptProp* prop(const char* name);
    const ScriptProp* prop(const char* name) const;
    // "ball" for ".../ball.lua": the name other scripts use to find it.
    const char* name() const;
    bool name_is(const char* n) const;
};

// Reads the `@prop` declarations from a script's text into `out` (at most
// kMaxProps, with their default values). Returns how many were found; lines
// that do not parse are skipped and counted in *bad when it is given.
int parse_script_props(const char* text, uint32_t size, ScriptProp* out, int max, int* bad = nullptr);

// Brings a slot's inputs in line with freshly parsed declarations: inputs
// still declared with the same type keep the value set in the editor (a
// number is clamped to a new range, a list choice is looked up by its text);
// new inputs get their defaults; inputs no longer declared are dropped.
void merge_script_props(ScriptSlot* slot, const ScriptProp* declared, int count);

const char* prop_type_name(PropType t);

}  // namespace fe
