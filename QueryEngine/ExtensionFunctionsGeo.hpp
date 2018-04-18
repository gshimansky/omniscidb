#define COMPRESSION_NONE 0
#define COMPRESSION_GEOINT32 1

DEVICE
double decompress_coord(int8_t* data, int32_t index, int32_t ic, bool x) {
  if (ic == COMPRESSION_GEOINT32) {
    auto compressed_coords = reinterpret_cast<int32_t*>(data);
    auto compressed_coord = compressed_coords[index];
    // decompress longitude: -2,147,483,647..2,147,483,647  --->  -180..180
    // decompress latitude: -2,147,483,647..2,147,483,647  --->  -90..90
    return (x ? 180.0 : 90.0) * (static_cast<double>(compressed_coord) / 2147483647.0);
  }
  auto double_coords = reinterpret_cast<double*>(data);
  return double_coords[index];
}

DEVICE ALWAYS_INLINE int32_t compression_unit_size(int32_t ic) {
  if (ic == COMPRESSION_GEOINT32) {
    return 4;
  }
  return 8;
}

// X coord accessor handling on-the-fly decommpression and transforms
DEVICE
double coord_x(int8_t* data, int32_t index, int32_t ic, int32_t isr, int32_t osr) {
  auto decompressed_coord_x = decompress_coord(data, index, ic, true);
  if (isr == 4326) {
    if (osr == 900913) {
      return conv_4326_900913_x(decompressed_coord_x);  // WGS 84 --> Web Mercator
    }
  }
  return decompressed_coord_x;
}

// Y coord accessor handling on-the-fly decommpression and transforms
DEVICE
double coord_y(int8_t* data, int32_t index, int32_t ic, int32_t isr, int32_t osr) {
  auto decompressed_coord_y = decompress_coord(data, index, ic, false);
  if (isr == 4326) {
    if (osr == 900913) {
      return conv_4326_900913_y(decompressed_coord_y);  // WGS 84 --> Web Mercator
    }
  }
  return decompressed_coord_y;
}

DEVICE
double hypotenuse(double x, double y) {
  x = fabs(x);
  y = fabs(y);
  if (x < y) {
    auto t = x;
    x = y;
    y = t;
  }
  if (y == 0.0)
    return x;
  return x * sqrt(1.0 + (y * y) / (x * x));
}

// Cartesian distance between points
DEVICE ALWAYS_INLINE double distance_point_point(double p1x, double p1y, double p2x, double p2y) {
  return hypotenuse(p1x - p2x, p1y - p2y);
}

// Cartesian distance between a point and a line segment
DEVICE
double distance_point_line(double px, double py, double l1x, double l1y, double l2x, double l2y) {
  double length = distance_point_point(l1x, l1y, l2x, l2y);
  if (length == 0.0)
    return distance_point_point(px, py, l1x, l1y);

  // Find projection of point P onto the line segment AB:
  // Line containing that segment: A + k * (B - A)
  // Projection of point P onto the line touches it at
  //   k = dot(P-A,B-A) / length^2
  // AB segment is represented by k = [0,1]
  // Clamping k to [0,1] will give the shortest distance from P to AB segment
  double dotprod = (px - l1x) * (l2x - l1x) + (py - l1y) * (l2y - l1y);
  double k = dotprod / (length * length);
  k = fmax(0.0, fmin(1.0, k));
  double projx = l1x + k * (l2x - l1x);
  double projy = l1y + k * (l2y - l1y);
  return distance_point_point(px, py, projx, projy);
}

// Given three colinear points p, q, r, the function checks if
// point q lies on line segment 'pr'
DEVICE ALWAYS_INLINE bool on_segment(double px, double py, double qx, double qy, double rx, double ry) {
  return (qx <= fmax(px, rx) && qx >= fmin(px, rx) && qy <= fmax(py, ry) && qy >= fmin(py, ry));
}

DEVICE ALWAYS_INLINE int16_t orientation(double px, double py, double qx, double qy, double rx, double ry) {
  auto val = ((qy - py) * (rx - qx) - (qx - px) * (ry - qy));
  if (val == 0.0)
    return 0;  // Points p, q and r are colinear
  if (val > 0.0)
    return 1;  // Clockwise point orientation
  return 2;    // Counterclockwise point orientation
}

// Cartesian intersection of two line segments l11-l12 and l21-l22
DEVICE
bool intersects_line_line(double l11x,
                          double l11y,
                          double l12x,
                          double l12y,
                          double l21x,
                          double l21y,
                          double l22x,
                          double l22y) {
  auto o1 = orientation(l11x, l11y, l12x, l12y, l21x, l21y);
  auto o2 = orientation(l11x, l11y, l12x, l12y, l22x, l22y);
  auto o3 = orientation(l21x, l21y, l22x, l22y, l11x, l11y);
  auto o4 = orientation(l21x, l21y, l22x, l22y, l12x, l12y);

  // General case
  if (o1 != o2 && o3 != o4)
    return true;

  // Special Cases
  // l11, l12 and l21 are colinear and l21 lies on segment l11-l12
  if (o1 == 0 && on_segment(l11x, l11y, l21x, l21y, l12x, l12y))
    return true;

  // l11, l12 and l21 are colinear and l22 lies on segment l11-l12
  if (o2 == 0 && on_segment(l11x, l11y, l22x, l22y, l12x, l12y))
    return true;

  // l21, l22 and l11 are colinear and l11 lies on segment l21-l22
  if (o3 == 0 && on_segment(l21x, l21y, l11x, l11y, l22x, l22y))
    return true;

  // l21, l22 and l12 are colinear and l12 lies on segment l21-l22
  if (o4 == 0 && on_segment(l21x, l21y, l12x, l12y, l22x, l22y))
    return true;

  return false;
}

