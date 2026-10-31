#include "radar_model.h"

namespace radar
{
namespace
{
// The tracing runs in doubles: reflections chain, and a ray that grazes a segment must not slip through its end
struct Vec
{
    double x, y;

    Vec operator+ (Vec v) const { return { x + v.x, y + v.y }; }
    Vec operator- (Vec v) const { return { x - v.x, y - v.y }; }
    Vec operator* (double s) const { return { x * s, y * s }; }
};

Vec vec (Point p) { return { p.x, p.y }; }
double dot (Vec a, Vec b) { return a.x * b.x + a.y * b.y; }
double cross (Vec a, Vec b) { return a.x * b.y - a.y * b.x; }

Vec normalised (Vec v)
{
    const auto length = std::sqrt (dot (v, v));
    return length > 0.0 ? v * (1.0 / length) : v;
}

constexpr double epsilon = 1.0e-5; // a ray leaving a reflection does not hit the same point again

struct Hit
{
    double distance;
    Vec normal;
};

std::optional<Hit> hitLine (Vec a, Vec b, Vec origin, Vec direction)
{
    const auto d = b - a;
    const auto denominator = cross (direction, d);

    if (std::abs (denominator) < 1.0e-12)
        return {};

    const auto w = a - origin;
    const auto t = cross (w, d) / denominator;
    const auto u = cross (w, direction) / denominator;

    if (t <= epsilon || u < 0.0 || u > 1.0)
        return {};

    return Hit { t, normalised ({ -d.y, d.x }) };
}

/** The circle through three points, if they are not on a line. */
std::optional<std::pair<Vec, double>> circleThrough (Vec a, Vec b, Vec c)
{
    const auto d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));

    if (std::abs (d) < 1.0e-9)
        return {};

    const auto aa = dot (a, a), bb = dot (b, b), cc = dot (c, c);
    const Vec centre { (aa * (b.y - c.y) + bb * (c.y - a.y) + cc * (a.y - b.y)) / d,
                       (aa * (c.x - b.x) + bb * (a.x - c.x) + cc * (b.x - a.x)) / d };
    return std::make_pair (centre, std::sqrt (dot (a - centre, a - centre)));
}

std::optional<Hit> hitArc (const Segment& segment, Vec origin, Vec direction)
{
    const auto a = vec (segment.a), b = vec (segment.b), peak = vec (segment.bulge());
    const auto circle = circleThrough (a, b, peak);

    if (std::abs (segment.bend) < 1.0e-4f || ! circle)
        return hitLine (a, b, origin, direction);

    // The arc is the part of its circle on the peak's side of the chord
    const auto [centre, radius] = *circle;
    const auto chord = b - a;
    const auto side = cross (chord, peak - a) > 0.0;
    const auto e = origin - centre;
    const auto half = dot (e, direction);
    const auto discriminant = half * half - (dot (e, e) - radius * radius);

    if (discriminant < 0.0)
        return {};

    const auto root = std::sqrt (discriminant);

    for (const auto t : { -half - root, -half + root })
    {
        if (t <= epsilon)
            continue;

        const auto point = origin + direction * t;

        if ((cross (chord, point - a) > 0.0) == side)
            return Hit { t, (point - centre) * (1.0 / radius) };
    }

    return {};
}

std::optional<Hit> hitCurve (const Segment& segment, Vec origin, Vec direction)
{
    const auto a = vec (segment.a), b = vec (segment.b), c = vec (segment.c);
    const Vec normal { -direction.y, direction.x };

    // The curve's distance across the ray, a quadratic in t
    const auto qa = dot (normal, a - c * 2.0 + b);
    const auto qb = dot (normal, (c - a) * 2.0);
    const auto qc = dot (normal, a - origin);

    double roots[2];
    auto count = 0;

    if (std::abs (qa) < 1.0e-12)
    {
        if (std::abs (qb) > 1.0e-12)
            roots[count++] = -qc / qb;
    }
    else
    {
        const auto discriminant = qb * qb - 4.0 * qa * qc;

        if (discriminant >= 0.0)
        {
            const auto root = std::sqrt (discriminant);
            roots[count++] = (-qb - root) / (2.0 * qa);
            roots[count++] = (-qb + root) / (2.0 * qa);
        }
    }

    std::optional<Hit> nearest;

    for (int i = 0; i < count; ++i)
    {
        const auto t = roots[i];

        if (t < 0.0 || t > 1.0)
            continue;

        const auto point = a * ((1.0 - t) * (1.0 - t)) + c * (2.0 * t * (1.0 - t)) + b * (t * t);
        const auto distance = dot (direction, point - origin);
        const auto tangent = (c - a) * (1.0 - t) + (b - c) * t;

        if (distance <= epsilon || dot (tangent, tangent) < 1.0e-18 || (nearest && nearest->distance <= distance))
            continue;

        nearest = Hit { distance, normalised ({ -tangent.y, tangent.x }) };
    }

    return nearest;
}

std::optional<Hit> hit (const Segment& segment, Vec origin, Vec direction)
{
    switch (segment.shape)
    {
        case Shape::arc:   return hitArc (segment, origin, direction);
        case Shape::curve: return hitCurve (segment, origin, direction);
        case Shape::line:  break;
    }

    return hitLine (vec (segment.a), vec (segment.b), origin, direction);
}

