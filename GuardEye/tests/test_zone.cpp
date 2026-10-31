#include "check.h"
#include "zone.h"
using namespace guardeye;
int main() {
  std::vector<Pt> square = {{0.2f, 0.2f}, {0.8f, 0.2f}, {0.8f, 0.8f}, {0.2f, 0.8f}};
  CHECK(pointInPolygon(square, 0.5f, 0.5f));
  CHECK(!pointInPolygon(square, 0.1f, 0.5f));
  CHECK(!pointInPolygon(square, 0.5f, 0.9f));
  // concave "L" shape: the notch must be outside
  std::vector<Pt> L = {{0, 0}, {1, 0}, {1, 0.4f}, {0.4f, 0.4f}, {0.4f, 1}, {0, 1}};
  CHECK(pointInPolygon(L, 0.2f, 0.8f));
  CHECK(!pointInPolygon(L, 0.8f, 0.8f));
  CHECK(!pointInPolygon({}, 0.5f, 0.5f));            // no zone configured -> never inside
  CHECK(!pointInPolygon({{0, 0}, {1, 1}}, 0.5f, 0.5f));  // <3 points
  // bottom-centre rule: box whose FEET are inside counts, even if its head sticks out
  // box x 300..400, y 20..270: head (y=20 -> 0.06) is above the zone but feet (350,270)->(0.55,0.75) are inside
  CHECK(boxInZone(square, 300, 20, 100, 250, 640, 360));
  // box standing just behind the zone: feet (350,300)->(0.55,0.83) are outside
  CHECK(!boxInZone(square, 300, 100, 100, 200, 640, 360));
  PASS("zone");
  return 0;
}
