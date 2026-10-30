// Minimal stub -- same purpose as our opencv2/core.hpp. The LeiaSR SDK's
// Transformation class includes <opencv2/opencv.hpp>; we just forward
// to our core.hpp stub which provides enough of cv::Mat / cv::Matx for
// the compiler to parse SR headers. We never call opencv code.
#pragma once
#include "opencv2/core.hpp"