bool finite (float v) { return std::isfinite (v) && std::abs (v) < 16.0f; }
} // namespace

Point Segment::bulge() const
{
    const auto middle = (a + b) * 0.5f;
    const auto d = b - a;
    return middle + Point { d.y, -d.x } * (0.5f * bend);
}

void Segment::sample (std::vector<Point>& points, int steps) const
{
    points.clear();

    if (shape == Shape::curve)
    {
        for (int i = 0; i <= steps; ++i)
        {
            const auto t = (float) i / (float) steps;
            points.push_back (a * ((1.0f - t) * (1.0f - t)) + c * (2.0f * t * (1.0f - t)) + b * (t * t));
        }

        return;
    }

    const auto circle = shape == Shape::arc && std::abs (bend) >= 1.0e-4f
                            ? circleThrough (vec (a), vec (b), vec (bulge())) : std::nullopt;

    if (! circle)
    {
        points.push_back (a);
        points.push_back (b);
        return;
    }

    // From a to b the way that passes the peak
    const auto [centre, radius] = *circle;
    const auto angleOf = [c = centre] (Point p) { return std::atan2 ((double) p.y - c.y, (double) p.x - c.x); };
    const auto wrap = [] (double angle) { return angle - juce::MathConstants<double>::twoPi * std::floor (angle / juce::MathConstants<double>::twoPi); };
    const auto start = angleOf (a);
    const auto sweep = wrap (angleOf (b) - start);
    const auto through = wrap (angleOf (bulge()) - start) < sweep ? sweep : sweep - juce::MathConstants<double>::twoPi;

    for (int i = 0; i <= steps; ++i)
    {
        const auto angle = start + through * i / steps;
        points.push_back ({ (float) (centre.x + std::cos (angle) * radius), (float) (centre.y + std::sin (angle) * radius) });
    }
}

float Segment::distanceTo (Point p) const
{
    std::vector<Point> points;
    sample (points, 64);
    auto nearest = std::numeric_limits<float>::max();

    for (size_t i = 1; i < points.size(); ++i)
    {
        const auto a0 = points[i - 1], d = points[i] - a0;
        const auto lengthSquared = d.x * d.x + d.y * d.y;
        const auto t = lengthSquared > 0.0f ? juce::jlimit (0.0f, 1.0f, ((p.x - a0.x) * d.x + (p.y - a0.y) * d.y) / lengthSquared) : 0.0f;
        nearest = juce::jmin (nearest, (p - (a0 + d * t)).length());
    }

    return nearest;
}

void Pattern::write (juce::OutputStream& out) const
{
    out.writeFloat (origin.x);
    out.writeFloat (origin.y);
    out.writeShort ((short) segments.size());

    for (const auto& s : segments)
    {
        out.writeByte ((char) s.shape);

        for (const auto v : { s.a.x, s.a.y, s.b.x, s.b.y, s.c.x, s.c.y, s.bend })
            out.writeFloat (v);
    }
}

bool Pattern::read (juce::InputStream& in)
{
    constexpr juce::int64 headerSize = 10, segmentSize = 29;

    if (in.getNumBytesRemaining() < headerSize)
        return false;

    Pattern pattern;
    pattern.origin = { in.readFloat(), in.readFloat() };
    const auto count = (int) (std::uint16_t) in.readShort();

    if (count > maxSegments || in.getNumBytesRemaining() < count * segmentSize
        || ! finite (pattern.origin.x) || ! finite (pattern.origin.y))
        return false;

    for (int i = 0; i < count; ++i)
    {
        Segment s;
        const auto shape = (int) in.readByte();
        float v[7];

        for (auto& value : v)
            value = in.readFloat();

        if (shape < 0 || shape > (int) Shape::curve || ! std::all_of (std::begin (v), std::end (v), finite))
            return false;

        s.shape = (Shape) shape;
        s.a = inside ({ v[0], v[1] });
        s.b = inside ({ v[2], v[3] });
        s.c = { v[4], v[5] };
        s.bend = v[6];
        pattern.segments.push_back (s);
    }

    pattern.origin = inside (pattern.origin, 0.98f);
    *this = std::move (pattern);
    return true;
}

Trace trace (const Pattern& pattern, float phase, std::vector<Point>* path)
{
    auto origin = vec (inside (pattern.origin, 0.98f));
    auto direction = vec (onCircle (phase));

    if (path != nullptr)
    {
        path->clear();
        path->push_back (pattern.origin);
    }

    for (int bounce = 0; bounce <= Trace::maxBounces; ++bounce)
    {
        // Where it leaves the circle, unless a segment is nearer
        const auto along = dot (origin, direction);
        auto nearest = -along + std::sqrt (juce::jmax (0.0, along * along - (dot (origin, origin) - 1.0)));
        std::optional<Vec> normal;

        for (const auto& segment : pattern.segments)
            if (const auto h = hit (segment, origin, direction); h && h->distance < nearest)
            {
                nearest = h->distance;
                normal = h->normal;
            }

        origin = origin + direction * nearest;

        if (path != nullptr)
            path->push_back ({ (float) origin.x, (float) origin.y });

        if (! normal)
            return { turnsOf ({ (float) origin.x, (float) origin.y }), true };

        direction = normalised (direction - *normal * (2.0 * dot (direction, *normal)));
    }

    return {};
}
} // namespace radar
