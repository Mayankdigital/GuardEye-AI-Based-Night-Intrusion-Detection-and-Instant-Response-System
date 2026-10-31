// Restricted-zone geometry. Polygon points are NORMALISED (0..1) so the
// config does not depend on the camera resolution.
#pragma once
#include <vector>

namespace guardeye {

struct Pt { float x; float y; };

// Ray-casting point-in-polygon test.
inline bool pointInPolygon(const std::vector<Pt>& poly, float x, float y) {
  if (poly.size() < 3) return false;
  bool inside = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const Pt& a = poly[i];
    const Pt& b = poly[j];
    bool crosses = ((a.y > y) != (b.y > y)) &&
                   (x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x);
    if (crosses) inside = !inside;
  }
  return inside;
}

// A person is "in the zone" when the bottom-centre of the box (≈ the feet)
// is inside the polygon. Box is in pixels; frame size converts to 0..1.
inline bool boxInZone(const std::vector<Pt>& poly, float bx, float by, float bw, float bh,
                      int frameW, int frameH) {
  float fx = (bx + bw * 0.5f) / static_cast<float>(frameW);
  float fy = (by + bh) / static_cast<float>(frameH);
  return pointInPolygon(poly, fx, fy);
}

}  // namespace guardeye
