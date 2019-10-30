uniform mat4 ModelMatrix;
uniform mat4 ViewMatrix;
uniform mat4 ViewMatrixInverse;
uniform mat4 ProjectionMatrix;
uniform mat4 ProjectionMatrixInverse;

uniform int use_contour;
uniform int use_crease;
uniform int use_material;
uniform int use_edge_mark;
uniform int use_intersection;

uniform float crease_threshold;
uniform float crease_fade_threshold;

uniform int is_perspective;  // persp and orth use different crease line determin method

// uniform float sample_step; // length calculation unused now.
// uniform int buffer_width;

uniform vec4 viewport;

uniform sampler2D tex_vert0;
uniform sampler2D tex_vert1;
uniform sampler2D tex_fnormal0;
uniform sampler2D tex_fnormal1;
uniform sampler2D tex_edge_mask;

// uniform sampler2D TexSample4;
//#define path_start_end_ptrs TexSample4 // edge adjacent data

// calculate in shader

vec3 view_pos;
vec3 view_dir;

int is_crease;  // we calculate crease in GPU because it's faster and we have normal data anyway.
                // and we need to indicate crease test success result using p1.w==1 && p2.w==0

float crease_strength;

// these are for adapting argument names...
#define modelview (ViewMatrix * ModelMatrix)
#define projection ProjectionMatrix
#define inverse_projection ProjectionMatrixInverse

// ivec2 getTexturePix(vec2 fb_coord){
//    vec2 n = ((fb_coord+vec2(1,1))/2).xy;
//    return ivec2(n.x*buffer_width,n.y*buffer_width);
//}

// Amount of padding around a segment in the segment atlas.
// The amount of padding rolls off to zero for short segments,
// and is zero for segments in the middle of paths.

vec2 segmentPadding(float num_samples, float index, float start_index, float end_index)
{
  const float MAX_PADDING = 10.0;

  float amount = floor(clamp((num_samples - 2.0) * 0.5, 0.0, MAX_PADDING));

  float left = amount * max(1.0 + start_index - index, 0.0);
  float right = amount * max(1.0 + index - end_index, 0.0);

  return vec2(left, right);
}

// Converting from linear indices to 2D coordinates and back:

float coordinateToIndex(vec2 coord, float buf_size)
{
  vec2 floor_coord = floor(coord);
  return floor_coord.x + floor_coord.y * buf_size;
}

vec2 indexToCoordinate(float index, float buf_size)
{
  return vec2(mod(index, buf_size), floor(index / buf_size));
}

// Packing and unpacking values in the segment atlas offset texture:

float unpackNumSamples(vec4 offset_texel)
{
  return offset_texel.b;
}

float unpackArcLength(vec4 offset_texel)
{
  return offset_texel.a;
}

float unpackSampleOffset(vec4 offset_texel)
{
  return offset_texel.r;
}

float unpackArcLengthOffset(vec4 offset_texel)
{
  return offset_texel.g;
}

vec4 packOffsetTexel(float num_samples, float arc_length)
{
  return vec4(num_samples, arc_length, num_samples, arc_length);
}

vec4 packOffsetTexel(float num_samples,
                     float arc_length,
                     float num_samples_offset,
                     float arc_length_offset)
{
  return vec4(num_samples_offset, arc_length_offset, num_samples, arc_length);
}

// Packing and unpacking values in the 3D vertex positions:

float unpackPathStart(vec4 texel)
{
  return texel.r;
}

float unpackPathEnd(vec4 texel)
{
  return texel.g;
}

float unpackPathLength(vec4 texel)
{
  return texel.b;
}

// Projecting and unprojecting:

vec2 clipToWindow(sampler2D clip_positions, vec4 viewport, ivec2 coordinate)
{
  vec4 clip = texelFetch(clip_positions, coordinate, 0);
  vec3 post_div = clip.xyz / clip.w;
  return (post_div.xy + vec2(1.0, 1.0)) * 0.5 * viewport.zw;
}

vec2 clipToWindow(vec4 clip, vec4 viewport)
{
  vec3 post_div = clip.xyz / clip.w;
  return (post_div.xy + vec2(1.0, 1.0)) * 0.5 * viewport.zw;
}

// Path id encoding and decoding.

bool idEqualGreaterThan(vec3 a, vec3 b)
{
  float ida = a.b * 256.0 * 256.0 + a.g * 256.0 + a.r;
  float idb = b.b * 256.0 * 256.0 + b.g * 256.0 + b.r;
  const float small = 0.001;
  return ida - idb > -small;
}

