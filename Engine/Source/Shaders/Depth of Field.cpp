/******************************************************************************/
#include "!Header.h"
#ifndef RANGE
#define RANGE 0
#endif
#ifndef HALF_RES
#define HALF_RES 0
#endif
#ifndef CLAMP
#define CLAMP 0
#endif
/******************************************************************************
TODO: add slow but high quality circular bokeh DoF
   -create small / quarter res (or maybe even smaller) RT containing info about biggest blur radius
   -in the blur function iterate "radius x radius" using BRANCH/LOOP and the small RT image (perhaps need to use 'SamplerPoint')
   -sample intensity should be based on Length2(sample_distance)<=Sqr(sample_range) (making bool true/false)
   -probably should divide this by sample area "intensity / Sqr(sample_range)" (samples covering more areas are stretched and their intensity spreaded?)
/******************************************************************************/
#define SHOW_BLUR        0
#define SHOW_SMOOTH_BLUR 0
#define SHOW_BLURRED     0

#define FRONT_EXTEND 0 // 0 or 1, method for extending the front, default=0
#define DEPTH_TOLERANCE (FRONT_EXTEND ? 1.5 : 1.0) // 1..2 are reasonable
#define FINAL_MODE  1 // 1(default)=maximize smooth blur only if it's closer, 0=always maximize smooth blur
#define FINAL_SCALE 4.0 // final blur scale, increases transition between sharp and blurred in 0..1/FINAL_SCALE step, instead of 0..1
/******************************************************************************/
BUFFER(Dof)
   Vec4 DofParams; // Intensity, Focus, SimpleMulAdd
BUFFER_END

