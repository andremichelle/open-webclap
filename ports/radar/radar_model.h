/*
    Radar's model, shared by the DSP side and the editors: patterns of reflecting segments inside the unit circle,
    and the ray that finds a sample position for every moment of the loop.

    A ray starts at the pattern's origin, pointing at the loop's phase (0 up, clockwise, a full turn per loop). It
    reflects off the segments until it leaves the circle; where it leaves is the position in the sample (the same
    angle, a full turn is the whole sample). Without segments and with the origin in the centre, the loop plays
    straight through. Coordinates are the circle's: x right, y down, radius 1.

    A rewrite of radar's TypeScript (https://github.com/andremichelle/radar): the same geometry and ray, arcs
    through three points instead of the bend's closed form, curves solved as quadratics.
*/

#pragma once

#include <JuceHeader.h>

namespace radar
{
struct Point
{
    float x = 0.0f, y = 0.0f;

    Point operator+ (Point p) const { return { x + p.x, y + p.y }; }
    Point operator- (Point p) const { return { x - p.x, y - p.y }; }
    Point operator* (float s) const { return { x * s, y * s }; }
    bool operator== (const Point& p) const { return x == p.x && y == p.y; }

    float length() const { return std::sqrt (x * x + y * y); }
};

enum class Shape : std::uint8_t { line, arc, curve };

/** A reflecting segment from a to b. An arc bulges by bend (its peak lies bend * half the chord beside the chord's
    middle, to the right of a to b; 1 is a half circle, negative to the left); a curve is the quadratic through its
    control point c. */
struct Segment
{
    Shape shape = Shape::line;
    Point a, b, c;
    float bend = 0.0f;

    bool operator== (const Segment& s) const { return shape == s.shape && a == s.a && b == s.b && c == s.c && bend == s.bend; }

    /** The arc's peak. */
    Point bulge() const;

    /** Where the curve passes closest to its control point (t = 1/2), the handle that bends it. */
    Point curvePeak() const { return (a + b) * 0.25f + c * 0.5f; }
    void setCurvePeak (Point peak) { c = peak * 2.0f - (a + b) * 0.5f; }

    /** Points along the segment, for drawing and hit testing. */
    void sample (std::vector<Point>& points, int steps = 48) const;

    float distanceTo (Point p) const;
};

struct Pattern
{
    static constexpr int maxSegments = 96;

    Point origin;
    std::vector<Segment> segments;

    bool operator== (const Pattern& p) const { return origin == p.origin && segments == p.segments; }
    bool isEmpty() const { return segments.empty() && origin == Point {}; }

    void write (juce::OutputStream&) const;
    bool read (juce::InputStream&);
};

constexpr int numPatterns = 12;
using Patterns = std::array<Pattern, numPatterns>;

/** The ray's way through a pattern. */
struct Trace
{
    static constexpr int maxBounces = 12; // more, and the ray counts as trapped

    /** Where it left the circle, in turns (0 to 1, 0 up, clockwise). Invalid when it never left (trapped). */
    float exit = 0.0f;
    bool escaped = false;
};

/** Traces the ray of a phase (turns) through a pattern. With path, also every point it touched (the origin, the
    reflections, the exit). */
Trace trace (const Pattern&, float phase, std::vector<Point>* path = nullptr);

/** A point on the circle for an angle in turns, and back. */
inline Point onCircle (float turns, float radius = 1.0f)
{
    const auto angle = turns * juce::MathConstants<float>::twoPi;
    return { std::sin (angle) * radius, -std::cos (angle) * radius };
}

inline float turnsOf (Point p)
{
    auto turns = std::atan2 (p.x, -p.y) / juce::MathConstants<float>::twoPi;
    return turns < 0.0f ? turns + 1.0f : turns;
}

/** Keeps a point inside the circle (segment ends, the origin). */
inline Point inside (Point p, float radius = 1.0f)
{
    const auto length = p.length();
    return length > radius ? p * (radius / length) : p;
}
} // namespace radar