bool idsEqual(vec3 a, vec3 b)
{
  float ida = a.b * 256.0 * 256.0 + a.g * 256.0 + a.r;
  float idb = b.b * 256.0 * 256.0 + b.g * 256.0 + b.r;
  const float small = 0.001;
  return abs(ida - idb) < small;
}

vec3 idToColor(float id)
{
  id = id + 1.0;
  float blue = floor(id / (256.0 * 256.0));
  float green = floor(id / 256.0) - blue * 256.0;
  float red = id - green * 256.0 - blue * 256.0 * 256.0;
  return vec3(red, green, blue) / 255.0;
}

struct segment {
  vec3 p1;
  vec3 p2;
  bool on_screen;
};

float epsilon = 0.00001;
float xmin = -1.1;
float xmax = 1.1;
float ymin = -1.1;
float ymax = 1.1;

// this is a conservative offscreen rejection test ... catches most cases
bool segmentOffScreen(vec3 p0, vec3 p1)
{
  return ((p0[0] < xmin && p1[0] < xmin) || (p0[0] > xmax && p1[0] > xmax) ||
          (p0[1] < ymin && p1[1] < ymin) || (p0[1] > ymax && p1[1] > ymax));
}

bool pointOffScreen(vec3 p)
{
  return (p[0] < xmin || p[0] > xmax || p[1] < ymin || p[1] > ymax);
}

vec3 clipMinMaxX(vec3 outv, vec3 inv)
{
  vec3 ret = outv;
  if (outv.x < xmin) {
    float t = (xmin - outv.x) / (inv.x - outv.x);
    ret = t * inv + (1.0 - t) * outv;
  }
  else if (outv.x > xmax) {
    float t = (xmax - inv.x) / (outv.x - inv.x);
    ret = t * outv + (1.0 - t) * inv;
  }
  return ret;
}

vec3 clipMinMaxY(vec3 outv, vec3 inv)
{
  vec3 ret = outv;
  if (outv.y < ymin) {
    float t = (ymin - outv.y) / (inv.y - outv.y);
    ret = t * inv + (1.0 - t) * outv;
  }
  else if (outv.y > ymax) {
    float t = (ymax - inv.y) / (outv.y - inv.y);
    ret = t * outv + (1.0 - t) * inv;
  }
  return ret;
}

vec3 clipSegmentOneOut(vec3 off_screen, vec3 on_screen)
{
  vec3 outv = off_screen;

  // first clip against the x coords
  outv = clipMinMaxX(outv, on_screen);

  // now clip against the y coords using the newly clipped point
  outv = clipMinMaxY(outv, on_screen);

  return outv;
}

segment clipToMin(float min, segment inseg, float p1val, float p2val)
{
  float minPos = min + epsilon;
  float minNeg = min - epsilon;
  segment outseg = segment(inseg.p1, inseg.p2, inseg.on_screen);

  // trivial reject
  if ((p1val < minPos && p2val < minPos) || inseg.on_screen == false) {
    outseg.on_screen = false;
  }

  // cut at min
  if (p1val < minPos) {
    float t = (min - p1val) / (p2val - p1val);
    outseg.p1 = t * inseg.p2 + (1.0 - t) * inseg.p1;
  }
  else if (p2val < minPos) {
    float t = (min - p2val) / (p1val - p2val);
    outseg.p2 = t * inseg.p1 + (1.0 - t) * inseg.p2;
  }
  return outseg;
}

segment clipToMax(float max, segment inseg, float p1val, float p2val)
{
  float maxPos = max + epsilon;
  float maxNeg = max - epsilon;
  segment outseg = segment(inseg.p1, inseg.p2, inseg.on_screen);

  // trivial reject
  if ((p1val > maxNeg && p2val > maxNeg) || inseg.on_screen == false) {
    outseg.on_screen = false;
  }

  // cut at max
  if (p1val > maxNeg) {
    float t = (max - p2val) / (p1val - p2val);
    outseg.p1 = t * inseg.p1 + (1.0 - t) * inseg.p2;
  }
  else if (p2val > maxNeg) {
    float t = (max - p1val) / (p2val - p1val);
    outseg.p2 = t * inseg.p2 + (1.0 - t) * inseg.p1;
  }
  return outseg;
}

segment clipSegmentBothOut(vec3 p1, vec3 p2)
{
  segment seg = segment(p1, p2, true);

  seg = clipToMin(xmin, seg, seg.p1.x, seg.p2.x);
  seg = clipToMax(xmax, seg, seg.p1.x, seg.p2.x);
  seg = clipToMin(ymin, seg, seg.p1.y, seg.p2.y);
  seg = clipToMax(ymax, seg, seg.p1.y, seg.p2.y);

  return seg;
}

