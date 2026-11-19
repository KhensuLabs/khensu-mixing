// desktop-app/Source/midi/MacroMappingEngine.cpp
#include "MacroMappingEngine.h"
#include <algorithm>
#include <cmath>

namespace khensu {

namespace {
const char* sourceTypeName(MacroMappingEngine::SourceType t)
{
    switch (t) {
    case MacroMappingEngine::SourceType::MidiCC:
        return "cc";
    case MacroMappingEngine::SourceType::MidiNote:
        return "note";
    case MacroMappingEngine::SourceType::MidiPitchBend:
        return "pitchbend";
    case MacroMappingEngine::SourceType::Osc:
        return "osc";
    }
    return "cc";
}

std::optional<MacroMappingEngine::SourceType> sourceTypeFromName(const juce::String& s)
{
    if (s == "cc") return MacroMappingEngine::SourceType::MidiCC;
    if (s == "note") return MacroMappingEngine::SourceType::MidiNote;
    if (s == "pitchbend") return MacroMappingEngine::SourceType::MidiPitchBend;
    if (s == "osc") return MacroMappingEngine::SourceType::Osc;
    return std::nullopt;
}
} // namespace

bool MacroMappingEngine::Source::sameControlAs(const Source& o) const
{
    if (type != o.type) return false;
    if (type == SourceType::Osc) return oscAddress == o.oscAddress;
    const bool channelMatch = channel < 0 || o.channel < 0 || channel == o.channel;
    if (type == SourceType::MidiPitchBend) return channelMatch;
    return channelMatch && number == o.number;
}

juce::String MacroMappingEngine::Source::describe() const
{
    const juce::String ch = channel < 0 ? juce::String("any ch") : "ch " + juce::String(channel + 1);
    switch (type) {
    case SourceType::MidiCC:
        return "MIDI CC " + juce::String(number) + " (" + ch + ")";
    case SourceType::MidiNote:
        return "MIDI note " + juce::String(number) + " velocity (" + ch + ")";
    case SourceType::MidiPitchBend:
        return "MIDI pitch bend (" + ch + ")";
    case SourceType::Osc:
        return "OSC " + oscAddress;
    }
    return {};
}

double MacroMappingEngine::Mapping::apply(double normalised) const
{
    double x = juce::jlimit(0.0, 1.0, normalised);
    if (invert) x = 1.0 - x;
    switch (curve) {
    case Curve::Linear:
        break;
    case Curve::Exponential:
        x = x * x;
        break; // fine control at the bottom
    case Curve::Logarithmic:
        x = std::sqrt(x);
        break; // fine control at the top
    case Curve::SCurve:
        x = x * x * (3.0 - 2.0 * x);
        break; // smoothstep
    }
    return minValue + (maxValue - minValue) * x;
}

MacroMappingEngine::MacroMappingEngine(juce::StringArray targets) : targets_(std::move(targets)) {}

bool MacroMappingEngine::setMapping(const Mapping& m)
{
    if (!targets_.isEmpty() && !targets_.contains(m.target)) return false;
    mappings_.erase(std::remove_if(mappings_.begin(), mappings_.end(),
                                   [&m](const Mapping& e) { return e.source.sameControlAs(m.source); }),
                    mappings_.end());
    mappings_.push_back(m);
    return true;
}

bool MacroMappingEngine::removeMapping(size_t index)
{
    if (index >= mappings_.size()) return false;
    mappings_.erase(mappings_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool MacroMappingEngine::updateMapping(size_t index, const Mapping& m)
{
    if (index >= mappings_.size()) return false;
    if (!targets_.isEmpty() && !targets_.contains(m.target)) return false;
    mappings_[index] = m;
    return true;
}

bool MacroMappingEngine::armLearn(const juce::String& target)
{
    if (!targets_.isEmpty() && !targets_.contains(target)) return false;
    learnTarget_ = target;
    return true;
}

std::vector<MacroMappingEngine::Output> MacroMappingEngine::handle(const Source& source, double normalised)
{
    std::vector<Output> out;
    if (learnTarget_) {
        Mapping m;
        // Keep the user's range/curve if this macro was already mapped.
        for (const auto& existing : mappings_)
            if (existing.target == *learnTarget_) {
                m = existing;
                break;
            }
        m.source = source;
        m.target = *learnTarget_;
        learnTarget_.reset();
        mappings_.erase(
            std::remove_if(mappings_.begin(), mappings_.end(), [&m](const Mapping& e) { return e.target == m.target; }),
            mappings_.end());
        setMapping(m);
        out.push_back({m.target, m.apply(normalised)});
        return out;
    }
    for (const auto& m : mappings_)
        if (m.source.sameControlAs(source)) out.push_back({m.target, m.apply(normalised)});
    return out;
}

juce::var MacroMappingEngine::toVar() const
{
    juce::Array<juce::var> arr;
    for (const auto& m : mappings_) {
        auto* o = new juce::DynamicObject();
        o->setProperty("type", sourceTypeName(m.source.type));
        o->setProperty("channel", m.source.channel);
        o->setProperty("number", m.source.number);
        o->setProperty("osc_address", m.source.oscAddress);
        o->setProperty("target", m.target);
        o->setProperty("min", m.minValue);
        o->setProperty("max", m.maxValue);
        o->setProperty("curve", curveName(m.curve));
        o->setProperty("invert", m.invert);
        arr.add(juce::var(o));
    }
    return arr;
}

int MacroMappingEngine::fromVar(const juce::var& v)
{
    mappings_.clear();
    learnTarget_.reset();
    int loaded = 0;
    if (auto* arr = v.getArray()) {
        for (const auto& row : *arr) {
            const auto type = sourceTypeFromName(row["type"].toString());
            if (!type) continue;
            Mapping m;
            m.source.type = *type;
            m.source.channel = juce::jlimit(-1, 15, static_cast<int>(row["channel"]));
            m.source.number = juce::jlimit(0, 127, static_cast<int>(row["number"]));
            m.source.oscAddress = row["osc_address"].toString();
            if (m.source.type == SourceType::Osc && !m.source.oscAddress.startsWith("/")) continue;
            m.target = row["target"].toString();
            m.minValue = row.hasProperty("min") ? static_cast<double>(row["min"]) : 0.0;
            m.maxValue = row.hasProperty("max") ? static_cast<double>(row["max"]) : 100.0;
            m.curve = curveFromName(row["curve"].toString());
            m.invert = static_cast<bool>(row["invert"]);
            if (setMapping(m)) ++loaded;
        }
    }
    return loaded;
}

juce::String MacroMappingEngine::curveName(Curve c)
{
    switch (c) {
    case Curve::Linear:
        return "linear";
    case Curve::Exponential:
        return "exponential";
    case Curve::Logarithmic:
        return "logarithmic";
    case Curve::SCurve:
        return "s-curve";
    }
    return "linear";
}

MacroMappingEngine::Curve MacroMappingEngine::curveFromName(const juce::String& s)
{
    if (s == "exponential") return Curve::Exponential;
    if (s == "logarithmic") return Curve::Logarithmic;
    if (s == "s-curve") return Curve::SCurve;
    return Curve::Linear;
}

double MacroMappingEngine::normaliseMidi(std::uint32_t value, std::uint8_t valueBits)
{
    const int bits = valueBits == 0 ? 7 : juce::jlimit(1, 32, static_cast<int>(valueBits));
    const double maxValue = std::ldexp(1.0, bits) - 1.0;
    return juce::jlimit(0.0, 1.0, static_cast<double>(value) / maxValue);
}

double MacroMappingEngine::normaliseOsc(const juce::var& arg)
{
    if (arg.isInt() || arg.isInt64()) return juce::jlimit(0.0, 1.0, static_cast<double>(arg) / 127.0);
    return juce::jlimit(0.0, 1.0, static_cast<double>(arg));
}

} // namespace khensu
