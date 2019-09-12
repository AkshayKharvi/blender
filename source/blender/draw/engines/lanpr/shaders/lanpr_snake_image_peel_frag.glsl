
in vec4 uvcoordsvar;
uniform sampler2D tex_sample_0;
uniform int stage;

int decisions[256] = int[](0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           1,
                           0,
                           1,
                           1,
                           1,
                           0,
                           1,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           1,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           1,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           1,
                           0,
                           1,
                           1,
                           1,
                           0,
                           1,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           1,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           1,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           0,
                           1,
                           1,
                           1,
                           0,
                           1,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           1,
                           0,
                           1,
                           1,
                           1,
                           0,
                           1,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           1,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           1,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           1,
                           0,
                           1,
                           1,
                           1,
                           0,
                           1,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           1,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           1,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           0,
                           1,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           1,
                           1,
                           0,
                           1,
                           1,
                           0,
                           0,
                           1,
                           0,
                           0,
                           0);

int PickPixel(ivec2 sp)
{
  vec4 accum = vec4(0);
  // for(int i=0;i<4;i++){
  accum += texelFetch(tex_sample_0, sp, 0);
  //}
  return (accum.r > 0.9) ? 1 : 0;
}

// MZS Thinning method, implemented by YimingWu

void main()
{

  ivec2 texSize = textureSize(tex_sample_0, 0);
  ivec2 sp = ivec2(uvcoordsvar.xy * texSize);
  vec4 OriginalColor = texelFetch(tex_sample_0, sp, 0);

  int p2 = PickPixel(sp + ivec2(0, +1));
  int p3 = PickPixel(sp + ivec2(+1, +1));
  int p4 = PickPixel(sp + ivec2(+1, 0));
  int p5 = PickPixel(sp + ivec2(+1, -1));
  int p6 = PickPixel(sp + ivec2(0, -1));
  int p7 = PickPixel(sp + ivec2(-1, -1));
  int p8 = PickPixel(sp + ivec2(-1, 0));
  int p9 = PickPixel(sp + ivec2(-1, +1));

  int Bp1 = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;

  bool bp2 = bool(p2);
  bool bp3 = bool(p3);
  bool bp4 = bool(p4);
  bool bp5 = bool(p5);
  bool bp6 = bool(p6);
  bool bp7 = bool(p7);
  bool bp8 = bool(p8);
  bool bp9 = bool(p9);

  int Cp1 = int(!bp2 && (bp3 || bp4)) + int(!bp4 && (bp5 || bp6)) + int(!bp6 && (bp7 || bp8)) +
            int(!bp8 && (bp9 || bp2));

  if (stage == 0) {
    if (((sp.x + sp.y) % 2 == 0) && (Cp1 == 1) && (Bp1 >= 2 && Bp1 <= 7) && (p2 * p4 * p6 == 0) &&
        (p4 * p6 * p8 == 0)) {
      gl_FragColor = vec4(0, 0, 0, 1);
      return;
    }
    gl_FragColor = OriginalColor;
  }
  else {
    if (((sp.x + sp.y) % 2 != 0) && (Cp1 == 1) && (Bp1 >= 1 && Bp1 <= 7) && (p2 * p4 * p8 == 0) &&
        (p2 * p6 * p8 == 0)) {
      gl_FragColor = vec4(0, 0, 0, 1);
      return;
    }
    gl_FragColor = OriginalColor;
  }

  // int test = PickPixel(sp+ivec2(-1,-1))*1 + PickPixel(sp+ivec2(0 ,-1))*2 +
  // PickPixel(sp+ivec2(+1,-1))*4
  //         + PickPixel(sp+ivec2(-1, 0))*8 + PickPixel(sp+ivec2(+1, 0))*16
  //		 + PickPixel(sp+ivec2(-1,+1))*32 + PickPixel(sp+ivec2( 0,+1))*64 +
  // PickPixel(sp+ivec2(+1,+1))*128;

  // if(decisions[test]==1) gl_FragColor=vec4(1,0,0,1);
  // else gl_FragColor=texelFetch(tex_sample_0, sp, 0);//vec4(1,1,1,1);
}