// Cartesian distance between two line segments l11-l12 and l21-l22
DEVICE
double distance_line_line(double l11x,
                          double l11y,
                          double l12x,
                          double l12y,
                          double l21x,
                          double l21y,
                          double l22x,
                          double l22y) {
  if (intersects_line_line(l11x, l11y, l12x, l12y, l21x, l21y, l22x, l22y))
    return 0.0;
  double dist12 = fmin(distance_point_line(l11x, l11y, l21x, l21y, l22x, l22y),
                       distance_point_line(l12x, l12y, l21x, l21y, l22x, l22y));
  double dist21 = fmin(distance_point_line(l21x, l21y, l11x, l11y, l12x, l12y),
                       distance_point_line(l22x, l22y, l11x, l11y, l12x, l12y));
  return fmin(dist12, dist21);
}

// Checks if a simple polygon (no holes) contains a point.
// Poly coords are extracted from raw data, based on compression (ic1) and input/output SRIDs (isr1/osr).
DEVICE
bool contains_polygon_point(int8_t* poly,
                            int32_t poly_num_coords,
                            double px,
                            double py,
                            int32_t ic1,
                            int32_t isr1,
                            int32_t osr) {
  // Shoot a line from point P to the right along x axis, register intersections with any of polygon's edges.
  // Each intersection means we're entered/exited the polygon.
  // Odd number of intersections means the polygon does contain P.
  bool result = false;
  int64_t i, j;
  for (i = 0, j = poly_num_coords - 2; i < poly_num_coords; j = i, i += 2) {
    double e1x = coord_x(poly, j, ic1, isr1, osr);
    double e1y = coord_y(poly, j + 1, ic1, isr1, osr);
    double e2x = coord_x(poly, i, ic1, isr1, osr);
    double e2y = coord_y(poly, i + 1, ic1, isr1, osr);
    double xray = fmax(e2x, e1x);
    if (xray < px)
      continue;                   // No intersection - edge is to the left of point, we're casting the ray to the right
    if (intersects_line_line(px,  // ray shooting from point p to the right
                             py,
                             xray + 1.0,  // overshoot the ray a little bit to detect intersection if there is one
                             py,
                             e1x,  // polygon edge
                             e1y,
                             e2x,
                             e2y)) {
      result = !result;
      if (distance_point_line(e2x, e2y, px, py, xray + 1.0, py) == 0.0) {
        // If ray goes through the edge's second vertex, flip the result again -
        // that vertex will be crossed again when we look at the next edge
        // TODO: sensitivity: how close should the ray be to the vertex be to register a crossing
        result = !result;
      }
    }
  }
  return result;
}

DEVICE
bool contains_polygon_linestring(int8_t* poly,
                                 int32_t poly_num_coords,
                                 int8_t* l,
                                 int64_t lnum_coords,
                                 int32_t ic1,
                                 int32_t isr1,
                                 int32_t ic2,
                                 int32_t isr2,
                                 int32_t osr) {
  // Check if each of the linestring vertices are inside the polygon.
  for (int32_t i = 0; i < lnum_coords; i += 2) {
    double lx = coord_x(l, i, ic2, isr2, osr);
    double ly = coord_y(l, i + 1, ic2, isr2, osr);
    if (!contains_polygon_point(poly, poly_num_coords, lx, ly, ic1, isr1, osr))
      return false;
  }
  return true;
}

EXTENSION_NOINLINE
double ST_X_Point(int8_t* p, int64_t psize, int32_t ic, int32_t isr, int32_t osr) {
  return coord_x(p, 0, ic, isr, osr);
}

EXTENSION_NOINLINE
double ST_Y_Point(int8_t* p, int64_t psize, int32_t ic, int32_t isr, int32_t osr) {
  return coord_y(p, 1, ic, isr, osr);
}

EXTENSION_NOINLINE
double ST_X_LineString(int8_t* l, int64_t lsize, int32_t lindex, int32_t ic, int32_t isr, int32_t osr) {
  auto l_num_points = lsize / (2 * compression_unit_size(ic));
  if (lindex < 0 || lindex > l_num_points)
    lindex = l_num_points;  // Endpoint
  return coord_x(l, 2 * (lindex - 1), ic, isr, osr);
}

EXTENSION_NOINLINE
double ST_Y_LineString(int8_t* l, int64_t lsize, int32_t lindex, int32_t ic, int32_t isr, int32_t osr) {
  auto l_num_points = lsize / (2 * compression_unit_size(ic));
  if (lindex < 0 || lindex > l_num_points)
    lindex = l_num_points;  // Endpoint
  return coord_y(l, 2 * (lindex - 1) + 1, ic, isr, osr);
}

EXTENSION_NOINLINE
double ST_XMin(int8_t* coords, int64_t size, int32_t ic, int32_t isr, int32_t osr) {
  auto num_coords = size / compression_unit_size(ic);
  double xmin = 0.0;
  for (int32_t i = 0; i < num_coords; i += 2) {
    double x = coord_x(coords, i, ic, isr, osr);
    if (i == 0 || x < xmin)
      xmin = x;
  }
  return xmin;
}

EXTENSION_NOINLINE
double ST_YMin(int8_t* coords, int64_t size, int32_t ic, int32_t isr, int32_t osr) {
  auto num_coords = size / compression_unit_size(ic);
  double ymin = 0.0;
  for (int32_t i = 1; i < num_coords; i += 2) {
    double y = coord_y(coords, i, ic, isr, osr);
    if (i == 1 || y < ymin)
      ymin = y;
  }
  return ymin;
}