inline Flt DofIntensity() {return DofParams.x;}
inline Flt DofFocus    () {return DofParams.y;}
inline Flt DofMul      () {return DofParams.z;}
inline Flt DofAdd      () {return DofParams.w;}
/******************************************************************************/
inline Flt Blur(Flt z)
{
#if REALISTIC
   #if 0 // F makes almost no difference
      Flt F=0.075; return DofIntensity() /* * F */ * (z - DofFocus()) / (z * (DofFocus() - F));
   #else
      return DofIntensity()*(z-DofFocus())/(z*DofFocus()); // 'DofRange' ignored
   #endif
#else
   return DofIntensity()*Mid(z*DofMul() + DofAdd(), -1, 1); // (z-DofFocus())/DofRange, z/DofRange - DofFocus()/DofRange
#endif
}
/******************************************************************************/
// CLAMP, REALISTIC, HALF_RES, GATHER
VecH4 DofDS_PS(NOPERSP Vec2 inTex:TEXCOORD):TARGET
{
   VecH4 ret; // RGB=col, W=Blur
   Flt   depth;
   if(HALF_RES)
   {
      ret.rgb=TexLod(Img, UVClamp(inTex, CLAMP)).rgb; // use linear filtering because we're downsampling
   #if GATHER // gather available since SM_4_1, GL 4.0, GL ES 3.1
      depth=DEPTH_MIN(TexDepthGather(inTex));
   #else
      Vec2 tex_min=inTex-ImgSize.xy*0.5,
           tex_max=inTex+ImgSize.xy*0.5;
      depth=DEPTH_MIN(TexDepthRawPoint(Vec2(tex_min.x, tex_min.y)),
                      TexDepthRawPoint(Vec2(tex_max.x, tex_min.y)),
                      TexDepthRawPoint(Vec2(tex_min.x, tex_max.y)),
                      TexDepthRawPoint(Vec2(tex_max.x, tex_max.y)));
   #endif
   }else // quarter
   {
      Vec2 tex_min=UVClamp(inTex-ImgSize.xy, CLAMP),
           tex_max=UVClamp(inTex+ImgSize.xy, CLAMP);
      Vec2 t00=Vec2(tex_min.x, tex_min.y),
           t10=Vec2(tex_max.x, tex_min.y),
           t01=Vec2(tex_min.x, tex_max.y),
           t11=Vec2(tex_max.x, tex_max.y);
      // use linear filtering because we're downsampling
      ret.rgb=(TexLod(Img, t00).rgb
              +TexLod(Img, t10).rgb
              +TexLod(Img, t01).rgb
              +TexLod(Img, t11).rgb)/4;
   #if GATHER // gather available since SM_4_1, GL 4.0, GL ES 3.1
      depth=DEPTH_MIN(DEPTH_MIN(TexDepthGather(t00)),
                      DEPTH_MIN(TexDepthGather(t10)),
                      DEPTH_MIN(TexDepthGather(t01)),
                      DEPTH_MIN(TexDepthGather(t11)));
   #else
      // this is approximation because we would have to take 16 samples
      depth=DEPTH_MIN(TexDepthRawPoint(t00),
                      TexDepthRawPoint(t10),
                      TexDepthRawPoint(t01),
                      TexDepthRawPoint(t11));
   #endif
   }
   ret.w=Blur(LinearizeDepth(depth))*0.5+0.5;
   return ret;
}
/******************************************************************************/
inline Flt Center(Flt center_blur_u) // center_blur_u=0..1 (0.5=focus) here we can offload some of the calculation done for each sample, by making necessary adjustments to 'center_blur_u'
{
   return center_blur_u*2-1;
}
inline Flt Weight(Flt center_blur, Flt test_blur_u, Int dist, Int range) // center_blur=-1..1 (0=focus), center_blur_u=0..1 (0.5=focus), test_blur_u=0..1 (0.5=focus)
{
   Flt f=dist/Flt(range+1),
     //center_blur=center_blur_u*2-1, // -1..1, we've already done this in 'Center'
         test_blur=  test_blur_u*2-1, // -1..1
      cb=Abs(center_blur), // 0..1
      tb=Abs(  test_blur), // 0..1
       b=Max(tb, cb // have to extend search radius for cases when center is in the front
        *Sat(FRONT_EXTEND ? (tb-center_blur)*DEPTH_TOLERANCE+1 : -center_blur*DEPTH_TOLERANCE)) // skip if: test focused and center in the back, or apply if: center in front
        *Sat((cb-test_blur)*DEPTH_TOLERANCE+1); // skip if: center focused and test in the back

   if(!b)return 0; // this check is needed only for low precision devices, or when using high precision RT's. Low precision unsigned RT's don't have exact zero, however we scale 'b' above and zero could be reached?

   Flt x=f/b; // NaN
   x=Sat(x); // x=Min(x, 1); to prevent for returning 'LerpCube' values outside 0..1 range
   return (1-LerpCube(x))/b; // weight, divide by 'b' to make narrower ranges more intense to preserve total intensity
   // !! if changing from 'LerpCube' to another function then we need to change 'WeightSum' as well !!
}
inline Flt FinalBlur(Flt blur, Flt blur_smooth) // 'blur'=-Inf .. Inf, 'blur_smooth'=0..1
{
   if(SHOW_BLURRED)return 1;
 //blur_smooth=(FINAL_MODE ? Sat(blur_smooth*-2+1) : Abs(blur_smooth*2-1)); already done in 'DofBlurY_PS'
   return Sat(Max(Abs(blur), blur_smooth)*FINAL_SCALE);
}
inline Flt WeightSum(Int range) {return range+1;} // Sum of all weights for all "-range..range" steps, calculated using "Flt weight=0; for(Int dist=-range; dist<=range; dist++)weight+=BlendSmoothCube(dist/Flt(range+1));"
/******************************************************************************/
// can use 'RTSize' instead of 'ImgSize' since there's no scale
// Use HighPrec because we operate on lot of samples
// RANGE

