// Minimal opencv2/core.hpp stub for compiling Odyssey3DSDK against the
// LeiaSR SDK headers. We only use SR::IDX12Weaver1 + SR::SRContext, never
// instantiate SR::Transformation (which actually uses cv::Mat by value),
// but the compiler still needs to parse Transformation's class body when
// walking srcontext.h -> sense.h -> transformation.h.
//
// Giving cv::Mat a conservative byte size matches real opencv 3.4.3 cv::Mat
// (header ~96 bytes on x64) so even if a Transformation ends up as a
// member of a type we touch, the overall layout computes.
#pragma once
#include <stdint.h>

namespace cv {

class Mat {
    uint8_t _opaque[96];
};

template<typename T, int M, int N>
class Matx {
    T val[M * N];
};

template<typename T, int N> using Vec = Matx<T, N, 1>;

using Matx33f = Matx<float, 3, 3>;
using Matx44f = Matx<float, 4, 4>;
using Matx33d = Matx<double, 3, 3>;
using Matx44d = Matx<double, 4, 4>;
using Matx13d = Matx<double, 1, 3>;
using Matx31d = Matx<double, 3, 1>;
using Matx14d = Matx<double, 1, 4>;
using Matx41d = Matx<double, 4, 1>;
using Matx13f = Matx<float, 1, 3>;
using Matx31f = Matx<float, 3, 1>;
using Matx14f = Matx<float, 1, 4>;
using Matx41f = Matx<float, 4, 1>;
using Matx22f = Matx<float, 2, 2>;
using Matx22d = Matx<double, 2, 2>;
using Vec3f   = Vec<float, 3>;
using Vec4f   = Vec<float, 4>;
using Vec3d   = Vec<double, 3>;
using Vec4d   = Vec<double, 4>;

class Scalar {
    double val[4];
};

template<typename T> class Point_ { public: T x, y; };
using Point2f = Point_<float>;
using Point2d = Point_<double>;

template<typename T> class Point3_ { public: T x, y, z; };
using Point3f = Point3_<float>;
using Point3d = Point3_<double>;

template<typename T> class Size_ { public: T width, height; };
using Size   = Size_<int>;
using Size2f = Size_<float>;

template<typename T> class Rect_ { public: T x, y, width, height; };
using Rect = Rect_<int>;

}  // namespace cv