EXTENSION_NOINLINE
double ST_XMax(int8_t* coords, int64_t size, int32_t ic, int32_t isr, int32_t osr) {
  auto num_coords = size / compression_unit_size(ic);
  double xmax = 0.0;
  for (int32_t i = 0; i < num_coords; i += 2) {
    double x = coord_x(coords, i, ic, isr, osr);
    if (i == 0 || x > xmax)
      xmax = x;
  }
  return xmax;
}

EXTENSION_NOINLINE
double ST_YMax(int8_t* coords, int64_t size, int32_t ic, int32_t isr, int32_t osr) {
  auto num_coords = size / compression_unit_size(ic);
  double ymax = 0.0;
  for (int32_t i = 1; i < num_coords; i += 2) {
    double y = coord_y(coords, i, ic, isr, osr);
    if (i == 1 || y > ymax)
      ymax = y;
  }
  return ymax;
}

EXTENSION_NOINLINE
double ST_Distance_Point_Point(int8_t* p1,
                               int64_t p1size,
                               int8_t* p2,
                               int64_t p2size,
                               int32_t ic1,
                               int32_t isr1,
                               int32_t ic2,
                               int32_t isr2,
                               int32_t osr) {
  double p1x = coord_x(p1, 0, ic1, isr1, osr);
  double p1y = coord_y(p1, 1, ic1, isr1, osr);
  double p2x = coord_x(p2, 0, ic2, isr2, osr);
  double p2y = coord_y(p2, 1, ic2, isr2, osr);
  return distance_point_point(p1x, p1y, p2x, p2y);
}

EXTENSION_NOINLINE
double ST_Distance_Point_Point_Geodesic(int8_t* p1,
                                        int64_t p1size,
                                        int8_t* p2,
                                        int64_t p2size,
                                        int32_t ic1,
                                        int32_t isr1,
                                        int32_t ic2,
                                        int32_t isr2,
                                        int32_t osr) {
  double p1x = coord_x(p1, 0, ic1, 4326, 4326);
  double p1y = coord_y(p1, 1, ic1, 4326, 4326);
  double p2x = coord_x(p2, 0, ic2, 4326, 4326);
  double p2y = coord_y(p2, 1, ic2, 4326, 4326);
  return distance_in_meters(p1x, p1y, p2x, p2y);
}

EXTENSION_NOINLINE
double ST_Distance_Point_LineString_Geodesic(int8_t* p,
                                             int64_t psize,
                                             int8_t* l,
                                             int64_t lsize,
                                             int32_t lindex,
                                             int32_t ic1,
                                             int32_t isr1,
                                             int32_t ic2,
                                             int32_t isr2,
                                             int32_t osr) {
  // Currently only indexed LineString is supported
  double px = coord_x(p, 0, ic1, 4326, 4326);
  double py = coord_y(p, 1, ic1, 4326, 4326);
  auto lpoints = lsize / (2 * compression_unit_size(ic2));
  if (lindex < 0 || lindex > lpoints)
    lindex = lpoints;  // Endpoint
  double lx = coord_x(l, 2 * (lindex - 1), ic2, 4326, 4326);
  double ly = coord_y(l, 2 * (lindex - 1) + 1, ic2, 4326, 4326);
  return distance_in_meters(px, py, lx, ly);
}

EXTENSION_INLINE
double ST_Distance_LineString_Point_Geodesic(int8_t* l,
                                             int64_t lsize,
                                             int32_t lindex,
                                             int8_t* p,
                                             int64_t psize,
                                             int32_t ic1,
                                             int32_t isr1,
                                             int32_t ic2,
                                             int32_t isr2,
                                             int32_t osr) {
  // Currently only indexed LineString is supported
  return ST_Distance_Point_LineString_Geodesic(p, psize, l, lsize, lindex, ic2, isr2, ic1, isr1, osr);
}

EXTENSION_NOINLINE
double ST_Distance_LineString_LineString_Geodesic(int8_t* l1,
                                                  int64_t l1size,
                                                  int32_t l1index,
                                                  int8_t* l2,
                                                  int64_t l2size,
                                                  int32_t l2index,
                                                  int32_t ic1,
                                                  int32_t isr1,
                                                  int32_t ic2,
                                                  int32_t isr2,
                                                  int32_t osr) {
  // Currently only indexed LineStrings are supported
  auto l1points = l1size / (2 * compression_unit_size(ic1));
  if (l1index < 0 || l1index > l1points)
    l1index = l1points;  // Endpoint
  double l1x = coord_x(l1, 2 * (l1index - 1), ic1, 4326, 4326);
  double l1y = coord_y(l1, 2 * (l1index - 1) + 1, ic1, 4326, 4326);
  auto l2points = l2size / (2 * compression_unit_size(ic2));
  if (l2index < 0 || l2index > l2points)
    l2index = l2points;  // Endpoint
  double l2x = coord_x(l2, 2 * (l2index - 1), ic2, 4326, 4326);
  double l2y = coord_y(l2, 2 * (l2index - 1) + 1, ic2, 4326, 4326);
  return distance_in_meters(l1x, l1y, l2x, l2y);
}