#define SCALE 0.5 // at the end we need 0 .. 0.5 range, and since we start with 0..1 we need to scale by "0.5"
VecH4 DofBlurX_PS(NOPERSP Vec2 inTex:TEXCOORD):TARGET
{  //  INPUT: Img: RGB         , Blur
   // OUTPUT:      RGB BlurredX, BlurSmooth

   Vec4 center=TexPoint(Img, inTex);
   Flt  center_blur=Center(center.a),
        weight=0,
        blur_abs=0;
   Vec4 color =0;
   Vec2 t; t.y=inTex.y;
   UNROLL for(Int i=-RANGE; i<=RANGE; i++)if(i)
   {
      t.x=inTex.x+RTSize.x*i;
      Vec4 c=TexPoint(Img, t);
      Flt  test_blur=c.a,
           w=Weight(center_blur, test_blur, Abs(i), RANGE);
      weight  +=w;
      color   +=w*    c;
      blur_abs+=w*Abs(c.a * (2*SCALE) - (1*SCALE)); // SCALE here so we don't have to do it later
   }
   Flt b=Abs(center_blur),
       w=Lerp(WeightSum(RANGE)-weight, 1, b);
   color   +=w*    center;
   blur_abs+=w*Abs(center.a * (2*SCALE) - (1*SCALE)); // SCALE here so we don't have to do it later
   weight  +=w;

   blur_abs/=weight;
 //return VecH4(color.rgb/weight, color.a/weight);
   return VecH4(color.rgb/weight, (color.a>=0.5*weight) ? 0.5+blur_abs : 0.5-blur_abs); // color.a/weight>=0.5 ? .. : ..
}
#undef  SCALE
#define SCALE 1.0 // at the end we need 0..1 range, and since we start with 0..1 we need to scale by "1"
VecH4 DofBlurY_PS(NOPERSP Vec2 inTex:TEXCOORD):TARGET
{  //  INPUT: Img: RGB BlurredX , BlurSmooth
   // OUTPUT:      RGB BlurredXY, BlurSmooth

   Vec4 center=TexPoint(Img, inTex);
   Flt  center_blur=Center(center.a),
        weight=0,
        blur_abs=0;
   Vec4 color =0;
   Vec2 t; t.x=inTex.x;
   UNROLL for(Int i=-RANGE; i<=RANGE; i++)if(i)
   {
      t.y=inTex.y+RTSize.y*i;
      Vec4 c=TexPoint(Img, t);
      Flt  test_blur=c.a,
           w=Weight(center_blur, test_blur, Abs(i), RANGE);
      weight  +=w;
      color   +=w*    c;
      blur_abs+=w*Abs(c.a * (2*SCALE) - (1*SCALE)); // SCALE here so we don't have to do it later
   }
   Flt b=Abs(center_blur),
       w=Lerp(WeightSum(RANGE)-weight, 1, b);
   color   +=w*    center;
   blur_abs+=w*Abs(center.a * (2*SCALE) - (1*SCALE)); // SCALE here so we don't have to do it later
   weight  +=w;

   blur_abs/=weight;
 //color.a  =((color.a>=0.5*weight) ? 0.5+blur_abs : 0.5-blur_abs); // color.a/weight>=0.5 ? .. : ..
   return VecH4(color.rgb/weight, FINAL_MODE ? ((color.a>=0.5*weight) ? 0 : blur_abs) : blur_abs);
}
/******************************************************************************/
// DITHER, REALISTIC
VecH4 Dof_PS(NOPERSP Vec2 inTex:TEXCOORD,
             NOPERSP PIXEL              ):TARGET
{
   Flt z=TexDepthPoint(inTex),
       b=Blur(z);
#if SHOW_BLUR
   b=1-Abs(b); return Vec4(b, b, b, 1);
#endif
   VecH4 focus=TexLod(Img , inTex), // can't use 'TexPoint' because 'Img' can be supersampled
         blur =TexLod(Img1, inTex), // use linear filtering because 'Img1' may be smaller RT
         col;
     #if SHOW_SMOOTH_BLUR
        col.rgb=blur.a;
     #else
        col.rgb=Lerp(focus.rgb, blur.rgb, FinalBlur(b, blur.a));
     #endif
        col.a=1; // force full alpha so back buffer effects can work ok
#if DITHER
   ApplyDither(col.rgb, pixel.xy);
#endif
   return col;
}
/******************************************************************************/
