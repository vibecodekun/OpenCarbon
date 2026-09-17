#include "platform/win32/Script.h"
#include "app/Input.h"
#include <fstream>
#include <sstream>

namespace script {
namespace {

std::vector<Event> events;
bool loaded = false;

bool ParseButtons(const std::string& spec, uint64_t& out) {
    static const struct { const char* name; uint64_t bit; } kNames[] = {
        {"A", 1u << 0},      {"B", 1u << 1},       {"X", 1u << 2},     {"Y", 1u << 3},
        {"LSTICK", 1u << 4}, {"RSTICK", 1u << 5},  {"L", 1u << 6},     {"R", 1u << 7},
        {"ZL", 1u << 8},     {"ZR", 1u << 9},      {"PLUS", 1u << 10}, {"MINUS", 1u << 11},
        {"LEFT", 1u << 12},  {"UP", 1u << 13},     {"RIGHT", 1u << 14}, {"DOWN", 1u << 15},
    };
    out = 0;
    std::stringstream ss(spec);
    std::string name;
    while (std::getline(ss, name, '+')) {
        bool found = false;
        for (const auto& n : kNames) {
            if (name == n.name) {
                out |= n.bit;
                found = true;
            }
        }
        if (!found) return false;
    }
    return out != 0;
}

}  // namespace

bool Load(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    int64_t previous = 0;
    std::string line;
    for (int lineNo = 1; std::getline(in, line); ++lineNo) {
        if (size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string frameText, command;
        if (!(ls >> frameText)) continue;
        const auto fail = [&](const char* what) {
            error = path + ":" + std::to_string(lineNo) + ": " + what;
            return false;
        };
        if (!(ls >> command)) return fail("missing command");

        Event e;
        try {
            e.frame = (frameText[0] == '+') ? previous + std::stoll(frameText.substr(1)) : std::stoll(frameText);
        } catch (...) {
            return fail("bad frame number");
        }
        if (command == "hold") {
            std::string buttons;
            if (!(ls >> e.frames >> buttons) || !ParseButtons(buttons, e.buttons)) return fail("usage: hold <frames> <BUTTON>[+...]");
            e.kind = Event::Kind::Hold;
        } else if (command == "stick") {
            if (!(ls >> e.frames >> e.lx >> e.ly >> e.rx >> e.ry)) return fail("usage: stick <frames> <lx> <ly> <rx> <ry>");
            e.kind = Event::Kind::Stick;
        } else if (command == "shot") {
            if (!(ls >> e.text)) return fail("usage: shot <name>");
            e.kind = Event::Kind::Shot;
        } else if (command == "log") {
            std::getline(ls >> std::ws, e.text);
            e.kind = Event::Kind::Log;
        } else if (command == "quit") {
            e.kind = Event::Kind::Quit;
        } else {
            return fail("unknown command");
        }
        previous = e.frame;
        events.push_back(e);
    }
    loaded = true;
    return true;
}

bool Active() { return loaded; }

void PadState(int64_t frame, NpadSnapshot& out) {
    out = NpadSnapshot{};
    out.connected = true;
    for (const Event& e : events) {
        if (frame < e.frame || frame >= e.frame + e.frames) continue;
        if (e.kind == Event::Kind::Hold) {
            out.buttons |= e.buttons;
        } else if (e.kind == Event::Kind::Stick) {
            out.lx = e.lx;
            out.ly = e.ly;
            out.rx = e.rx;
            out.ry = e.ry;
        }
    }
}

std::vector<const Event*> EventsAt(int64_t frame) {
    std::vector<const Event*> out;
    for (const Event& e : events)
        if (e.frame == frame && (e.kind == Event::Kind::Shot || e.kind == Event::Kind::Log || e.kind == Event::Kind::Quit))
            out.push_back(&e);
    return out;
}

}  // namespace script