EXTENSION_NOINLINE
double ST_Distance_Point_LineString(int8_t* p,
                                    int64_t psize,
                                    int8_t* l,
                                    int64_t lsize,
                                    int32_t lindex,
                                    int32_t ic1,
                                    int32_t isr1,
                                    int32_t ic2,
                                    int32_t isr2,
                                    int32_t osr) {
  double px = coord_x(p, 0, ic1, isr1, osr);
  double py = coord_y(p, 1, ic1, isr1, osr);

  auto l_num_coords = lsize / compression_unit_size(ic2);
  auto l_num_points = l_num_coords / 2;
  if (lindex != 0) {  // Indexed linestring
    if (lindex < 0 || lindex > l_num_points)
      lindex = l_num_points;  // Endpoint
    double lx = coord_x(l, 2 * (lindex - 1), ic2, isr2, osr);
    double ly = coord_y(l, 2 * (lindex - 1) + 1, ic2, isr2, osr);
    return distance_point_point(px, py, lx, ly);
  }

  double l1x = coord_x(l, 0, ic2, isr2, osr);
  double l1y = coord_y(l, 1, ic2, isr2, osr);
  double l2x = coord_x(l, 2, ic2, isr2, osr);
  double l2y = coord_y(l, 3, ic2, isr2, osr);

  double dist = distance_point_line(px, py, l1x, l1y, l2x, l2y);
  for (int32_t i = 4; i < l_num_coords; i += 2) {
    l1x = l2x;  // advance one point
    l1y = l2y;
    l2x = coord_x(l, i, ic2, isr2, osr);
    l2y = coord_y(l, i + 1, ic2, isr2, osr);
    double ldist = distance_point_line(px, py, l1x, l1y, l2x, l2y);
    if (dist > ldist)
      dist = ldist;
  }
  return dist;
}

EXTENSION_NOINLINE
double ST_Distance_Point_Polygon(int8_t* p,
                                 int64_t psize,
                                 int8_t* poly,
                                 int64_t polysize,
                                 int32_t* poly_ring_sizes,
                                 int64_t poly_num_rings,
                                 int32_t ic1,
                                 int32_t isr1,
                                 int32_t ic2,
                                 int32_t isr2,
                                 int32_t osr) {
  auto exterior_ring_num_coords = polysize / compression_unit_size(ic2);
  if (poly_num_rings > 0)
    exterior_ring_num_coords = poly_ring_sizes[0] * 2;
  auto exterior_ring_coords_size = exterior_ring_num_coords * compression_unit_size(ic2);

  double px = coord_x(p, 0, ic1, isr1, osr);
  double py = coord_y(p, 1, ic1, isr1, osr);
  if (!contains_polygon_point(poly, exterior_ring_num_coords, px, py, ic2, isr2, osr)) {
    // Outside the exterior ring
    return ST_Distance_Point_LineString(p, psize, poly, exterior_ring_coords_size, 0, ic1, isr1, ic2, isr2, osr);
  }
  // Inside exterior ring
  // Advance to first interior ring
  poly += exterior_ring_coords_size;
  // Check if one of the polygon's holes contains that point
  for (auto r = 1; r < poly_num_rings; r++) {
    auto interior_ring_num_coords = poly_ring_sizes[r] * 2;
    auto interior_ring_coords_size = interior_ring_num_coords * compression_unit_size(ic2);
    if (contains_polygon_point(poly, interior_ring_num_coords, px, py, ic2, isr2, osr)) {
      // Inside an interior ring
      return ST_Distance_Point_LineString(p, psize, poly, interior_ring_coords_size, 0, ic1, isr1, ic2, isr2, osr);
    }
    poly += interior_ring_coords_size;
  }
  return 0.0;
}

EXTENSION_NOINLINE
double ST_Distance_Point_MultiPolygon(int8_t* p,
                                      int64_t psize,
                                      int8_t* mpoly_coords,
                                      int64_t mpoly_coords_size,
                                      int32_t* mpoly_ring_sizes,
                                      int64_t mpoly_num_rings,
                                      int32_t* mpoly_poly_sizes,
                                      int64_t mpoly_num_polys,
                                      int32_t ic1,
                                      int32_t isr1,
                                      int32_t ic2,
                                      int32_t isr2,
                                      int32_t osr) {
  if (mpoly_num_polys <= 0)
    return 0.0;
  double min_distance = 0.0;

  // Set specific poly pointers as we move through the coords/ringsizes/polyrings arrays.
  auto next_poly_coords = mpoly_coords;
  auto next_poly_ring_sizes = mpoly_ring_sizes;

  for (auto poly = 0; poly < mpoly_num_polys; poly++) {
    auto poly_coords = next_poly_coords;
    auto poly_ring_sizes = next_poly_ring_sizes;
    auto poly_num_rings = mpoly_poly_sizes[poly];
    // Count number of coords in all of poly's rings, advance ring size pointer.
    int32_t poly_num_coords = 0;
    for (auto ring = 0; ring < poly_num_rings; ring++) {
      poly_num_coords += 2 * *next_poly_ring_sizes++;
    }
    auto poly_coords_size = poly_num_coords * compression_unit_size(ic1);
    next_poly_coords += poly_coords_size;
    double distance = ST_Distance_Point_Polygon(
        p, psize, poly_coords, poly_coords_size, poly_ring_sizes, poly_num_rings, ic1, isr1, ic2, isr2, osr);
    if (poly == 0 || min_distance > distance) {
      min_distance = distance;
      if (min_distance == 0.0)
        break;
    }
  }

  return min_distance;
}

EXTENSION_INLINE
double ST_Distance_LineString_Point(int8_t* l,
                                    int64_t lsize,
                                    int32_t lindex,
                                    int8_t* p,
                                    int64_t psize,
                                    int32_t ic1,
                                    int32_t isr1,
                                    int32_t ic2,
                                    int32_t isr2,
                                    int32_t osr) {
  return ST_Distance_Point_LineString(p, psize, l, lsize, lindex, ic2, isr2, ic1, isr1, osr);
}

