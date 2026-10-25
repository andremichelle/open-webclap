// What OpenSpatialDelay uses of juce_osc, without sockets: a page has no UDP, so connect() fails and the plugin
// reports OSC as not connected. Messages are still built (ADM-OSC send) and parsed (tests), nothing goes out.

#pragma once

#include <juce_core/juce_core.h>

namespace juce
{
class OSCAddressPattern
{
public:
    OSCAddressPattern() = default;
    OSCAddressPattern (const String& text) : pattern (text) {}
    OSCAddressPattern (const char* text) : pattern (text) {}

    String toString() const { return pattern; }

private:
    String pattern;
};

class OSCArgument
{
public:
    OSCArgument (int32 value) : isInt (true), intValue (value) {}
    OSCArgument (float value) : isFloat (true), floatValue (value) {}

    bool isInt32() const noexcept   { return isInt; }
    bool isFloat32() const noexcept { return isFloat; }
    int32 getInt32() const noexcept { return intValue; }
    float getFloat32() const noexcept { return floatValue; }

private:
    bool isInt = false, isFloat = false;
    int32 intValue = 0;
    float floatValue = 0.0f;
};

class OSCMessage
{
public:
    OSCMessage (const OSCAddressPattern& address) : pattern (address) {}
    OSCMessage (const String& address) : pattern (address) {}
    OSCMessage (const char* address) : pattern (address) {}

    const OSCAddressPattern& getAddressPattern() const noexcept { return pattern; }
    int size() const noexcept { return arguments.size(); }
    const OSCArgument& operator[] (int index) const noexcept { return arguments.getReference (index); }

    void addInt32 (int32 value)   { arguments.add (OSCArgument (value)); }
    void addFloat32 (float value) { arguments.add (OSCArgument (value)); }

private:
    OSCAddressPattern pattern;
    Array<OSCArgument> arguments;
};

class OSCReceiver
{
public:
    struct MessageLoopCallback {};
    struct RealtimeCallback {};

    template <typename CallbackType>
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void oscMessageReceived (const OSCMessage& message) = 0;
    };

    bool connect (int) { return false; }
    bool disconnect() { return true; }

    template <typename CallbackType>
    void addListener (Listener<CallbackType>*) {}

    template <typename CallbackType>
    void removeListener (Listener<CallbackType>*) {}
};

class OSCSender
{
public:
    bool connect (const String&, int) { return false; }
    bool disconnect() { return true; }
    bool send (const OSCMessage&) { return false; }
};
} // namespace juce
