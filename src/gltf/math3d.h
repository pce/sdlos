#pragma once
// Column-major Mat4 for SDL3/Metal (right-handed, clip z ∈ [0,1])
// Storage: data[col*4 + row]

#include <array>
#include <cmath>
#include <utility>

namespace pce::sdlos::math3d {

struct Vec3 {
    float x{}, y{}, z{};

    constexpr Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    constexpr Vec3 operator*(float s)       const { return {x*s,   y*s,   z*s};   }
    constexpr Vec3 operator-()              const { return {-x,    -y,    -z};     }

    constexpr Vec3& operator+=(const Vec3& o) { x+=o.x; y+=o.y; z+=o.z; return *this; }
    constexpr Vec3& operator-=(const Vec3& o) { x-=o.x; y-=o.y; z-=o.z; return *this; }
    constexpr Vec3& operator*=(float s)       { x*=s;   y*=s;   z*=s;   return *this; }
};

constexpr Vec3 operator*(float s, const Vec3& v) { return v * s; }

inline float dot(Vec3 a, Vec3 b)  { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float length(Vec3 v)       { return std::sqrt(dot(v, v)); }
inline Vec3  normalize(Vec3 v)    { const float l = length(v); return l > 0.f ? v*(1.f/l) : Vec3{}; }
inline Vec3  cross(Vec3 a, Vec3 b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}

struct Vec4 {
    float x{}, y{}, z{}, w{};

    constexpr Vec4 operator+(const Vec4& o) const { return {x+o.x, y+o.y, z+o.z, w+o.w}; }
    constexpr Vec4 operator-(const Vec4& o) const { return {x-o.x, y-o.y, z-o.z, w-o.w}; }
    constexpr Vec4 operator*(float s)       const { return {x*s,   y*s,   z*s,   w*s};   }
    constexpr Vec4 operator-()              const { return {-x,    -y,    -z,    -w};     }
};

constexpr Vec4 operator*(float s, const Vec4& v) { return v * s; }

struct Mat4 {
    std::array<float, 16> data{};

    float  operator()(std::size_t row, std::size_t col) const { return data[col*4 + row]; }
    float& operator()(std::size_t row, std::size_t col)       { return data[col*4 + row]; }

    static Mat4 identity() {
        Mat4 m{};
        m(0,0) = m(1,1) = m(2,2) = m(3,3) = 1.f;
        return m;
    }

    Mat4 operator*(const Mat4& b) const {
        Mat4 c{};
        for (std::size_t col = 0; col < 4; ++col)
            for (std::size_t row = 0; row < 4; ++row) {
                float s = 0.f;
                for (std::size_t k = 0; k < 4; ++k) s += (*this)(row,k) * b(k,col);
                c(row,col) = s;
            }
        return c;
    }

    Vec4 operator*(Vec4 v) const {
        return {
            (*this)(0,0)*v.x + (*this)(0,1)*v.y + (*this)(0,2)*v.z + (*this)(0,3)*v.w,
            (*this)(1,0)*v.x + (*this)(1,1)*v.y + (*this)(1,2)*v.z + (*this)(1,3)*v.w,
            (*this)(2,0)*v.x + (*this)(2,1)*v.y + (*this)(2,2)*v.z + (*this)(2,3)*v.w,
            (*this)(3,0)*v.x + (*this)(3,1)*v.y + (*this)(3,2)*v.z + (*this)(3,3)*v.w,
        };
    }
};

// Right-handed lookAt, -Z forward (glTF convention).
// f = forward, s = right, u = recomputed up.
inline Mat4 lookAt(Vec3 eye, Vec3 center, Vec3 up = {0.f, 1.f, 0.f}) {
    const Vec3 f = normalize(center - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);
    Mat4 m{};
    m(0,0) =  s.x;  m(0,1) =  s.y;  m(0,2) =  s.z;  m(0,3) = -dot(s, eye);
    m(1,0) =  u.x;  m(1,1) =  u.y;  m(1,2) =  u.z;  m(1,3) = -dot(u, eye);
    m(2,0) = -f.x;  m(2,1) = -f.y;  m(2,2) = -f.z;  m(2,3) =  dot(f, eye);
    m(3,3) = 1.f;
    return m;
}

// Metal clip z ∈ [0,1]: m(2,2) = -far/(far-near), m(3,2) = -1.
inline Mat4 perspective(float fov_y_rad, float aspect, float near_z, float far_z) {
    const float f   = 1.f / std::tan(fov_y_rad * 0.5f);
    const float rng = far_z - near_z;
    Mat4 m{};
    m(0,0) =  f / aspect;
    m(1,1) =  f;
    m(2,2) = -far_z / rng;
    m(2,3) = -(near_z * far_z) / rng;
    m(3,2) = -1.f;
    return m;
}

// Metal clip z ∈ [0,1].
inline Mat4 orthographic(float l, float r, float b, float t, float near_z, float far_z) {
    const float rml = r-l, tmb = t-b, fmn = far_z-near_z;
    Mat4 m{};
    m(0,0) =  2.f/rml;  m(0,3) = -(r+l)/rml;
    m(1,1) =  2.f/tmb;  m(1,3) = -(t+b)/tmb;
    m(2,2) = -1.f/fmn;  m(2,3) = -near_z/fmn;
    m(3,3) =  1.f;
    return m;
}

inline Mat4 translate(Vec3 t) {
    Mat4 m = Mat4::identity();
    m(0,3) = t.x; m(1,3) = t.y; m(2,3) = t.z;
    return m;
}

inline Mat4 scale(Vec3 s) {
    Mat4 m = Mat4::identity();
    m(0,0) = s.x; m(1,1) = s.y; m(2,2) = s.z;
    return m;
}

// Unit quaternion (x,y,z,w) → rotation matrix.
inline Mat4 fromQuat(float qx, float qy, float qz, float qw) {
    const float x2=qx*qx, y2=qy*qy, z2=qz*qz;
    const float xy=qx*qy, xz=qx*qz, yz=qy*qz;
    const float wx=qw*qx, wy=qw*qy, wz=qw*qz;
    Mat4 m{};
    m(0,0) = 1.f-2.f*(y2+z2); m(0,1) =     2.f*(xy-wz); m(0,2) =     2.f*(xz+wy);
    m(1,0) =     2.f*(xy+wz); m(1,1) = 1.f-2.f*(x2+z2); m(1,2) =     2.f*(yz-wx);
    m(2,0) =     2.f*(xz-wy); m(2,1) =     2.f*(yz+wx); m(2,2) = 1.f-2.f*(x2+y2);
    m(3,3) = 1.f;
    return m;
}

// Euler rotation helpers — angle in radians, right-handed.
// Compose as T * Rz * Ry * Rx * S for the standard TRS order.

inline Mat4 rotateX(float rad) {
    const float c = std::cos(rad), s = std::sin(rad);
    Mat4 m = Mat4::identity();
    m(1,1) =  c;  m(1,2) = -s;
    m(2,1) =  s;  m(2,2) =  c;
    return m;
}

inline Mat4 rotateY(float rad) {
    const float c = std::cos(rad), s = std::sin(rad);
    Mat4 m = Mat4::identity();
    m(0,0) =  c;  m(0,2) =  s;
    m(2,0) = -s;  m(2,2) =  c;
    return m;
}

inline Mat4 rotateZ(float rad) {
    const float c = std::cos(rad), s = std::sin(rad);
    Mat4 m = Mat4::identity();
    m(0,0) =  c;  m(0,1) = -s;
    m(1,0) =  s;  m(1,1) =  c;
    return m;
}

// Returns {-1,-1} if the point is behind the camera (w ≤ 0).
// Flips Y: NDC +1 → screen y = 0.
inline std::pair<float,float> projectToScreen(const Mat4& mvp, Vec3 world,
                                               float vw, float vh) {
    const Vec4 clip = mvp * Vec4{world.x, world.y, world.z, 1.f};
    if (clip.w <= 0.f) return {-1.f, -1.f};
    const float inv_w = 1.f / clip.w;
    return {
        ( clip.x * inv_w + 1.f) * 0.5f * vw,
        (-clip.y * inv_w + 1.f) * 0.5f * vh
    };
}

} // namespace pce::sdlos::math3d