EXTENSION_NOINLINE
double ST_Distance_LineString_LineString(int8_t* l1,
                                         int64_t l1size,
                                         int32_t l1index,
                                         int8_t* l2,
                                         int64_t l2size,
                                         int32_t l2index,
                                         int32_t ic1,
                                         int32_t isr1,
                                         int32_t ic2,
                                         int32_t isr2,
                                         int32_t osr) {
  auto l1_num_coords = l1size / compression_unit_size(ic1);
  auto l1_num_points = l1_num_coords / 2;
  auto l2_num_coords = l2size / compression_unit_size(ic2);
  auto l2_num_points = l2_num_coords / 2;

  if (l1index != 0 && l2index != 0) {  // Indexed linestrings
    // TODO: distance between a linestring and an indexed linestring, i.e. point
    if (l1index < 0 || l1index > l1_num_points)
      l1index = l1_num_points;
    double l1x = coord_x(l1, 2 * (l1index - 1), ic1, isr1, osr);
    double l1y = coord_y(l1, 2 * (l1index - 1) + 1, ic1, isr1, osr);
    if (l2index < 0 || l2index > l2_num_points)
      l2index = l2_num_points;
    double l2x = coord_x(l2, 2 * (l2index - 1), ic2, isr2, osr);
    double l2y = coord_y(l2, 2 * (l2index - 1) + 1, ic2, isr2, osr);
    return distance_point_point(l1x, l1y, l2x, l2y);
  }

  double dist = 0.0;
  double l11x = coord_x(l1, 0, ic1, isr1, osr);
  double l11y = coord_y(l1, 1, ic1, isr1, osr);
  for (int32_t i1 = 2; i1 < l1_num_coords; i1 += 2) {
    double l12x = coord_x(l1, i1, ic1, isr1, osr);
    double l12y = coord_y(l1, i1 + 1, ic1, isr1, osr);

    double l21x = coord_x(l2, 0, ic2, isr2, osr);
    double l21y = coord_y(l2, 1, ic2, isr2, osr);
    for (int32_t i2 = 2; i2 < l2_num_coords; i2 += 2) {
      double l22x = coord_x(l2, i2, ic2, isr2, osr);
      double l22y = coord_y(l2, i2 + 1, ic2, isr2, osr);

      double ldist = distance_line_line(l11x, l11y, l12x, l12y, l21x, l21y, l22x, l22y);
      if (i1 == 2 && i2 == 2)
        dist = ldist;  // initialize dist with distance between the first two segments
      else if (dist > ldist)
        dist = ldist;
      if (dist == 0.0)
        return 0.0;  // segments touch

      l21x = l22x;  // advance to the next point on l2
      l21y = l22y;
    }

    l11x = l12x;  // advance to the next point on l1
    l11y = l12y;
  }
  return dist;
}

EXTENSION_NOINLINE
double ST_Distance_LineString_Polygon(int8_t* l,
                                      int64_t lsize,
                                      int32_t lindex,
                                      int8_t* poly_coords,
                                      int64_t poly_coords_size,
                                      int32_t* poly_ring_sizes,
                                      int64_t poly_num_rings,
                                      int32_t ic1,
                                      int32_t isr1,
                                      int32_t ic2,
                                      int32_t isr2,
                                      int32_t osr) {
  // TODO: revisit implementation, cover all cases

  if (lindex > 0) {
    // Indexed linestring
    auto p = l + lindex * compression_unit_size(ic1);
    auto psize = 2 * compression_unit_size(ic2);
    return ST_Distance_Point_Polygon(
        p, psize, poly_coords, poly_coords_size, poly_ring_sizes, poly_num_rings, ic1, isr1, ic2, isr2, osr);
  }

  auto exterior_ring_num_coords = poly_coords_size / compression_unit_size(ic2);
  if (poly_num_rings > 0)
    exterior_ring_num_coords = poly_ring_sizes[0] * 2;
  auto exterior_ring_coords_size = exterior_ring_num_coords * compression_unit_size(ic2);

  auto l_num_coords = lsize / compression_unit_size(ic1);
  auto poly = poly_coords;
  if (!contains_polygon_linestring(poly, exterior_ring_num_coords, l, l_num_coords, ic2, isr2, ic1, isr1, osr)) {
    // Linestring is outside poly's exterior ring
    return ST_Distance_LineString_LineString(
        poly, exterior_ring_coords_size, 0, l, lsize, 0, ic2, isr2, ic1, isr1, osr);
  }

  // Linestring is inside poly's exterior ring
  poly += exterior_ring_coords_size;
  // Check if one of the polygon's holes contains that linestring
  for (auto r = 1; r < poly_num_rings; r++) {
    int64_t interior_ring_num_coords = poly_ring_sizes[r] * 2;
    if (contains_polygon_linestring(poly, interior_ring_num_coords, l, l_num_coords, ic2, isr2, ic1, isr1, osr)) {
      // Inside an interior ring
      auto interior_ring_coords_size = interior_ring_num_coords * compression_unit_size(ic2);
      return ST_Distance_LineString_LineString(
          poly, interior_ring_coords_size, 0, l, lsize, 0, ic2, isr2, ic1, isr1, osr);
    }
    poly += interior_ring_num_coords * compression_unit_size(ic2);
  }

  return 0.0;
}

