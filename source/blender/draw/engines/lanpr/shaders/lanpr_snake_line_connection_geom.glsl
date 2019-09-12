layout(lines_adjacency) in;
layout(triangle_strip, max_vertices = 6) out;

in vec2 gOffset[];

uniform float line_width;
uniform float taper_l_dist;
uniform float taper_r_dist;
uniform float taper_l_strength;
uniform float taper_r_strength;

#define M_PI 3.1415926535897932384626433832795

vec4 MakeLeftTaperLinear(vec4 L, vec4 a, float offset)
{
  if (offset >= taper_l_dist)
    return a;
  a = mix(mix(a, L, taper_l_strength), a, offset / taper_l_dist);
  return a;
}

vec4 MakeRightTaperLinear(vec4 R, vec4 c, float offset)
{
  if (offset >= taper_r_dist)
    return c;
  c = mix(mix(c, R, taper_r_strength), c, offset / taper_r_dist);
  return c;
}

void main()
{

  float LAngle, RAngle;

  vec4 LL = gl_in[0].gl_Position, L = gl_in[1].gl_Position, R = gl_in[2].gl_Position,
       RR = gl_in[3].gl_Position;

  float OffsetL = gOffset[1].x;
  float OffsetR = gOffset[2].x;
  float OffsetL2 = gOffset[1].y;
  float OffsetR2 = gOffset[2].y;

  if (L == R || L == LL || R == RR || LL == RR || L == RR || R == LL)
    return;

  vec4 a;
  vec4 b;
  vec4 c;
  vec4 d;
  vec4 Line = R - L;
  vec4 Normal = normalize(vec4(-Line.y, Line.x, 0, 0));

  a = L - line_width * Normal * 0.001;
  b = L + line_width * Normal * 0.001;
  c = R - line_width * Normal * 0.001;
  d = R + line_width * Normal * 0.001;

  float lim = line_width * 0.002;

  {
    vec4 Tangent = normalize(normalize(L - LL) + normalize(R - L));
    vec4 Minter = normalize(vec4(-Tangent.y, Tangent.x, 0, 0));
    float length = line_width / (dot(Minter, Normal)) * 0.001;
    a = L - length * Minter;
    b = L + length * Minter;
    if (distance(a, b) > 2 * lim) {
      a = L - lim * Minter;
      b = L + lim * Minter;
    }
  }

  {
    vec4 Tangent = normalize(normalize(RR - R) + normalize(R - L));
    vec4 Minter = normalize(vec4(-Tangent.y, Tangent.x, 0, 0));
    float length = line_width / (dot(Minter, Normal)) * 0.001;
    c = R - length * Minter;
    d = R + length * Minter;
    if (distance(c, d) > 2 * lim) {
      c = R - lim * Minter;
      d = R + lim * Minter;
    }
  }

  a = MakeLeftTaperLinear(L, a, OffsetL);
  b = MakeLeftTaperLinear(L, b, OffsetL);
  c = MakeLeftTaperLinear(R, c, OffsetR);
  d = MakeLeftTaperLinear(R, d, OffsetR);

  a = MakeRightTaperLinear(L, a, OffsetL2);
  b = MakeRightTaperLinear(L, b, OffsetL2);
  c = MakeRightTaperLinear(R, c, OffsetR2);
  d = MakeRightTaperLinear(R, d, OffsetR2);

  a.w = 1;
  b.w = 1;
  c.w = 1;
  d.w = 1;

  gl_Position = a;
  EmitVertex();
  gl_Position = b;
  EmitVertex();
  gl_Position = c;
  EmitVertex();
  EndPrimitive();

  gl_Position = c;
  EmitVertex();
  gl_Position = d;
  EmitVertex();
  gl_Position = b;
  EmitVertex();
  EndPrimitive();
}