vec3 clipSegmentToNear(vec3 off_screen, vec3 on_screen)
{
  // see http://members.tripod.com/~Paul_Kirby/vector/Vplanelineint.html

  vec3 a = off_screen;
  vec3 b = on_screen;
  vec3 c = view_pos + view_dir;
  vec3 n = view_dir;
  float t = dot((c - a), n) / dot((b - a), n);

  vec3 clipped = a + (b - a) * t;
  return clipped;
}

bool pointBeyondNear(vec3 p)
{
  vec3 offset = p - view_pos;
  bool beyond = dot(offset, view_dir) > 0.0;
  return beyond;
}

// 1 for contour 2 for others
int testProfileEdge(ivec2 texcoord, vec3 world_position)
{
  // This should really be the inverse transpose of the modelview matrix, but
  // that only matters if the camera has a weird anisotropic scale or skew.

  mat3 nm = mat3(transpose(inverse(ModelMatrix)));
  vec3 face_normal_0 = mat3(nm) * texelFetch(tex_fnormal0, texcoord, 0).xyz;
  vec3 face_normal_1 = mat3(nm) * texelFetch(tex_fnormal1, texcoord, 0).xyz;
  vec3 camera_to_line = is_perspective == 1 ? world_position - view_pos :
                                              view_dir;  // modelview * vec4(world_position, 1.0);

  vec4 edge_mask = texelFetch(tex_edge_mask, texcoord, 0);

  float dot0 = dot(camera_to_line.xyz, vec3(face_normal_0.xyz));
  float dot1 = dot(camera_to_line.xyz, vec3(face_normal_1.xyz));
  float dot2 = dot(normalize(vec3(face_normal_0.xyz)), normalize(vec3(face_normal_1.xyz)));

  bool contour = (dot0 >= 0.0 && dot1 <= 0.0) || (dot0 <= 0.0 && dot1 >= 0.0);

  is_crease = (((use_contour > 0 && !contour) || use_contour == 0) &&
               ((dot2 < crease_threshold) || (dot2 < crease_fade_threshold))) ?
                  1 :
                  0;

  crease_strength = (is_crease > 0 && dot2 > crease_threshold) ?
                        ((dot2 - crease_threshold) / (crease_fade_threshold - crease_threshold) /
                         2) :
                        0;
  // use 0 to 0.5 to repesent the range, because 1 will represent another meaning

  if (use_contour > 0 && contour)
    return 1;
  if (((use_crease > 0) && (is_crease > 0)) || ((use_material > 0) && (edge_mask.r > 0)) ||
      ((use_edge_mark > 0) && (edge_mask.g > 0)) ||
      ((use_intersection > 0) && (edge_mask.b > 0)) || false)
    return 2;
  return 0;
}