EXTENSION_INLINE
double ST_Distance_Polygon_Point(int8_t* poly_coords,
                                 int64_t poly_coords_size,
                                 int32_t* poly_ring_sizes,
                                 int64_t poly_num_rings,
                                 int8_t* p,
                                 int64_t psize,
                                 int32_t ic1,
                                 int32_t isr1,
                                 int32_t ic2,
                                 int32_t isr2,
                                 int32_t osr) {
  return ST_Distance_Point_Polygon(
      p, psize, poly_coords, poly_coords_size, poly_ring_sizes, poly_num_rings, ic2, isr2, ic1, isr1, osr);
}

EXTENSION_INLINE
double ST_Distance_Polygon_LineString(int8_t* poly_coords,
                                      int64_t poly_coords_size,
                                      int32_t* poly_ring_sizes,
                                      int64_t poly_num_rings,
                                      int8_t* l,
                                      int64_t lsize,
                                      int32_t li,
                                      int32_t ic1,
                                      int32_t isr1,
                                      int32_t ic2,
                                      int32_t isr2,
                                      int32_t osr) {
  return ST_Distance_LineString_Polygon(
      l, lsize, li, poly_coords, poly_coords_size, poly_ring_sizes, poly_num_rings, ic2, isr2, ic1, isr2, osr);
}

EXTENSION_NOINLINE
double ST_Distance_Polygon_Polygon(int8_t* poly1_coords,
                                   int64_t poly1_coords_size,
                                   int32_t* poly1_ring_sizes,
                                   int64_t poly1_num_rings,
                                   int8_t* poly2_coords,
                                   int64_t poly2_coords_size,
                                   int32_t* poly2_ring_sizes,
                                   int64_t poly2_num_rings,
                                   int32_t ic1,
                                   int32_t isr1,
                                   int32_t ic2,
                                   int32_t isr2,
                                   int32_t osr) {
  // TODO: revisit implementation

  auto poly1_exterior_ring_num_coords = poly1_coords_size / compression_unit_size(ic1);
  if (poly1_num_rings > 0)
    poly1_exterior_ring_num_coords = poly1_ring_sizes[0] * 2;
  auto poly1_exterior_ring_coords_size = poly1_exterior_ring_num_coords * compression_unit_size(ic1);

  auto poly2_exterior_ring_num_coords = poly2_coords_size / compression_unit_size(ic2);
  if (poly2_num_rings > 0)
    poly2_exterior_ring_num_coords = poly2_ring_sizes[0] * 2;
  auto poly2_exterior_ring_coords_size = poly2_exterior_ring_num_coords * compression_unit_size(ic2);

  // check if poly2 is inside poly1 exterior ring and outside poly1 holes
  auto poly1 = poly1_coords;
  if (contains_polygon_linestring(poly1,
                                  poly1_exterior_ring_num_coords,
                                  poly2_coords,
                                  poly2_exterior_ring_num_coords,
                                  ic1,
                                  isr1,
                                  ic2,
                                  isr2,
                                  osr)) {
    // poly1 exterior ring contains poly2 exterior ring
    poly1 += poly1_exterior_ring_num_coords * compression_unit_size(ic1);
    // Check if one of the poly1's holes contains that poly2 exterior ring
    for (auto r = 1; r < poly1_num_rings; r++) {
      int64_t poly1_interior_ring_num_coords = poly1_ring_sizes[r] * 2;
      if (contains_polygon_linestring(poly1,
                                      poly1_interior_ring_num_coords,
                                      poly2_coords,
                                      poly2_exterior_ring_num_coords,
                                      ic1,
                                      isr1,
                                      ic2,
                                      isr2,
                                      osr)) {
        // Inside an interior ring - measure the distance of poly2 exterior to that hole's border
        auto poly1_interior_ring_coords_size = poly1_interior_ring_num_coords * compression_unit_size(ic1);
        return ST_Distance_LineString_LineString(poly1,
                                                 poly1_interior_ring_coords_size,
                                                 0,
                                                 poly2_coords,
                                                 poly2_exterior_ring_coords_size,
                                                 0,
                                                 ic1,
                                                 isr1,
                                                 ic2,
                                                 isr2,
                                                 osr);
      }
      poly1 += poly1_interior_ring_num_coords * compression_unit_size(ic1);
      ;
    }
    return 0.0;
  }

  // check if poly1 is inside poly2 exterior ring and outside poly2 holes
  auto poly2 = poly2_coords;
  if (contains_polygon_linestring(poly2,
                                  poly2_exterior_ring_num_coords,
                                  poly1_coords,
                                  poly1_exterior_ring_num_coords,
                                  ic2,
                                  isr2,
                                  ic1,
                                  isr1,
                                  osr)) {
    // poly2 exterior ring contains poly1 exterior ring
    poly2 += poly2_exterior_ring_num_coords * compression_unit_size(ic2);
    // Check if one of the poly2's holes contains that poly1 exterior ring
    for (auto r = 1; r < poly2_num_rings; r++) {
      int64_t poly2_interior_ring_num_coords = poly2_ring_sizes[r] * 2;
      if (contains_polygon_linestring(poly2,
                                      poly2_interior_ring_num_coords,
                                      poly1_coords,
                                      poly1_exterior_ring_num_coords,
                                      ic2,
                                      isr2,
                                      ic1,
                                      isr1,
                                      osr)) {
        // Inside an interior ring - measure the distance of poly1 exterior to that hole's border
        auto poly2_interior_ring_coords_size = poly2_interior_ring_num_coords * compression_unit_size(ic2);
        return ST_Distance_LineString_LineString(poly2,
                                                 poly2_interior_ring_coords_size,
                                                 0,
                                                 poly1_coords,
                                                 poly1_exterior_ring_coords_size,
                                                 0,
                                                 ic2,
                                                 isr2,
                                                 ic1,
                                                 isr1,
                                                 osr);
      }
      poly2 += poly2_interior_ring_num_coords * compression_unit_size(ic2);
      ;
    }
    return 0.0;
  }

  // poly1 does not properly contain poly2, poly2 does not properly contain poly1
  // Assuming disjoint or intersecting shapes: return distance between exterior rings.
  return ST_Distance_LineString_LineString(poly1_coords,
                                           poly1_exterior_ring_coords_size,
                                           0,
                                           poly2_coords,
                                           poly2_exterior_ring_coords_size,
                                           0,
                                           ic1,
                                           isr1,
                                           ic2,
                                           isr2,
                                           osr);
}

