#pragma once

#include <string_view>

constexpr const char* GrokBotCaption(std::string_view clip) {
    constexpr struct {
        std::string_view clip;
        const char* text;
    } captions[] = {
        {"idle", "Just hanging out"},
        {"listening", "All ears"},
        {"streaming", "Chatting with you"},
        {"loading", "Getting ready"},
        {"radar", "Looking around"},
        {"waiting", "Waiting patiently"},
        {"searching", "On the lookout"},
        {"waking", "I'm awake!"},
        {"approving", "Looks good!"},
        {"uploading", "Sending it over"},
        {"failed", "Oops..."},
        {"happy", "Feeling happy"},
        {"laughing", "Can't stop laughing"},
        {"playful", "Feeling playful"},
        {"shy", "A little shy"},
        {"proud", "Feeling proud"},
        {"sad", "Feeling blue"},
        {"sleeping", "Sweet dreams"},
        {"angry", "A little grumpy"},
        {"surprised", "Oh, wow!"},
        {"thinking", "Let me think..."},
        {"confused", "A little puzzled"},
        {"scared", "That startled me!"},
        {"alerting", "Heads up!"},
        {"excited", "So excited!"},
        {"curious", "What's over there?"},
        {"drowsy", "Getting sleepy"},
        {"notifying", "Psst, over here!"},
        {"humming", "Humming a tune"},
        {"suspicious", "Hmm, really?"},
        {"finished", "All done!"},
        {"celebrate", "Time to celebrate!"},
        {"working", "Keeping busy"},
        {"turntable", "Feeling groovy"},
        {"logo", "Hello there!"},
        {"spawning", "Here I am!"},
        {"writing", "Jotting it down"},
        {"receiving", "Taking it in"},
        {"powering-down", "Taking a rest"},
    };
    for (const auto& caption : captions) {
        if (caption.clip == clip)
            return caption.text;
    }
    return "Right here with you";
}

static_assert(std::string_view(GrokBotCaption("shy")) == "A little shy");
static_assert(std::string_view(GrokBotCaption("sleeping")) == "Sweet dreams");
static_assert(std::string_view(GrokBotCaption("unknown")) == "Right here with you");