void main()
{

  view_dir = -mat3(ViewMatrixInverse) * vec3(0, 0, 1);
  view_pos = (ViewMatrixInverse)[3].xyz;

  xmin *= viewport.z / viewport.w;
  xmax *= viewport.z / viewport.w;

  // look up the world positions of the segment vertices
  ivec2 texcoord = ivec2(gl_FragCoord.xy);

  vec4 v0_world_pos = texelFetch(tex_vert0, texcoord, 0);
  vec4 v1_world_pos = texelFetch(tex_vert1, texcoord, 0);
  v0_world_pos = ModelMatrix * vec4(v0_world_pos.xyz, 1);
  v1_world_pos = ModelMatrix * vec4(v1_world_pos.xyz, 1);

  // early exit if there are no vertices here to process
  if (v0_world_pos.w < 0.5) {
    // no vertex data to process
    gl_FragData[0] = vec4(0.5, 0.0, 0.0, 0.0);
    gl_FragData[1] = vec4(0.5, 0.5, 0.0, 0.0);
    // must write something into fragdata[2] to prevent
    // buffer 2 from getting filled with garbage? (very weird)
    gl_FragData[2] = vec4(0.0, 1.0, 0.0, 0.0);
    return;
  }

  vec3 v0_clipped_near = v0_world_pos.xyz;
  vec3 v1_clipped_near = v1_world_pos.xyz;

  if (is_perspective == 1) {
    // clip to the near plane
    bool v0_beyond_near = pointBeyondNear(v0_world_pos.xyz);
    bool v1_beyond_near = pointBeyondNear(v1_world_pos.xyz);

    if (!v0_beyond_near && !v1_beyond_near) {
      // segment entirely behind the camera
      gl_FragData[0] = vec4(0.0, 1.0, 0.0, 0.0);
      gl_FragData[1] = vec4(0.0, 0.0, 1.0, 0.0);
      gl_FragData[2] = vec4(0.0, 1.0, 0.0, 0.0);
      return;
    }
    else if (!v0_beyond_near) {
      v0_clipped_near = clipSegmentToNear(v0_world_pos.xyz, v1_clipped_near);
    }
    else if (!v1_beyond_near) {
      v1_clipped_near = clipSegmentToNear(v1_world_pos.xyz, v0_clipped_near);
    }
  }

  // If this segment is a profile edge, test to see if it should be turned on.
  // if (v1_world_pos.w > 0.5)
  //{
  int profile_on = testProfileEdge(texcoord, v0_clipped_near);
  if (profile_on == 0) {
    // Profile edge should be off.
    gl_FragData[0] = vec4(0.0, 1.0, 0.5, 0.0);
    gl_FragData[1] = vec4(0.0, 0.5, 1.0, 0.0);
    gl_FragData[2] = vec4(0.0, 1.0, 0.0, 0.0);
    return;
  }
  //}

  // project
  vec4 v0_pre_div = projection * ViewMatrix * vec4(v0_clipped_near, 1.0);
  vec4 v1_pre_div = projection * ViewMatrix * vec4(v1_clipped_near, 1.0);

  // perspective divide
  vec3 v0_clip_pos = v0_pre_div.xyz;
  vec3 v1_clip_pos = v1_pre_div.xyz;
  if (is_perspective == 1) {
    v0_clip_pos /= v0_pre_div.w;
    v1_clip_pos /= v1_pre_div.w;
  }
  // clip to frustum
  bool v0_on_screen = !pointOffScreen(v0_clip_pos);
  bool v1_on_screen = !pointOffScreen(v1_clip_pos);

  if (!v0_on_screen && !v1_on_screen) {
    segment ret = clipSegmentBothOut(v0_clip_pos, v1_clip_pos);
    if (ret.on_screen == false) {
      // segment entirely off screen: BLUE / MAGENTA / BLACK
      gl_FragData[0] = vec4(0.0, 0.0, 1.0, 0.0);
      gl_FragData[1] = vec4(1.0, 0.0, 1.0, 0.0);
      gl_FragData[2] = vec4(0.0, 0.0, 0.0, 0.0);
      return;
    }
    v0_clip_pos = ret.p1;
    v1_clip_pos = ret.p2;
  }
  else if (!v0_on_screen) {
    v0_clip_pos = clipSegmentOneOut(v0_clip_pos, v1_clip_pos);
  }
  else if (!v1_on_screen) {
    v1_clip_pos = clipSegmentOneOut(v1_clip_pos, v0_clip_pos);
  }

  // convert to window coordinates
  vec2 v0_screen = (v0_clip_pos.xy + vec2(1.0, 1.0)) * 0.5 * viewport.zw;
  vec2 v1_screen = (v1_clip_pos.xy + vec2(1.0, 1.0)) * 0.5 * viewport.zw;

  // if(v1_screen == v0_screen){ gl_FragData[0] = vec4(1,0,0,1); return; }

  float segment_screen_length = length(v0_screen - v1_screen);

  // scale the length by sample_step to get the number of samples
  // float num_samples = segment_screen_length / sample_step;
  // num_samples = ceil(num_samples);

  // Unproject and reproject the final clipped positions
  // so that interpolation is perspective correct later on.
  vec4 v0_world = inverse_projection * vec4(v0_clip_pos, 1.0);
  vec4 v1_world = inverse_projection * vec4(v1_clip_pos, 1.0);
  vec4 v0_clipped_pre_div = projection * v0_world;
  vec4 v1_clipped_pre_div = projection * v1_world;

  // Add some padding to the number of samples so that filters
  // that work along the segment length (such as overshoot)
  // have some room to work with at the end of each segment.
  // vec4 path_texel = texelFetch(path_start_end_ptrs, texcoord,0);
  // vec2 padding = segmentPadding(num_samples,
  //    coordinateToIndex(texcoord, buffer_width),
  //    unpackPathStart(path_texel),
  //    unpackPathEnd(path_texel));
  // float total_padding = padding.x + padding.y;

  // if(v0_clipped_pre_div == v1_clipped_pre_div)gl_FragData[0] =vec4(1);
  // else gl_FragData[0] = vec4(v0_clipped_pre_div.xyz,1);

  gl_FragData[0] = vec4(v0_clipped_pre_div.xyz,
                        1);  // contour has priority, modification cause trouble
  gl_FragData[1] = vec4(v1_clipped_pre_div.xyz, is_crease > 0 ? crease_strength : 1);
  // gl_FragData[2] = packOffsetTexel(num_samples, segment_screen_length,
  // num_samples, segment_screen_length);
  // num_samples + total_padding, segment_screen_length);
}