EXTENSION_INLINE
double ST_Distance_MultiPolygon_Point(int8_t* mpoly_coords,
                                      int64_t mpoly_coords_size,
                                      int32_t* mpoly_ring_sizes,
                                      int64_t mpoly_num_rings,
                                      int32_t* mpoly_poly_sizes,
                                      int64_t mpoly_num_polys,
                                      int8_t* p,
                                      int64_t psize,
                                      int32_t ic1,
                                      int32_t isr1,
                                      int32_t ic2,
                                      int32_t isr2,
                                      int32_t osr) {
  return ST_Distance_Point_MultiPolygon(p,
                                        psize,
                                        mpoly_coords,
                                        mpoly_coords_size,
                                        mpoly_ring_sizes,
                                        mpoly_num_rings,
                                        mpoly_poly_sizes,
                                        mpoly_num_polys,
                                        ic2,
                                        isr2,
                                        ic1,
                                        isr1,
                                        osr);
}

EXTENSION_NOINLINE
bool ST_Contains_Point_Point(int8_t* p1,
                             int64_t p1size,
                             int8_t* p2,
                             int64_t p2size,
                             int32_t ic1,
                             int32_t isr1,
                             int32_t ic2,
                             int32_t isr2,
                             int32_t osr) {
  double p1x = coord_x(p1, 0, ic1, isr1, osr);
  double p1y = coord_y(p1, 1, ic1, isr1, osr);
  double p2x = coord_x(p2, 0, ic2, isr2, osr);
  double p2y = coord_y(p2, 1, ic2, isr2, osr);
  return (p1x == p2x) && (p1y == p2y);  // TODO: precision sensitivity
}

EXTENSION_NOINLINE
bool ST_Contains_Point_LineString(int8_t* p,
                                  int64_t psize,
                                  int8_t* l,
                                  int64_t lsize,
                                  int32_t li,
                                  int32_t ic1,
                                  int32_t isr1,
                                  int32_t ic2,
                                  int32_t isr2,
                                  int32_t osr) {
  double px = coord_x(p, 0, ic1, isr1, osr);
  double py = coord_y(p, 1, ic1, isr1, osr);
  auto l_num_coords = lsize / compression_unit_size(ic2);
  for (int i = 0; i < l_num_coords; i += 2) {
    double lx = coord_x(l, i, ic2, isr2, osr);
    double ly = coord_y(l, i + 1, ic2, isr2, osr);
    if (px == lx && py == ly)  // TODO: precision sensitivity
      continue;
    return false;
  }
  return true;
}

EXTENSION_NOINLINE
bool ST_Contains_Point_Polygon(int8_t* p,
                               int64_t psize,
                               int8_t* poly_coords,
                               int64_t poly_coords_size,
                               int32_t* poly_ring_sizes,
                               int64_t poly_num_rings,
                               int32_t ic1,
                               int32_t isr1,
                               int32_t ic2,
                               int32_t isr2,
                               int32_t osr) {
  auto exterior_ring_num_coords = poly_coords_size / compression_unit_size(ic2);
  if (poly_num_rings > 0)
    exterior_ring_num_coords = poly_ring_sizes[0] * 2;
  auto exterior_ring_coords_size = exterior_ring_num_coords * compression_unit_size(ic2);

  return ST_Contains_Point_LineString(p, psize, poly_coords, exterior_ring_coords_size, 0, ic1, isr1, ic2, isr2, osr);
}

EXTENSION_INLINE
bool ST_Contains_LineString_Point(int8_t* l,
                                  int64_t lsize,
                                  int32_t li,
                                  int8_t* p,
                                  int64_t psize,
                                  int32_t ic1,
                                  int32_t isr1,
                                  int32_t ic2,
                                  int32_t isr2,
                                  int32_t osr) {
  // TODO: precision sensitivity
  return (ST_Distance_Point_LineString(p, psize, l, lsize, li, ic2, isr2, ic1, isr1, osr) == 0.0);
}

EXTENSION_NOINLINE
bool ST_Contains_LineString_LineString(int8_t* l1,
                                       int64_t l1size,
                                       int32_t l1i,
                                       int8_t* l2,
                                       int64_t l2size,
                                       int32_t l2i,
                                       int32_t ic1,
                                       int32_t isr1,
                                       int32_t ic2,
                                       int32_t isr2,
                                       int32_t osr) {
  return false;
}

EXTENSION_NOINLINE
bool ST_Contains_LineString_Polygon(int8_t* l,
                                    int64_t lsize,
                                    int32_t li,
                                    int8_t* poly_coords,
                                    int64_t poly_coords_size,
                                    int32_t* poly_ring_sizes,
                                    int64_t poly_num_rings,
                                    int32_t ic1,
                                    int32_t isr1,
                                    int32_t ic2,
                                    int32_t isr2,
                                    int32_t osr) {
  return false;
}

EXTENSION_NOINLINE
bool ST_Contains_Polygon_Point(int8_t* poly_coords,
                               int64_t poly_coords_size,
                               int32_t* poly_ring_sizes,
                               int64_t poly_num_rings,
                               int8_t* p,
                               int64_t psize,
                               int32_t ic1,
                               int32_t isr1,
                               int32_t ic2,
                               int32_t isr2,
                               int32_t osr) {
  auto poly_num_coords = poly_coords_size / compression_unit_size(ic1);
  auto exterior_ring_num_coords = poly_num_coords;
  if (poly_num_rings > 0)
    exterior_ring_num_coords = poly_ring_sizes[0] * 2;

  auto poly = poly_coords;
  double px = coord_x(p, 0, ic2, isr2, osr);
  double py = coord_y(p, 1, ic2, isr2, osr);
  if (contains_polygon_point(poly, exterior_ring_num_coords, px, py, ic1, isr1, osr)) {
    // Inside exterior ring
    poly += exterior_ring_num_coords * compression_unit_size(ic1);
    // Check that none of the polygon's holes contain that point
    for (auto r = 1; r < poly_num_rings; r++) {
      int64_t interior_ring_num_coords = poly_ring_sizes[r] * 2;
      if (contains_polygon_point(poly, interior_ring_num_coords, px, py, ic1, isr1, osr))
        return false;
      poly += interior_ring_num_coords * compression_unit_size(ic1);
    }
    return true;
  }
  return false;
}

EXTENSION_NOINLINE
bool ST_Contains_Polygon_LineString(int8_t* poly_coords,
                                    int64_t poly_coords_size,
                                    int32_t* poly_ring_sizes,
                                    int64_t poly_num_rings,
                                    int8_t* l,
                                    int64_t lsize,
                                    int32_t li,
                                    int32_t ic1,
                                    int32_t isr1,
                                    int32_t ic2,
                                    int32_t isr2,
                                    int32_t osr) {
  if (poly_num_rings > 0)
    return false;  // TODO: support polygons with interior rings
  int32_t poly_num_coords = poly_coords_size / compression_unit_size(ic1);
  int32_t l_num_coords = lsize / compression_unit_size(ic2);
  for (int64_t i = 0; i < l_num_coords; i += 2) {
    // Check if polygon contains each point in the LineString
    double lx = coord_x(l, i, ic2, isr2, osr);
    double ly = coord_y(l, i + 1, ic2, isr2, osr);
    if (!contains_polygon_point(poly_coords, poly_num_coords, lx, ly, ic1, isr1, osr))
      return false;
  }
  return true;
}

EXTENSION_NOINLINE
bool ST_Contains_Polygon_Polygon(int8_t* poly1_coords,
                                 int64_t poly1_coords_size,
                                 int32_t* poly1_ring_sizes,
                                 int64_t poly1_num_rings,
                                 int8_t* poly2_coords,
                                 int64_t poly2_coords_size,
                                 int32_t* poly2_ring_sizes,
                                 int64_t poly2_num_rings,
                                 int32_t ic1,
                                 int32_t isr1,
                                 int32_t ic2,
                                 int32_t isr2,
                                 int32_t osr) {
  // TODO: needs to be extended, cover more cases
  // Right now only checking if simple poly1 (no holes) contains poly2's exterior shape
  if (poly1_num_rings > 0)
    return false;
  int64_t poly2_exterior_ring_coords_size = poly2_coords_size;
  if (poly2_num_rings > 0)
    poly2_exterior_ring_coords_size = 2 * poly2_ring_sizes[0] * compression_unit_size(ic2);
  return ST_Contains_Polygon_LineString(poly1_coords,
                                        poly1_coords_size,
                                        poly1_ring_sizes,
                                        poly1_num_rings,
                                        poly2_coords,
                                        poly2_exterior_ring_coords_size,
                                        0,
                                        ic1,
                                        isr1,
                                        ic2,
                                        isr2,
                                        osr);
}

EXTENSION_NOINLINE
bool ST_Contains_MultiPolygon_Point(int8_t* mpoly_coords,
                                    int64_t mpoly_coords_size,
                                    int32_t* mpoly_ring_sizes,
                                    int64_t mpoly_num_rings,
                                    int32_t* mpoly_poly_sizes,
                                    int64_t mpoly_num_polys,
                                    int8_t* p,
                                    int64_t psize,
                                    int32_t ic1,
                                    int32_t isr1,
                                    int32_t ic2,
                                    int32_t isr2,
                                    int32_t osr) {
  if (mpoly_num_polys <= 0)
    return false;

  // Set specific poly pointers as we move through the coords/ringsizes/polyrings arrays.
  auto next_poly_coords = mpoly_coords;
  auto next_poly_ring_sizes = mpoly_ring_sizes;

  for (auto poly = 0; poly < mpoly_num_polys; poly++) {
    auto poly_coords = next_poly_coords;
    auto poly_ring_sizes = next_poly_ring_sizes;
    auto poly_num_rings = mpoly_poly_sizes[poly];
    // Count number of coords in all of poly's rings, advance ring size pointer.
    int32_t poly_num_coords = 0;
    for (auto ring = 0; ring < poly_num_rings; ring++) {
      poly_num_coords += 2 * *next_poly_ring_sizes++;
    }
    auto poly_coords_size = poly_num_coords * compression_unit_size(ic1);
    next_poly_coords += poly_coords_size;
    if (ST_Contains_Polygon_Point(
            poly_coords, poly_coords_size, poly_ring_sizes, poly_num_rings, p, psize, ic1, isr1, ic2, isr2, osr)) {
      return true;
    }
  }

  return false;
}
